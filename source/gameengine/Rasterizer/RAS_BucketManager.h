/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file RAS_BucketManager.h
 *  \ingroup bgerast
 */

#ifndef __RAS_BUCKETMANAGER_H__
#define __RAS_BUCKETMANAGER_H__

#include "RAS_MaterialBucket.h"
#include "RAS_MeshSlot.h"

#include <ankerl/unordered_dense.h>
#include <chrono>
#include <vector>

class RAS_OffScreen;
class SCA_IScene;
class RAS_BatchGroup;
class RAS_MeshUser;

#define RAS_ENABLE_CPU_PROFILE 0

#if RAS_ENABLE_CPU_PROFILE
enum RAS_CpuProfileId
{
	RAS_CPU_BUCKETMANAGER_RENDERBUCKETS = 0,
	RAS_CPU_BUCKETMANAGER_PREPAREBUCKETS,
	RAS_CPU_BUCKETMANAGER_RENDERBASIC,
	RAS_CPU_BUCKETMANAGER_RENDERSORTED,
	RAS_CPU_BUCKETMANAGER_RENDERBASICCOMBINED,
	RAS_CPU_MATERIALBUCKET_GENERATETREE,
	RAS_CPU_DISPLAYARRAYBUCKET_RUNINSTANCINGNODE,
	RAS_CPU_DISPLAYARRAYBUCKET_INSTANCING_SORT,
	RAS_CPU_DISPLAYARRAYBUCKET_INSTANCING_CHECK_DIRTY,
	RAS_CPU_DISPLAYARRAYBUCKET_BINDATTRIBS,
	RAS_CPU_DISPLAYARRAYBUCKET_ACTIVATEINSTANCING,
	RAS_CPU_DISPLAYARRAYBUCKET_DRAWINSTANCED,
	RAS_CPU_DISPLAYARRAYBUCKET_RUNBATCHINGNODE,
	RAS_CPU_DISPLAYARRAYBUCKET_GENERATETREE,
	RAS_CPU_SCENE_CALCULATEVISIBLEMESHES,
	RAS_CPU_SCENE_CVM_CACHE_LOOKUP,
	RAS_CPU_SCENE_CVM_CACHE_APPLY,
	RAS_CPU_SCENE_CVM_SCAN_BB_MODIFIED,
	RAS_CPU_SCENE_CVM_UPDATEDBVT,
	RAS_CPU_SCENE_CVM_DBVT_CULLINGTEST,
	RAS_CPU_SCENE_CVM_FALLBACK_HANDLER,
	RAS_CPU_SCENE_CVM_CACHE_STORE,
	RAS_CPU_SCENE_CVM_CLEAR_BB_MODIFIED,
	RAS_CPU_SCENE_UPDATEDBVT_SET_CULLED,
	RAS_CPU_SCENE_UPDATEDBVT_UPDATE_BOUNDS,
	RAS_CPU_SCENE_UPDATEDBVT_SET_CULLED_ALL,
	RAS_CPU_SCENE_UPDATEOBJECTLODS,
	RAS_CPU_DISPLAYARRAYBUCKET_TRYAUTOBATCH,
	RAS_CPU_DISPLAYARRAYBUCKET_ACTIVATEMESH,
	RAS_CPU_DISPLAYARRAYBUCKET_UPDATEACTIVESLOTS,
	RAS_CPU_MATERIALBUCKET_ACTIVATE,
	RAS_CPU_ATTRIBUTEARRAYSTORAGE_BINDPRIMITIVES,
	RAS_CPU_ATTRIBUTEARRAYSTORAGE_UNBINDPRIMITIVES,
	RAS_CPU_INSTANCINGBUFFER_UPDATE,
	RAS_CPU_STORAGEVBO_DRAWINSTANCED,
	RAS_CPU_PROFILE_COUNT
};

void RAS_CpuProfile_Accumulate(RAS_CpuProfileId id, std::uint64_t micros);
void RAS_CpuProfile_EndFrameAndPrint();

class RAS_CpuProfileScope
{
public:
	RAS_CpuProfileScope(RAS_CpuProfileId id)
		:m_id(id),
		m_start(std::chrono::high_resolution_clock::now())
	{
	}

	~RAS_CpuProfileScope()
	{
		const auto end = std::chrono::high_resolution_clock::now();
		const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(end - m_start).count();
		RAS_CpuProfile_Accumulate(m_id, static_cast<std::uint64_t>(micros));
	}

private:
	RAS_CpuProfileId m_id;
	std::chrono::high_resolution_clock::time_point m_start;
};

#define RAS_CPU_PROFILE_SCOPE(ID) RAS_CpuProfileScope _rasCpuScope_##__LINE__(ID)
#else
#define RAS_CPU_PROFILE_SCOPE(ID) ((void)0)
#endif

class RAS_BucketManager : public mt::SimdClassAllocator
{
public:
	typedef std::vector<RAS_MaterialBucket *> BucketList;

	struct backtofront
	{
		bool operator()(const SortedMeshSlot &a, const SortedMeshSlot &b);
	};
	struct fronttoback
	{
		bool operator()(const SortedMeshSlot &a, const SortedMeshSlot &b);
	};

protected:
	enum BucketType {
		SOLID_BUCKET = 0,
		ALPHA_BUCKET,
		SOLID_INSTANCING_BUCKET,
		ALPHA_INSTANCING_BUCKET,
		ALPHA_DEPTH_BUCKET,
		ALPHA_DEPTH_INSTANCING_BUCKET,
		SOLID_SHADOW_BUCKET,
		ALPHA_SHADOW_BUCKET,
		SOLID_SHADOW_INSTANCING_BUCKET,
		ALPHA_SHADOW_INSTANCING_BUCKET,
		ALL_BUCKET,
		NUM_BUCKET_TYPE,
	};

	BucketList m_buckets[NUM_BUCKET_TYPE];

	RAS_ManagerNodeData m_nodeData;
	RAS_ManagerDownwardNode m_downwardNode;
	RAS_ManagerUpwardNode m_upwardNode;

	struct TextData
	{
		RAS_MaterialBucket *m_bucket;
		RAS_DisplayArrayBucket *m_arrayBucket;
	} m_text;

	std::vector<SortedMeshSlot> m_sortedSlots;
	ankerl::unordered_dense::map<RAS_IMaterial*, RAS_MaterialBucket*> m_materialToBucket;

private:
	ankerl::unordered_dense::map<size_t, RAS_BatchGroup*> m_frameBatchGroups;
	std::vector<RAS_MaterialBucket *> m_activeBuckets;
	size_t m_lastSortedLeafReserve;
	/// Cache de tamanho do leafs vector para sólidos — evita realocação por frame.
	size_t m_lastBasicLeafReserve = 0;
	/// Pool de RAS_BatchGroup — evita new/delete por frame.
	std::vector<RAS_BatchGroup *> m_batchGroupPool;

	void PrepareBuckets(RAS_Rasterizer *rasty, BucketType bucketType);
	void RenderBasicBuckets(RAS_Rasterizer *rasty, BucketType bucketType);
	void RenderSortedBuckets(RAS_Rasterizer *rasty, BucketType bucketType);
	void RenderBasicBucketsCombined(RAS_Rasterizer *rasty, BucketType bucketTypeA, BucketType bucketTypeB);
	void MarkBucketActive(RAS_MaterialBucket *bucket);

	friend class RAS_MaterialBucket;

public:
	RAS_BucketManager(RAS_IMaterial *textMaterial);
	virtual ~RAS_BucketManager();

	void Renderbuckets(RAS_Rasterizer::DrawType drawingMode, const mt::mat3x4& cameratrans, RAS_Rasterizer *rasty,
			RAS_OffScreen *offScreen);

	/// Renderiza apenas os buckets sólidos (para inserir grama antes dos alphas).
	void RenderbucketsSolids(RAS_Rasterizer::DrawType drawingMode, const mt::mat3x4& cameratrans, RAS_Rasterizer *rasty);
	/// Renderiza apenas os buckets alpha (chamado após a grama).
	void RenderbucketsAlphas(RAS_Rasterizer::DrawType drawingMode, const mt::mat3x4& cameratrans, RAS_Rasterizer *rasty,
			RAS_OffScreen *offScreen);

	RAS_MaterialBucket *FindBucket(RAS_IMaterial *material, bool &bucketCreated);
	RAS_DisplayArrayBucket *GetTextDisplayArrayBucket() const;

	void ReloadMaterials(RAS_IMaterial *material = nullptr);

	// freeing scenes only
	void RemoveMaterial(RAS_IMaterial *mat);

	// for merging
	void Merge(RAS_BucketManager *other, SCA_IScene *scene);

	// Global per-frame batch group cache API.
	RAS_BatchGroup* GetOrCreateFrameBatchGroup(size_t key, RAS_MeshUser* refUser);
	void ResetGlobalBatchGroupCache();
};

#endif // __RAS_BUCKETMANAGER_H__
