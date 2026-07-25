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

/** \file RAS_DisplayArrayBucket.h
 *  \ingroup bgerast
 */

#ifndef __RAS_DISPLAY_MATERIAL_BUCKET_H__
#define __RAS_DISPLAY_MATERIAL_BUCKET_H__

#include "CM_Update.h"

#include "RAS_MeshSlot.h"
#include "RAS_AttributeArray.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

class RAS_MaterialBucket;
class RAS_DisplayArray;
class RAS_Mesh;
class RAS_MeshMaterial;
class RAS_Deformer;
class RAS_IStorageInfo;
class RAS_InstancingBuffer;
class RAS_BatchGroup;
class RAS_MeshUser;

class RAS_DisplayArrayBucket
{
private:
	/// The parent bucket.
	RAS_MaterialBucket *m_bucket;
	/// The display array = list of vertexes and indexes.
	RAS_DisplayArray *m_displayArray;
	/// The parent mesh object, it can be nullptr for text objects.
	RAS_Mesh *m_mesh;
	/// The material mesh.
	RAS_MeshMaterial *m_meshMaterial;
	/// The list of all visible mesh slots to render this frame.
	RAS_MeshSlotList m_activeMeshSlots;
	/// The deformer using this display array.
	RAS_Deformer *m_deformer;

	RAS_DisplayArrayStorage *m_arrayStorage;
	/// Attribute array used for each different render categories.
	RAS_AttributeArray m_attribArray;

	/// The vertex buffer object containing all the data used for the instancing rendering for each drawing category.
	std::unique_ptr<RAS_InstancingBuffer> m_instancingBuffer[RAS_Rasterizer::RAS_DRAW_MAX];

	CM_UpdateClient<RAS_IMaterial> m_materialUpdateClient;
	CM_UpdateClient<RAS_DisplayArray> m_arrayUpdateClient;

	RAS_DisplayArrayNodeData m_nodeData;
	RAS_DisplayArrayDownwardNode m_downwardNode;
	RAS_DisplayArrayUpwardNode m_upwardNode;

	RAS_DisplayArrayDownwardNode m_instancingNode;
	RAS_DisplayArrayDownwardNode m_batchingNode;

	/// Cached vectors to avoid per-frame allocations.
	std::vector<SortedMeshSlot> m_cachedSortedMeshSlots;
	RAS_MeshSlotList m_cachedMeshSlots;
	std::vector<int> m_cachedCounts;
	std::vector<intptr_t> m_cachedIndices;
	std::vector<std::pair<intptr_t, int>> m_cachedRanges;
	std::vector<RAS_MeshUser*> m_cachedToMerge;
	size_t m_lastActiveMeshSlotsCount;

	/// Cache para caminho sem sort: reuso de offsets/contagens coalescidos se a sequência de partes não mudou.
	bool m_batchNoSortCacheValid = false;
	std::vector<short> m_cachedPartIndicesSnapshot;
	std::vector<short> m_currentPartIndicesTmp;
	std::vector<intptr_t> m_cachedCoalescedIndices;
	std::vector<int> m_cachedCoalescedCounts;

	// Cache para caminho com sort: reuso de offsets/contagens quando a ordem de partes não muda.
	std::vector<short> m_cachedSortedPartIndicesSnapshot;
	std::vector<intptr_t> m_cachedSortedIndices;
	std::vector<int> m_cachedSortedCounts;
	size_t m_lastSortedSlotsCount = 0;
	uint64_t m_lastBatchSortFrameId = UINT64_MAX;
	unsigned int m_lastBatchSortSequenceVersion = 0;
	size_t m_lastBatchSortSlotsCount = 0;
	mt::vec3 m_lastBatchSortPnorm;
	bool m_lastBatchSortPnormValid = false;
	uint64_t m_lastInstancingSortFrameId = UINT64_MAX;
	unsigned int m_lastInstancingSortSequenceVersion = 0;
	size_t m_lastInstancingSortSlotsCount = 0;
	mt::vec3 m_lastInstancingSortPnorm;
	bool m_lastInstancingSortPnormValid = false;
	uint64_t m_lastInstancingUploadFrameId = UINT64_MAX;
	unsigned int m_lastInstancingUploadSequenceVersion = 0;
	size_t m_lastInstancingUploadSlotsCount = 0;
	RAS_Rasterizer::DrawType m_lastInstancingUploadDrawingMode = RAS_Rasterizer::RAS_DRAW_MAX;
	short m_lastInstancingUploadPassIndex = -1;
	bool m_lastInstancingUploadSort = false;
	mt::vec3 m_lastInstancingUploadPnorm;
	bool m_lastInstancingUploadPnormValid = false;
	std::vector<std::uint32_t> m_cachedInstancingTransformVersions;
	bool m_cachedInstancingTransformVersionsValid = false;

	// Versão da sequência de slots para evitar recomputar quando nada mudou.
	unsigned int m_sequenceVersion = 0;
	unsigned int m_lastSequenceVersion = 0;

	/// Cached batching state.
	bool m_cachedUseBatching = false;
	uint64_t m_lastUpdateFrameId = UINT64_MAX;
	RAS_Rasterizer::DrawType m_lastUpdateDrawingMode = RAS_Rasterizer::RAS_DRAW_MAX;
	bool m_lastUpdateInstancing = false;
	uint64_t m_lastDeformerFrameId = UINT64_MAX;
	RAS_AttributeArrayStorage *m_cachedAttribStorage[RAS_Rasterizer::RAS_DRAW_MAX] = {};
	bool m_cachedAttribStorageComputed[RAS_Rasterizer::RAS_DRAW_MAX] = {};

	// Índice do último slot já mesclado no grupo em caminho de batching.
	size_t m_lastMergedSlotIndex = 0;

	int m_autoBatchCounter;
	RAS_BatchGroup *m_cachedBatchGroup;
	static const int s_autoBatchThreshold = 10;
	void TryAutoBatch();

public:
	RAS_DisplayArrayBucket(RAS_MaterialBucket *bucket, RAS_DisplayArray *array,
						   RAS_Mesh *mesh, RAS_MeshMaterial *meshmat, RAS_Deformer *deformer);
	~RAS_DisplayArrayBucket();

	/// \section Accesor
	RAS_MaterialBucket *GetBucket() const;
	RAS_DisplayArray *GetDisplayArray() const;
	void SetDisplayArray(RAS_DisplayArray *displayArray);
	RAS_Mesh *GetMesh() const;
	RAS_MeshMaterial *GetMeshMaterial() const;

	/// \section Active Mesh Slots Management.
	void ActivateMesh(RAS_MeshSlot *slot);
	void RemoveActiveMeshSlot(RAS_MeshSlot *slot);
	/// Remove all mesh slots from the list.
	void RemoveActiveMeshSlots();

	/// \section Render Infos
	bool UseBatching() const;

	/// Number of active mesh slots for this bucket (for pre-reserving leafs).
	size_t GetActiveMeshSlotCount() const { return m_activeMeshSlots.size(); }

	/// Update render infos.
	void UpdateActiveMeshSlots(RAS_Rasterizer::DrawType drawingMode, bool instancing);

	void GenerateTree(RAS_MaterialDownwardNode& downwardRoot, RAS_MaterialUpwardNode& upwardRoot,
			RAS_UpwardTreeLeafs& upwardLeafs, RAS_Rasterizer::DrawType drawingMode, bool sort, bool instancing);
	void BindUpwardNode(const RAS_DisplayArrayNodeTuple& tuple);
	void UnbindUpwardNode(const RAS_DisplayArrayNodeTuple& tuple);
	void RunDownwardNode(const RAS_DisplayArrayNodeTuple& tuple);
	void RunDownwardNodeNoArray(const RAS_DisplayArrayNodeTuple& tuple);
	void RunInstancingNode(const RAS_DisplayArrayNodeTuple& tuple);
	void RunBatchingNode(const RAS_DisplayArrayNodeTuple& tuple);

	/// Replace the material bucket of this display array bucket by the one given.
	void ChangeMaterialBucket(RAS_MaterialBucket *bucket);
};

typedef std::vector<RAS_DisplayArrayBucket *> RAS_DisplayArrayBucketList;

#endif  // __RAS_DISPLAY_MATERIAL_BUCKET_H__
