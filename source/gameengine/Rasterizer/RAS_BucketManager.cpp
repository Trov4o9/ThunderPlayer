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

/** \file gameengine/Rasterizer/RAS_BucketManager.cpp
 *  \ingroup bgerast
 */

#ifdef _MSC_VER
/* don't show these anoying STL warnings */
#  pragma warning (disable:4786)
#endif

#include "KX_Globals.h"
#include "KX_KetsjiEngine.h"
#include "RAS_MaterialBucket.h"
#include "KX_BlenderMaterial.h"
#include "RAS_Mesh.h"
#include "RAS_MeshUser.h"
#include "RAS_ICanvas.h"
#include "RAS_IMaterial.h"
#include "RAS_Rasterizer.h"
#include "Gbuffer.h"
#include "GPU_glew.h"

#include "RAS_BucketManager.h"
#include "RAS_BatchGroup.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdio>
/* sorting */

#if RAS_ENABLE_CPU_PROFILE
static std::uint64_t g_rasCpuProfile[RAS_CPU_PROFILE_COUNT] = {};

void RAS_CpuProfile_Accumulate(RAS_CpuProfileId id, std::uint64_t micros)
{
	g_rasCpuProfile[static_cast<int>(id)] += micros;
}

static const char *RAS_CpuProfile_Name(RAS_CpuProfileId id)
{
	switch (id) {
		case RAS_CPU_BUCKETMANAGER_RENDERBUCKETS: return "BucketManager::Renderbuckets";
		case RAS_CPU_BUCKETMANAGER_PREPAREBUCKETS: return "BucketManager::PrepareBuckets";
		case RAS_CPU_BUCKETMANAGER_RENDERBASIC: return "BucketManager::RenderBasicBuckets";
		case RAS_CPU_BUCKETMANAGER_RENDERSORTED: return "BucketManager::RenderSortedBuckets";
		case RAS_CPU_BUCKETMANAGER_RENDERBASICCOMBINED: return "BucketManager::RenderBasicBucketsCombined";
		case RAS_CPU_MATERIALBUCKET_GENERATETREE: return "MaterialBucket::GenerateTree";
		case RAS_CPU_DISPLAYARRAYBUCKET_RUNINSTANCINGNODE: return "DisplayArrayBucket::RunInstancingNode";
		case RAS_CPU_DISPLAYARRAYBUCKET_INSTANCING_SORT: return "DisplayArrayBucket::InstancingSort";
		case RAS_CPU_DISPLAYARRAYBUCKET_INSTANCING_CHECK_DIRTY: return "DisplayArrayBucket::InstancingCheckDirty";
		case RAS_CPU_DISPLAYARRAYBUCKET_BINDATTRIBS: return "DisplayArrayBucket::BindAttribs";
		case RAS_CPU_DISPLAYARRAYBUCKET_ACTIVATEINSTANCING: return "DisplayArrayBucket::ActivateInstancing";
		case RAS_CPU_DISPLAYARRAYBUCKET_DRAWINSTANCED: return "DisplayArrayBucket::DrawInstanced";
		case RAS_CPU_DISPLAYARRAYBUCKET_RUNBATCHINGNODE: return "DisplayArrayBucket::RunBatchingNode";
		case RAS_CPU_DISPLAYARRAYBUCKET_GENERATETREE: return "DisplayArrayBucket::GenerateTree";
		case RAS_CPU_SCENE_CALCULATEVISIBLEMESHES: return "Scene::CalculateVisibleMeshes";
		case RAS_CPU_SCENE_CVM_CACHE_LOOKUP: return "Scene::CVM.CacheLookup";
		case RAS_CPU_SCENE_CVM_CACHE_APPLY: return "Scene::CVM.CacheApply";
		case RAS_CPU_SCENE_CVM_SCAN_BB_MODIFIED: return "Scene::CVM.ScanBBoxModified";
		case RAS_CPU_SCENE_CVM_UPDATEDBVT: return "Scene::CVM.UpdateDbvt";
		case RAS_CPU_SCENE_CVM_DBVT_CULLINGTEST: return "Scene::CVM.DbvtCullingTest";
		case RAS_CPU_SCENE_CVM_FALLBACK_HANDLER: return "Scene::CVM.FallbackHandler";
		case RAS_CPU_SCENE_CVM_CACHE_STORE: return "Scene::CVM.CacheStore";
		case RAS_CPU_SCENE_CVM_CLEAR_BB_MODIFIED: return "Scene::CVM.ClearBBoxModified";
		case RAS_CPU_SCENE_UPDATEDBVT_SET_CULLED: return "Scene::UpdateDbvt.SetCulled";
		case RAS_CPU_SCENE_UPDATEDBVT_UPDATE_BOUNDS: return "Scene::UpdateDbvt.UpdateBounds";
		case RAS_CPU_SCENE_UPDATEDBVT_SET_CULLED_ALL: return "Scene::UpdateDbvt.SetCulledAll";
		case RAS_CPU_SCENE_UPDATEOBJECTLODS: return "Scene::UpdateObjectLods";
		case RAS_CPU_DISPLAYARRAYBUCKET_TRYAUTOBATCH: return "DisplayArrayBucket::TryAutoBatch";
		case RAS_CPU_DISPLAYARRAYBUCKET_ACTIVATEMESH: return "DisplayArrayBucket::ActivateMesh";
		case RAS_CPU_DISPLAYARRAYBUCKET_UPDATEACTIVESLOTS: return "DisplayArrayBucket::UpdateActiveMeshSlots";
		case RAS_CPU_MATERIALBUCKET_ACTIVATE: return "MaterialBucket::ActivateMaterial";
		case RAS_CPU_ATTRIBUTEARRAYSTORAGE_BINDPRIMITIVES: return "AttributeArrayStorage::BindPrimitives";
		case RAS_CPU_ATTRIBUTEARRAYSTORAGE_UNBINDPRIMITIVES: return "AttributeArrayStorage::UnbindPrimitives";
		case RAS_CPU_INSTANCINGBUFFER_UPDATE: return "InstancingBuffer::Update";
		case RAS_CPU_STORAGEVBO_DRAWINSTANCED: return "StorageVbo::glDrawElementsInstanced";
		default: return "Unknown";
	}
}

void RAS_CpuProfile_EndFrameAndPrint()
{
	std::uint64_t total = 0;
	for (int i = 0; i < RAS_CPU_PROFILE_COUNT; ++i) {
		total += g_rasCpuProfile[i];
	}

	std::fprintf(stderr, "[RAS CPU] total=%.3fms", double(total) / 1000.0);
	for (int i = 0; i < RAS_CPU_PROFILE_COUNT; ++i) {
		const std::uint64_t us = g_rasCpuProfile[i];
		if (us == 0) {
			continue;
		}
		std::fprintf(stderr, " | %s=%.3fms", RAS_CpuProfile_Name(static_cast<RAS_CpuProfileId>(i)), double(us) / 1000.0);
	}
	std::fprintf(stderr, "\n");
	std::fill_n(g_rasCpuProfile, RAS_CPU_PROFILE_COUNT, 0);
}
#endif

bool RAS_BucketManager::backtofront::operator()(const SortedMeshSlot &a, const SortedMeshSlot &b)
{
	return (a.m_z < b.m_z) || (a.m_z == b.m_z && a.m_ms < b.m_ms);
}

bool RAS_BucketManager::fronttoback::operator()(const SortedMeshSlot &a, const SortedMeshSlot &b)
{
	return (a.m_z > b.m_z) || (a.m_z == b.m_z && a.m_ms > b.m_ms);
}

RAS_BucketManager::RAS_BucketManager(RAS_IMaterial *textMaterial)
	:m_downwardNode(this, &m_nodeData, nullptr, nullptr),
	m_upwardNode(this, &m_nodeData, nullptr, nullptr),
	m_lastSortedLeafReserve(0)
{
	bool created;
	m_text.m_bucket = FindBucket(textMaterial, created);
	m_text.m_arrayBucket = new RAS_DisplayArrayBucket(m_text.m_bucket, nullptr, nullptr, nullptr, nullptr);
	m_activeBuckets.reserve(64);
}

RAS_BucketManager::~RAS_BucketManager()
{
	delete m_text.m_arrayBucket;

	for (RAS_MaterialBucket *bucket : m_buckets[ALL_BUCKET]) {
		delete bucket;
	}

	// Libera pool de batch groups
	for (RAS_BatchGroup *bg : m_batchGroupPool)
		delete bg;
}

void RAS_BucketManager::PrepareBuckets(RAS_Rasterizer *rasty, RAS_BucketManager::BucketType bucketType)
{
	RAS_CPU_PROFILE_SCOPE(RAS_CPU_BUCKETMANAGER_PREPAREBUCKETS);
	m_nodeData.m_materialPrepared = false;
	if (m_nodeData.m_shaderOverride && !m_nodeData.m_shaderOverrideGBuffer) {
		return;
	}

	const bool isGBufferOverride = m_nodeData.m_shaderOverrideGBuffer;
	GLint currentProgram = 0;
	GLint locMatColor = -1, locSpecColor = -1, locRough = -1, locMetal = -1, locSpec = -1;

	if (isGBufferOverride) {
		glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
		if (currentProgram != 0) {
			static thread_local GLint cachedProgram = -1;
			static thread_local GLint tl_locMatColor = -1, tl_locSpecColor = -1, tl_locRough = -1, tl_locMetal = -1, tl_locSpec = -1;
			static thread_local RAS_IMaterial* tl_lastMaterial = nullptr;
			if (cachedProgram != currentProgram) {
				tl_locMatColor = glGetUniformLocation(currentProgram, "u_MaterialColor");
				tl_locSpecColor = glGetUniformLocation(currentProgram, "u_SpecularColor");
				tl_locRough = glGetUniformLocation(currentProgram, "u_Roughness");
				tl_locMetal = glGetUniformLocation(currentProgram, "u_Metallic");
				tl_locSpec = glGetUniformLocation(currentProgram, "u_SpecularIntensity");
				cachedProgram = currentProgram;
				tl_lastMaterial = nullptr;
			}
			locMatColor = tl_locMatColor;
			locSpecColor = tl_locSpecColor;
			locRough = tl_locRough;
			locMetal = tl_locMetal;
			locSpec = tl_locSpec;
			// Store last material pointer in node data for reuse in loop via thread_local above.
		}
	}

	for (RAS_MaterialBucket *bucket : m_buckets[bucketType]) {
		if (!bucket->IsActive()) {
			continue;
		}
		RAS_IMaterial *mat = bucket->GetMaterial();

		if (isGBufferOverride && currentProgram != 0) {
			static thread_local RAS_IMaterial* tl_lastMaterial = nullptr;
			if (tl_lastMaterial != mat) {
				KX_BlenderMaterial* kxmat = static_cast<KX_BlenderMaterial*>(mat);
				float r, g, b, a, specr, specg, specb, spec, rough, metal;
				kxmat->GetMaterialData(r, g, b, a, specr, specg, specb, spec, rough, metal);
				if (locMatColor >= 0) glUniform4f(locMatColor, r, g, b, a);
				if (locSpecColor >= 0) glUniform4f(locSpecColor, specr, specg, specb, 1.0f);
				if (locRough >= 0) glUniform1f(locRough, rough);
				if (locMetal >= 0) glUniform1f(locMetal, metal);
				if (locSpec >= 0) glUniform1f(locSpec, spec);
				tl_lastMaterial = mat;
			}
		}
		else {
			mat->Prepare(rasty);
		}
	}
	m_nodeData.m_materialPrepared = true;
}


void RAS_BucketManager::RenderSortedBuckets(RAS_Rasterizer *rasty, RAS_BucketManager::BucketType bucketType)
{
	RAS_CPU_PROFILE_SCOPE(RAS_CPU_BUCKETMANAGER_RENDERSORTED);
	m_nodeData.m_materialPrepared = false;
	BucketList& solidBuckets = m_buckets[bucketType];
	if (solidBuckets.empty()) {
		return;
	}

	PrepareBuckets(rasty, bucketType);
	RAS_UpwardTreeLeafs leafs;
	if (m_lastSortedLeafReserve > 0) {
		leafs.reserve(m_lastSortedLeafReserve);
	}
	for (RAS_MaterialBucket *bucket : solidBuckets) {
		if (!bucket->IsActive()) {
			continue;
		}
		bucket->GenerateTree(m_downwardNode, m_upwardNode, leafs, m_nodeData.m_drawingMode, true);
	}
	m_lastSortedLeafReserve = leafs.size();

	m_nodeData.m_sort = true;

	if (m_downwardNode.GetValid()) {
		m_downwardNode.Execute(RAS_DummyNodeTuple());
	}
	if (!leafs.empty()) {
		/* Camera's near plane equation: pnorm.dot(point) + pval,
		 * but we leave out pval since it's constant anyway */
		const mt::mat3x4& trans = m_nodeData.m_trans;
		const mt::vec3 pnorm(trans[2], trans[5], trans[8]);
		
		const size_t numLeafs = leafs.size();
		if (numLeafs == 1) {
			RAS_MeshSlotUpwardNodeIterator iterator(leafs[0]);
		}
		else {
			m_sortedSlots.clear();
			m_sortedSlots.reserve(numLeafs);
			for (size_t i = 0; i < numLeafs; ++i) m_sortedSlots.emplace_back(leafs[i], pnorm);
			std::sort(m_sortedSlots.begin(), m_sortedSlots.end(), backtofront());
			auto it = m_sortedSlots.begin();
			RAS_MeshSlotUpwardNodeIterator iterator(it->m_node);
			++it;
			for (auto end = m_sortedSlots.end(); it != end; ++it) {
				iterator.NextNode(it->m_node);
			}
		}
	}
}

void RAS_BucketManager::RenderBasicBuckets(RAS_Rasterizer *rasty, RAS_BucketManager::BucketType bucketType)
{
	RAS_CPU_PROFILE_SCOPE(RAS_CPU_BUCKETMANAGER_RENDERBASIC);
	m_nodeData.m_materialPrepared = false;
	
	BucketList& list = m_buckets[bucketType];
	if (list.empty()) {
		return;
	}

	RAS_UpwardTreeLeafs leafs;
	if (m_lastBasicLeafReserve > 0)
		leafs.reserve(m_lastBasicLeafReserve);

	for (RAS_MaterialBucket *bucket : list) {
		if (!bucket->IsActive()) {
			continue;
		}
		bucket->GenerateTree(m_downwardNode, m_upwardNode, leafs, m_nodeData.m_drawingMode, false);
	}

	m_lastBasicLeafReserve = leafs.size();

	if (m_downwardNode.GetValid()) {
		m_nodeData.m_sort = false;
		m_downwardNode.Execute(RAS_DummyNodeTuple());
	}
}

void RAS_BucketManager::RenderBasicBucketsCombined(RAS_Rasterizer *rasty, RAS_BucketManager::BucketType bucketTypeA, RAS_BucketManager::BucketType bucketTypeB)
{
	RAS_CPU_PROFILE_SCOPE(RAS_CPU_BUCKETMANAGER_RENDERBASICCOMBINED);
	m_nodeData.m_materialPrepared = false;
	BucketList& listA = m_buckets[bucketTypeA];
	BucketList& listB = m_buckets[bucketTypeB];
	if (listA.empty() && listB.empty()) {
		return;
	}

	RAS_UpwardTreeLeafs leafs;
	if (m_lastBasicLeafReserve > 0)
		leafs.reserve(m_lastBasicLeafReserve);

	for (RAS_MaterialBucket *bucket : listA) {
		if (!bucket->IsActive()) {
			continue;
		}
		bucket->GenerateTree(m_downwardNode, m_upwardNode, leafs, m_nodeData.m_drawingMode, false);
	}
	for (RAS_MaterialBucket *bucket : listB) {
		if (!bucket->IsActive()) {
			continue;
		}
		bucket->GenerateTree(m_downwardNode, m_upwardNode, leafs, m_nodeData.m_drawingMode, false);
	}

	m_lastBasicLeafReserve = leafs.size();

	if (m_downwardNode.GetValid()) {
		m_nodeData.m_sort = false;
		m_downwardNode.Execute(RAS_DummyNodeTuple());
	}
}


void RAS_BucketManager::Renderbuckets(RAS_Rasterizer::DrawType drawingMode,
                                      const mt::mat3x4& cameratrans,
                                      RAS_Rasterizer *rasty,
                                      RAS_OffScreen *offScreen)
{
	RAS_CPU_PROFILE_SCOPE(RAS_CPU_BUCKETMANAGER_RENDERBUCKETS);
    m_nodeData.m_rasty = rasty;
    m_nodeData.m_trans = cameratrans;
    m_nodeData.m_drawingMode = drawingMode;

    const bool hasSolid = !m_buckets[SOLID_BUCKET].empty();
    const bool hasAlpha = !m_buckets[ALPHA_BUCKET].empty();
    const bool hasSolidInstancing = !m_buckets[SOLID_INSTANCING_BUCKET].empty();
    const bool hasAlphaInstancing = !m_buckets[ALPHA_INSTANCING_BUCKET].empty();
    const bool hasAlphaDepth = !m_buckets[ALPHA_DEPTH_BUCKET].empty();
    const bool hasAlphaDepthInstancing = !m_buckets[ALPHA_DEPTH_INSTANCING_BUCKET].empty();
    const bool hasSolidShadow = !m_buckets[SOLID_SHADOW_BUCKET].empty();
    const bool hasAlphaShadow = !m_buckets[ALPHA_SHADOW_BUCKET].empty();
    const bool hasSolidShadowInstancing = !m_buckets[SOLID_SHADOW_INSTANCING_BUCKET].empty();
    const bool hasAlphaShadowInstancing = !m_buckets[ALPHA_SHADOW_INSTANCING_BUCKET].empty();

    switch (drawingMode) {
        case RAS_Rasterizer::RAS_SHADOW:
        {
            const bool isVarianceShadow = (rasty->GetShadowMode() == RAS_Rasterizer::RAS_SHADOW_VARIANCE);
            m_nodeData.m_shaderOverride = true;

            rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_ENABLED);

            if (hasSolidShadow || hasSolidShadowInstancing) {
                rasty->SetOverrideShader(isVarianceShadow ?
                    RAS_Rasterizer::RAS_OVERRIDE_SHADER_SHADOW_VARIANCE :
                    RAS_Rasterizer::RAS_OVERRIDE_SHADER_BLACK);
                RenderBasicBucketsCombined(rasty, SOLID_SHADOW_BUCKET, SOLID_SHADOW_INSTANCING_BUCKET);
            }

            if (isVarianceShadow) {
                if (hasAlphaShadowInstancing || hasAlphaShadow) {
                    rasty->SetOverrideShader(RAS_Rasterizer::RAS_OVERRIDE_SHADER_SHADOW_VARIANCE);
                    RenderBasicBucketsCombined(rasty, ALPHA_SHADOW_BUCKET, ALPHA_SHADOW_INSTANCING_BUCKET);
                }
            } else {
                rasty->SetOverrideShader(RAS_Rasterizer::RAS_OVERRIDE_SHADER_NONE);
                m_nodeData.m_shaderOverride = false;

                if (hasAlphaShadowInstancing || hasAlphaShadow) {
                    RenderBasicBucketsCombined(rasty, ALPHA_SHADOW_BUCKET, ALPHA_SHADOW_INSTANCING_BUCKET);
                }
            }
            
            rasty->SetOverrideShader(RAS_Rasterizer::RAS_OVERRIDE_SHADER_NONE);
            break;
        }

        case RAS_Rasterizer::RAS_WIREFRAME:
        {
            m_nodeData.m_shaderOverride = true;

            rasty->SetLines(true);
            rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_ENABLED);

            if (hasSolid) {
                rasty->SetOverrideShader(RAS_Rasterizer::RAS_OVERRIDE_SHADER_BLACK);
                RenderBasicBuckets(rasty, SOLID_BUCKET);
            }

            if (hasSolidInstancing || hasAlphaInstancing) {
                rasty->SetOverrideShader(RAS_Rasterizer::RAS_OVERRIDE_SHADER_BLACK_INSTANCING);
                
                if (hasSolidInstancing)
                    RenderBasicBuckets(rasty, SOLID_INSTANCING_BUCKET);
                
                if (hasAlphaInstancing)
                    RenderSortedBuckets(rasty, ALPHA_INSTANCING_BUCKET);
            }

            if (hasAlpha) {
                rasty->SetOverrideShader(RAS_Rasterizer::RAS_OVERRIDE_SHADER_BLACK);
                RenderSortedBuckets(rasty, ALPHA_BUCKET);
            }

            rasty->SetOverrideShader(RAS_Rasterizer::RAS_OVERRIDE_SHADER_NONE);
            rasty->SetLines(false);
            break;
        }

        case RAS_Rasterizer::RAS_TEXTURED:
        {
            m_nodeData.m_shaderOverride = false;

            rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_ENABLED);

            if (hasSolid || hasSolidInstancing) {
                RenderBasicBucketsCombined(rasty, SOLID_BUCKET, SOLID_INSTANCING_BUCKET);
            }

            rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_DISABLED);

            if (hasAlphaDepth || hasAlphaDepthInstancing) {
                KX_KetsjiEngine *ketsji = KX_GetActiveEngine();
                RAS_ICanvas* canvas = ketsji->GetCanvas();
                rasty->UpdateGlobalDepthTexture(offScreen, canvas);
            }

            if (hasAlphaInstancing)
                RenderBasicBuckets(rasty, ALPHA_INSTANCING_BUCKET);
            if (hasAlpha)
                RenderSortedBuckets(rasty, ALPHA_BUCKET);

            rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_ENABLED);
            break;
        }

        case RAS_Rasterizer::RAS_RENDERER:
        {
            m_nodeData.m_shaderOverride = false;

            rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_ENABLED);

            if (hasSolid || hasSolidInstancing) {
                RenderBasicBucketsCombined(rasty, SOLID_BUCKET, SOLID_INSTANCING_BUCKET);
            }

            rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_DISABLED);
            rasty->ResetGlobalDepthTexture();

            if (hasAlphaInstancing)
                RenderBasicBuckets(rasty, ALPHA_INSTANCING_BUCKET);
            if (hasAlpha)
                RenderSortedBuckets(rasty, ALPHA_BUCKET);

            rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_ENABLED);
            break;
        }

        default:
            break;
    }

    for (RAS_MaterialBucket *bucket : m_activeBuckets) {
        bucket->RemoveActiveMeshSlots();
    }
    m_activeBuckets.clear();

    rasty->SetClientObject(nullptr);

    ResetGlobalBatchGroupCache();

#if RAS_ENABLE_CPU_PROFILE
	RAS_CpuProfile_EndFrameAndPrint();
#endif
}

// Renderiza apenas sólidos — para inserir grama antes dos alphas.
// Não faz cleanup (activeBuckets, clientObject) — RenderbucketsAlphas faz isso.
void RAS_BucketManager::RenderbucketsSolids(RAS_Rasterizer::DrawType drawingMode,
                                             const mt::mat3x4& cameratrans,
                                             RAS_Rasterizer *rasty)
{
    m_nodeData.m_rasty = rasty;
    m_nodeData.m_trans = cameratrans;
    m_nodeData.m_drawingMode = drawingMode;
    m_nodeData.m_shaderOverride = false;

    if (drawingMode == RAS_Rasterizer::RAS_TEXTURED || drawingMode == RAS_Rasterizer::RAS_RENDERER) {
        rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_ENABLED);
        const bool hasSolid = !m_buckets[SOLID_BUCKET].empty();
        const bool hasSolidInstancing = !m_buckets[SOLID_INSTANCING_BUCKET].empty();
        if (hasSolid || hasSolidInstancing)
            RenderBasicBucketsCombined(rasty, SOLID_BUCKET, SOLID_INSTANCING_BUCKET);
    }
    // Para outros modos (shadow, wireframe) não fazemos nada — Renderbuckets normal é usado
}

// Renderiza apenas alphas — chamado após a grama.
void RAS_BucketManager::RenderbucketsAlphas(RAS_Rasterizer::DrawType drawingMode,
                                             const mt::mat3x4& cameratrans,
                                             RAS_Rasterizer *rasty,
                                             RAS_OffScreen *offScreen)
{
    m_nodeData.m_rasty = rasty;
    m_nodeData.m_trans = cameratrans;
    m_nodeData.m_drawingMode = drawingMode;
    m_nodeData.m_shaderOverride = false;

    if (drawingMode == RAS_Rasterizer::RAS_TEXTURED) {
        const bool hasAlpha = !m_buckets[ALPHA_BUCKET].empty();
        const bool hasAlphaInstancing = !m_buckets[ALPHA_INSTANCING_BUCKET].empty();
        const bool hasAlphaDepth = !m_buckets[ALPHA_DEPTH_BUCKET].empty();
        const bool hasAlphaDepthInstancing = !m_buckets[ALPHA_DEPTH_INSTANCING_BUCKET].empty();

        rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_DISABLED);

        if (hasAlphaDepth || hasAlphaDepthInstancing) {
            KX_KetsjiEngine *ketsji = KX_GetActiveEngine();
            RAS_ICanvas* canvas = ketsji->GetCanvas();
            rasty->UpdateGlobalDepthTexture(offScreen, canvas);
        }

        if (hasAlphaInstancing)
            RenderBasicBuckets(rasty, ALPHA_INSTANCING_BUCKET);
        if (hasAlpha)
            RenderSortedBuckets(rasty, ALPHA_BUCKET);

        rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_ENABLED);
    }
    else if (drawingMode == RAS_Rasterizer::RAS_RENDERER) {
        const bool hasAlpha = !m_buckets[ALPHA_BUCKET].empty();
        const bool hasAlphaInstancing = !m_buckets[ALPHA_INSTANCING_BUCKET].empty();

        rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_DISABLED);
        rasty->ResetGlobalDepthTexture();

        if (hasAlphaInstancing)
            RenderBasicBuckets(rasty, ALPHA_INSTANCING_BUCKET);
        if (hasAlpha)
            RenderSortedBuckets(rasty, ALPHA_BUCKET);

        rasty->SetDepthMask(RAS_Rasterizer::RAS_DEPTHMASK_ENABLED);
    }

    for (RAS_MaterialBucket *bucket : m_activeBuckets)
        bucket->RemoveActiveMeshSlots();
    m_activeBuckets.clear();

    rasty->SetClientObject(nullptr);
    ResetGlobalBatchGroupCache();
}

RAS_MaterialBucket *RAS_BucketManager::FindBucket(RAS_IMaterial *material, bool &bucketCreated)
{
	bucketCreated = false;

	auto it = m_materialToBucket.find(material);
	if (it != m_materialToBucket.end()) {
		return it->second;
	}

	RAS_MaterialBucket *bucket = new RAS_MaterialBucket(material, this);
	bucketCreated = true;
	m_materialToBucket[material] = bucket;

	const bool useinstancing = material->UseInstancing();
	if (!material->OnlyShadow()) {
		if (material->IsAlpha()) {
			m_buckets[useinstancing ? ALPHA_INSTANCING_BUCKET : ALPHA_BUCKET].push_back(bucket);
			if (material->IsAlphaDepth()) {
				m_buckets[useinstancing ? ALPHA_DEPTH_INSTANCING_BUCKET : ALPHA_DEPTH_BUCKET].push_back(bucket);
			}
		}
		else {
			m_buckets[useinstancing ? SOLID_INSTANCING_BUCKET : SOLID_BUCKET].push_back(bucket);
		}
	}
	if (material->CastsShadows()) {
		if (material->IsAlphaShadow()) {
			m_buckets[useinstancing ? ALPHA_SHADOW_INSTANCING_BUCKET : ALPHA_SHADOW_BUCKET].push_back(bucket);
		}
		else {
			m_buckets[useinstancing ? SOLID_SHADOW_INSTANCING_BUCKET : SOLID_SHADOW_BUCKET].push_back(bucket);
		}
	}

	// Used to free the bucket.
	m_buckets[ALL_BUCKET].push_back(bucket);
	return bucket;
}

RAS_DisplayArrayBucket *RAS_BucketManager::GetTextDisplayArrayBucket() const
{
	return m_text.m_arrayBucket;
}

void RAS_BucketManager::ReloadMaterials(RAS_IMaterial *mat)
{
	if (mat) {
		auto it = m_materialToBucket.find(mat);
		if (it != m_materialToBucket.end()) {
			it->second->GetMaterial()->ReloadMaterial();
		}
	}
	else {
		for (RAS_MaterialBucket *bucket : m_buckets[ALL_BUCKET]) {
			bucket->GetMaterial()->ReloadMaterial();
		}
	}
}

void RAS_BucketManager::RemoveMaterial(RAS_IMaterial *mat)
{
	auto itMap = m_materialToBucket.find(mat);
	if (itMap != m_materialToBucket.end()) {
		RAS_MaterialBucket *bucket = itMap->second;

		for (unsigned short i = 0; i < NUM_BUCKET_TYPE; ++i) {
			BucketList& buckets = m_buckets[i];
			BucketList::iterator itList = std::find(buckets.begin(), buckets.end(), bucket);
			if (itList != buckets.end()) {
				buckets.erase(itList);
			}
		}

		m_materialToBucket.erase(itMap);
		delete bucket;
	}
}

void RAS_BucketManager::Merge(RAS_BucketManager *other, SCA_IScene *scene)
{
	m_activeBuckets.clear();
	other->m_activeBuckets.clear();
	for (unsigned short i = 0; i < NUM_BUCKET_TYPE; ++i) {
		BucketList& buckets = m_buckets[i];
		BucketList& otherbuckets = other->m_buckets[i];

		// Skip the text bucket.
		std::remove_copy(otherbuckets.begin(), otherbuckets.end(), std::back_inserter(buckets), other->m_text.m_bucket);
		otherbuckets.clear();
	}
	m_materialToBucket.insert(other->m_materialToBucket.begin(), other->m_materialToBucket.end());
	other->m_materialToBucket.clear();
}

RAS_BatchGroup* RAS_BucketManager::GetOrCreateFrameBatchGroup(size_t key, RAS_MeshUser* refUser)
{
	auto it = m_frameBatchGroups.find(key);
	if (it != m_frameBatchGroups.end() && it->second) {
		return it->second;
	}

	RAS_BatchGroup* bg = nullptr;
	for (RAS_BatchGroup* candidate : m_batchGroupPool) {
		if (candidate && candidate->GetUsers() == 0) {
			bg = candidate;
			break;
		}
	}
	if (!bg) {
		bg = new RAS_BatchGroup();
		m_batchGroupPool.push_back(bg);
	}

	bg->AddMeshUser();
	bg->SetReferenceMeshUser(refUser);
	m_frameBatchGroups.emplace(key, bg);
	return bg;
}

void RAS_BucketManager::ResetGlobalBatchGroupCache()
{
	for (auto& kv : m_frameBatchGroups) {
		RAS_BatchGroup* bg = kv.second;
		if (bg)
			bg->RemoveMeshUser();
	}
	m_frameBatchGroups.clear();
}

void RAS_BucketManager::MarkBucketActive(RAS_MaterialBucket *bucket)
{
	m_activeBuckets.push_back(bucket);
}
