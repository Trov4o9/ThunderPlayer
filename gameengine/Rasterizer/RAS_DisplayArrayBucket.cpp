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

/** \file gameengine/Rasterizer/RAS_DisplayArrayBucket.cpp
 *  \ingroup bgerast
 */

#include "RAS_DisplayArrayBucket.h"
#include "RAS_BatchDisplayArray.h"
#include "RAS_DisplayArrayStorage.h"
#include "RAS_AttributeArrayStorage.h"
#include "RAS_MaterialBucket.h"
#include "RAS_IMaterial.h"
#include "RAS_Mesh.h"
#include "RAS_MeshUser.h"
#include "RAS_Deformer.h"
#include "RAS_BatchGroup.h"
#include "RAS_Rasterizer.h"
#include "RAS_InstancingBuffer.h"
#include "RAS_BucketManager.h"
#include "KX_Globals.h"
#include "KX_KetsjiEngine.h"
#include <unordered_set>
#include <cstdio>
#include <cstdint>   // uint8_t
#include <cstring>   // std::memcpy
#include <algorithm>

#ifdef _MSC_VER
#  pragma warning (disable:4786)
#endif

#ifdef WIN32
#  include <windows.h>
#endif // WIN32

RAS_DisplayArrayBucket::RAS_DisplayArrayBucket(RAS_MaterialBucket *bucket, RAS_DisplayArray *array,
                                               RAS_Mesh *mesh, RAS_MeshMaterial *meshmat, RAS_Deformer *deformer)
	:m_bucket(bucket),
	m_displayArray(array),
	m_mesh(mesh),
	m_meshMaterial(meshmat),
	m_deformer(deformer),
	m_arrayStorage(nullptr),
	m_attribArray(m_displayArray),
	m_materialUpdateClient(RAS_IMaterial::ATTRIBUTES_MODIFIED | RAS_IMaterial::SHADER_MODIFIED,
	                      RAS_IMaterial::ATTRIBUTES_MODIFIED | RAS_IMaterial::SHADER_MODIFIED),
	m_arrayUpdateClient(RAS_DisplayArray::ANY_MODIFIED, RAS_DisplayArray::STORAGE_INVALID),
	m_instancingNode(this, &m_nodeData, &RAS_DisplayArrayBucket::RunInstancingNode, nullptr),
	m_batchingNode(this, &m_nodeData, &RAS_DisplayArrayBucket::RunBatchingNode, nullptr),
	m_autoBatchCounter(0),
	m_cachedBatchGroup(nullptr),
	m_cachedUseBatching(false),
	m_lastMergedSlotIndex(0),
	m_lastActiveMeshSlotsCount(0)
{
	m_bucket->AddDisplayArrayBucket(this);

	// Display array can be null in case of text.
	if (m_displayArray) {
		m_cachedUseBatching = (m_displayArray->GetType() == RAS_DisplayArray::BATCHING);
		m_downwardNode = RAS_DisplayArrayDownwardNode(this, &m_nodeData, &RAS_DisplayArrayBucket::RunDownwardNode, nullptr);
		m_upwardNode = RAS_DisplayArrayUpwardNode(this, &m_nodeData, &RAS_DisplayArrayBucket::BindUpwardNode,
		                                          &RAS_DisplayArrayBucket::UnbindUpwardNode);

		m_arrayStorage = &m_displayArray->GetStorage();
		m_displayArray->AddUpdateClient(&m_arrayUpdateClient);
	}
	else {
		// If there's no display array then we draw using text, in this case the display array bind/unbind should be avoid.
		m_downwardNode = RAS_DisplayArrayDownwardNode(this, &m_nodeData, &RAS_DisplayArrayBucket::RunDownwardNodeNoArray, nullptr);
		m_upwardNode = RAS_DisplayArrayUpwardNode(this, &m_nodeData, nullptr, nullptr);
	}

	// Initialize node arguments.
	m_nodeData.m_array = m_displayArray;
	m_nodeData.m_arrayStorage = m_arrayStorage;
	m_nodeData.m_attribStorage = nullptr;
	m_nodeData.m_applyMatrix = (!m_deformer || !m_deformer->SkipVertexTransform());

	RAS_IMaterial *material = bucket->GetMaterial();
	material->AddUpdateClient(&m_materialUpdateClient);
}

RAS_DisplayArrayBucket::~RAS_DisplayArrayBucket()
{
	if (m_cachedBatchGroup) {
		m_cachedBatchGroup->RemoveMeshUser();
	}
	for (RAS_MeshSlot *slot : m_activeMeshSlots) {
		if (slot && slot->m_displayArrayBucket == this) {
			slot->m_displayArrayBucket = nullptr;
		}
	}
	m_activeMeshSlots.clear();
	m_bucket->RemoveDisplayArrayBucket(this);
}

RAS_MaterialBucket *RAS_DisplayArrayBucket::GetBucket() const
{
	return m_bucket;
}

RAS_DisplayArray *RAS_DisplayArrayBucket::GetDisplayArray() const
{
	return m_displayArray;
}

void RAS_DisplayArrayBucket::SetDisplayArray(RAS_DisplayArray *displayArray)
{
	m_displayArray = displayArray;
	m_cachedUseBatching = (m_displayArray && m_displayArray->GetType() == RAS_DisplayArray::BATCHING);
	m_lastUpdateFrameId = UINT64_MAX;
	m_lastDeformerFrameId = UINT64_MAX;
	std::fill_n(m_cachedAttribStorageComputed, RAS_Rasterizer::RAS_DRAW_MAX, false);
}

RAS_Mesh *RAS_DisplayArrayBucket::GetMesh() const
{
	return m_mesh;
}

RAS_MeshMaterial *RAS_DisplayArrayBucket::GetMeshMaterial() const
{
	return m_meshMaterial;
}

void RAS_DisplayArrayBucket::ActivateMesh(RAS_MeshSlot *slot)
{
	RAS_CPU_PROFILE_SCOPE(RAS_CPU_DISPLAYARRAYBUCKET_ACTIVATEMESH);
	if (m_activeMeshSlots.empty()) {
		m_bucket->MarkActive();
		m_bucket->MarkDisplayArrayBucketActive(this);
		if (m_lastActiveMeshSlotsCount > 0) {
			m_activeMeshSlots.reserve(m_lastActiveMeshSlotsCount);
		}
	}
	m_activeMeshSlots.push_back(slot);
	++m_sequenceVersion;
	m_lastUpdateFrameId = UINT64_MAX;

	if (m_bucket->IsZSort()) {
		return;
	}
	if (m_mesh && !m_mesh->AllowAutoBatching()) {
		return;
	}

	const bool canAutoBatch = !m_cachedUseBatching && !m_deformer && !m_bucket->UseInstancing();
	if (canAutoBatch) {
		const size_t numActive = m_activeMeshSlots.size();
		if (numActive > 1) {
			auto *mu = slot->m_meshUser;
			if (!mu->GetBatchGroup()) {
				m_autoBatchCounter++;
				if (m_autoBatchCounter > s_autoBatchThreshold) {
					TryAutoBatch();
					m_autoBatchCounter = 0;
				}
			}
		}
		else {
			m_autoBatchCounter = 0;
		}
	}
}

void RAS_DisplayArrayBucket::RemoveActiveMeshSlot(RAS_MeshSlot *slot)
{
	if (!slot || m_activeMeshSlots.empty()) {
		return;
	}

	const size_t prevSize = m_activeMeshSlots.size();
	m_activeMeshSlots.erase(std::remove(m_activeMeshSlots.begin(), m_activeMeshSlots.end(), slot), m_activeMeshSlots.end());
	if (m_activeMeshSlots.size() == prevSize) {
		return;
	}
	++m_sequenceVersion;
	m_lastUpdateFrameId = UINT64_MAX;
	m_batchNoSortCacheValid = false;
	m_cachedInstancingTransformVersionsValid = false;
	m_lastInstancingSortFrameId = UINT64_MAX;
	m_lastBatchSortFrameId = UINT64_MAX;
	m_lastInstancingUploadFrameId = UINT64_MAX;
	m_lastMergedSlotIndex = 0;
	m_cachedMeshSlots.clear();
	m_cachedSortedMeshSlots.clear();
	m_cachedPartIndicesSnapshot.clear();
	m_cachedSortedPartIndicesSnapshot.clear();
	m_cachedCoalescedIndices.clear();
	m_cachedCoalescedCounts.clear();
	m_cachedSortedIndices.clear();
	m_cachedSortedCounts.clear();
	m_cachedRanges.clear();
	m_currentPartIndicesTmp.clear();

	if (m_activeMeshSlots.empty()) {
		if (m_cachedBatchGroup) {
			m_cachedBatchGroup->RemoveMeshUser();
			m_cachedBatchGroup = nullptr;
		}
		m_autoBatchCounter = 0;
	}
}

void RAS_DisplayArrayBucket::TryAutoBatch()
{
	RAS_CPU_PROFILE_SCOPE(RAS_CPU_DISPLAYARRAYBUCKET_TRYAUTOBATCH);
	const RAS_MeshSlotList& activeSlots = m_activeMeshSlots;
	const size_t numSlots = activeSlots.size();
	if (numSlots <= 1) {
		return;
	}

	const size_t startIdx = (m_lastMergedSlotIndex <= numSlots) ? m_lastMergedSlotIndex : numSlots;

	if (!m_cachedBatchGroup) {
		const size_t key = RAS_MaterialBucket::ComputeBatchKey(m_mesh, m_displayArray, m_bucket->GetMaterial());
		m_cachedBatchGroup = m_bucket->GetOrCreateFrameBatchGroup(key, activeSlots.front()->m_meshUser);
		m_cachedBatchGroup->AddMeshUser();
		for (size_t i = 0; i < numSlots; ++i) {
			RAS_MeshUser *mu = activeSlots[i]->m_meshUser;
			RAS_BatchGroup *bg = mu->GetBatchGroup();
			if (bg && bg != m_cachedBatchGroup) {
				bg->SplitMeshUser(mu);
			}
			if (bg != m_cachedBatchGroup) {
				m_cachedBatchGroup->MergeMeshUser(mu, mu->GetMatrix());
			}
		}
		m_batchNoSortCacheValid = false;
		++m_sequenceVersion;
		m_lastMergedSlotIndex = numSlots;
		return;
	}

	for (size_t i = startIdx; i < numSlots; ++i) {
		RAS_MeshUser *mu = activeSlots[i]->m_meshUser;
		RAS_BatchGroup *bg = mu->GetBatchGroup();
		if (bg && bg != m_cachedBatchGroup) {
			bg->SplitMeshUser(mu);
			m_cachedBatchGroup->MergeMeshUser(mu, mu->GetMatrix());
			m_batchNoSortCacheValid = false;
			++m_sequenceVersion;
		}
		else if (!bg) {
			m_cachedBatchGroup->MergeMeshUser(mu, mu->GetMatrix());
			m_batchNoSortCacheValid = false;
			++m_sequenceVersion;
		}
	}
	m_lastMergedSlotIndex = numSlots;
}


void RAS_DisplayArrayBucket::RemoveActiveMeshSlots()
{
	if (!m_activeMeshSlots.empty()) {
		m_lastActiveMeshSlotsCount = m_activeMeshSlots.size();
		m_activeMeshSlots.clear();
		++m_sequenceVersion;
		m_lastUpdateFrameId = UINT64_MAX;
	}

	if (m_cachedBatchGroup) {
		m_cachedBatchGroup->RemoveMeshUser();
		m_cachedBatchGroup = nullptr;
	}
	m_lastMergedSlotIndex = 0;
}

bool RAS_DisplayArrayBucket::UseBatching() const
{
	return (m_displayArray && m_displayArray->GetType() == RAS_DisplayArray::BATCHING);
}

void RAS_DisplayArrayBucket::UpdateActiveMeshSlots(RAS_Rasterizer::DrawType drawingMode, bool instancing)
{
	RAS_CPU_PROFILE_SCOPE(RAS_CPU_DISPLAYARRAYBUCKET_UPDATEACTIVESLOTS);
    if (!m_displayArray)
        return;

	KX_KetsjiEngine *engine = KX_GetActiveEngine();
	const bool hasFrameStamp = (engine != nullptr);
	const uint64_t frameId = hasFrameStamp ? engine->GetFrameCounter() : UINT64_MAX;
	const bool sameDeformerFrame = hasFrameStamp && (m_lastDeformerFrameId == frameId);
	const bool sameUpdateFrame = hasFrameStamp && (m_lastUpdateFrameId == frameId);

	if (m_deformer) {
		if (!sameDeformerFrame) {
			m_deformer->Apply(m_displayArray);
			if (hasFrameStamp) {
				m_lastDeformerFrameId = frameId;
			}
		}
	}

	const bool hasCompatibleFrameCache = hasFrameStamp &&
	                                     sameUpdateFrame &&
	                                     (m_lastUpdateDrawingMode == drawingMode) &&
	                                     (m_lastUpdateInstancing == instancing);

    const unsigned int modifiedFlag = m_arrayUpdateClient.GetInvalidAndClear();
    const unsigned int materialFlag = m_materialUpdateClient.GetInvalidAndClear();

	if (hasCompatibleFrameCache && modifiedFlag == RAS_DisplayArray::NONE_MODIFIED && materialFlag == 0) {
		const unsigned short modeIndex = static_cast<unsigned short>(drawingMode);
		if (!m_cachedAttribStorageComputed[modeIndex]) {
			m_cachedAttribStorage[modeIndex] = m_attribArray.GetStorage(drawingMode);
			m_cachedAttribStorageComputed[modeIndex] = true;
		}
		m_nodeData.m_attribStorage = m_cachedAttribStorage[modeIndex];
		if (instancing && !m_instancingBuffer[drawingMode]) {
			RAS_IMaterial *mat = m_bucket->GetMaterial();
			m_instancingBuffer[drawingMode].reset(
				new RAS_InstancingBuffer(mat->GetInstancingAttribs())
			);
		}
		m_nodeData.m_instancingBuffer = instancing ? m_instancingBuffer[drawingMode].get() : nullptr;
		return;
	}

    if (modifiedFlag != RAS_DisplayArray::NONE_MODIFIED) {

        if (modifiedFlag & RAS_DisplayArray::STORAGE_INVALID) {
            m_displayArray->ConstructStorage();
        }

        if (modifiedFlag & RAS_DisplayArray::SIZE_MODIFIED) {
            m_arrayStorage->UpdateSize();
            m_attribArray.Clear();
            m_cachedUseBatching = (m_displayArray->GetType() == RAS_DisplayArray::BATCHING);
			std::fill_n(m_cachedAttribStorageComputed, RAS_Rasterizer::RAS_DRAW_MAX, false);
        }

        if (modifiedFlag & RAS_DisplayArray::MESH_MODIFIED) {
            m_arrayStorage->UpdateVertexData(modifiedFlag);
        }

        if (modifiedFlag & RAS_DisplayArray::POSITION_MODIFIED) {
            m_displayArray->InvalidatePolygonCenters();
        }
		m_batchNoSortCacheValid = false;
		++m_sequenceVersion;
    }

    if (materialFlag != 0) {
		if (materialFlag & RAS_IMaterial::SHADER_MODIFIED) {
			for (unsigned short i = 0; i < RAS_Rasterizer::RAS_DRAW_MAX; ++i) {
				m_instancingBuffer[i].reset(nullptr);
			}
		}

        RAS_IMaterial *mat = m_bucket->GetMaterial();
        const auto& layersInfo = m_mesh->GetLayersInfo();
        const auto attribList = mat->GetAttribs(layersInfo);

        m_attribArray = RAS_AttributeArray(attribList, m_displayArray);
		std::fill_n(m_cachedAttribStorageComputed, RAS_Rasterizer::RAS_DRAW_MAX, false);
		m_batchNoSortCacheValid = false;
		++m_sequenceVersion;
		m_lastUpdateFrameId = UINT64_MAX;
    }

	const unsigned short modeIndex = static_cast<unsigned short>(drawingMode);
	if (!m_cachedAttribStorageComputed[modeIndex]) {
		m_cachedAttribStorage[modeIndex] = m_attribArray.GetStorage(drawingMode);
		m_cachedAttribStorageComputed[modeIndex] = true;
	}
    m_nodeData.m_attribStorage = m_cachedAttribStorage[modeIndex];

	// Pre-reservas para evitar realocações frequentes nos nós de desenho.
	if (!m_activeMeshSlots.empty()) {
		const size_t n = m_activeMeshSlots.size();
		if (m_cachedCounts.capacity() < n) m_cachedCounts.reserve(n * 2);
		if (m_cachedIndices.capacity() < n) m_cachedIndices.reserve(n * 2);
		if (m_cachedRanges.capacity() < n) m_cachedRanges.reserve(n * 2);
		if (m_cachedSortedMeshSlots.capacity() < n) m_cachedSortedMeshSlots.reserve(n * 2);
		if (m_cachedMeshSlots.capacity() < n) m_cachedMeshSlots.reserve(n * 2);
		if (m_cachedCoalescedCounts.capacity() < n) m_cachedCoalescedCounts.reserve(n * 2);
		if (m_cachedCoalescedIndices.capacity() < n) m_cachedCoalescedIndices.reserve(n * 2);
		if (m_cachedPartIndicesSnapshot.capacity() < n) m_cachedPartIndicesSnapshot.reserve(n * 2);
	}

    if (instancing) {
        if (!m_instancingBuffer[drawingMode]) {
            RAS_IMaterial *mat = m_bucket->GetMaterial();
            m_instancingBuffer[drawingMode].reset(
                new RAS_InstancingBuffer(mat->GetInstancingAttribs())
            );
        }
        m_nodeData.m_instancingBuffer = m_instancingBuffer[drawingMode].get();
    }
    else {
        m_nodeData.m_instancingBuffer = nullptr;
    }

	if (hasFrameStamp) {
		m_lastUpdateFrameId = frameId;
		m_lastUpdateDrawingMode = drawingMode;
		m_lastUpdateInstancing = instancing;
	}
}

void RAS_DisplayArrayBucket::GenerateTree(RAS_MaterialDownwardNode& downwardRoot, RAS_MaterialUpwardNode& upwardRoot,
                                          RAS_UpwardTreeLeafs& upwardLeafs, RAS_Rasterizer::DrawType drawingMode, bool sort, bool instancing)
{
	RAS_CPU_PROFILE_SCOPE(RAS_CPU_DISPLAYARRAYBUCKET_GENERATETREE);
	if (m_activeMeshSlots.empty()) {
		return;
	}

	const bool useBatching = m_cachedUseBatching;
	if (useBatching) {
		instancing = false;
	}

	// Update deformer and render settings.
	UpdateActiveMeshSlots(drawingMode, instancing);

	if (instancing) {
		downwardRoot.AddChild(&m_instancingNode);
	}
	else if (useBatching) {
		downwardRoot.AddChild(&m_batchingNode);
	}
	else if (sort) {
		upwardLeafs.reserve(upwardLeafs.size() + m_activeMeshSlots.size());
		for (RAS_MeshSlot *slot : m_activeMeshSlots) {
			auto *node = slot->GetNode();
			node->SetParent(&m_upwardNode);
			upwardLeafs.push_back(node);
		}

		m_upwardNode.SetParent(&upwardRoot);
	}
	else {
		downwardRoot.AddChild(&m_downwardNode);
	}
}



void RAS_DisplayArrayBucket::BindUpwardNode(const RAS_DisplayArrayNodeTuple& tuple)
{
	m_nodeData.m_attribStorage->BindPrimitives();
}

void RAS_DisplayArrayBucket::UnbindUpwardNode(const RAS_DisplayArrayNodeTuple& tuple)
{
	m_nodeData.m_attribStorage->UnbindPrimitives();
}

void RAS_DisplayArrayBucket::RunDownwardNode(const RAS_DisplayArrayNodeTuple& tuple)
{
	RAS_AttributeArrayStorage *attribStorage = m_nodeData.m_attribStorage;
	attribStorage->BindPrimitives();

	const RAS_MeshSlotNodeTuple msTuple(tuple, &m_nodeData);
	for (RAS_MeshSlot *slot : m_activeMeshSlots) {
		slot->RunNode(msTuple);
	}

	attribStorage->UnbindPrimitives();
}

void RAS_DisplayArrayBucket::RunDownwardNodeNoArray(const RAS_DisplayArrayNodeTuple& tuple)
{
	const RAS_MeshSlotNodeTuple msTuple(tuple, &m_nodeData);
	for (RAS_MeshSlot *slot : m_activeMeshSlots) {
		slot->RunNode(msTuple);
	}
}

void RAS_DisplayArrayBucket::RunInstancingNode(const RAS_DisplayArrayNodeTuple& tuple)
{
	RAS_CPU_PROFILE_SCOPE(RAS_CPU_DISPLAYARRAYBUCKET_RUNINSTANCINGNODE);
	RAS_ManagerNodeData *managerData = tuple.m_managerData;
	RAS_MaterialNodeData *materialData = tuple.m_materialData;
	RAS_Rasterizer *rasty = managerData->m_rasty;

	const unsigned int nummeshslots = m_activeMeshSlots.size();
	if (nummeshslots == 0) {
		return;
	}
    /*
	static thread_local size_t s_totalInstancingSegments = 0;
	std::unordered_set<RAS_BatchGroup*> instGroups;
	instGroups.reserve(nummeshslots);
	for (unsigned int i = 0; i < nummeshslots; ++i) {
		RAS_MeshUser* mu = m_activeMeshSlots[i]->m_meshUser;
		instGroups.insert(mu ? mu->GetBatchGroup() : nullptr);
	}
	const bool isSortPath = managerData->m_sort != 0;
	const size_t segmentsDbg = nummeshslots;
	s_totalInstancingSegments += segmentsDbg;
	std::fprintf(stderr,
	             "[Dbg] RunInstancingNode path=%s slots=%u segments=%zu totalSegments=%zu groupsUnique=%zu\n",
	             isSortPath ? "sort" : "no-sort",
	             nummeshslots,
	             segmentsDbg,
	             s_totalInstancingSegments,
	             instGroups.size());

	*/

	RAS_IMaterial *material = materialData->m_material;
	RAS_InstancingBuffer *buffer = m_nodeData.m_instancingBuffer;

	const short matPasIndex = material->GetPassIndex();
	KX_KetsjiEngine *engine = KX_GetActiveEngine();
	const bool hasFrameStamp = (engine != nullptr);
	const uint64_t frameId = hasFrameStamp ? engine->GetFrameCounter() : UINT64_MAX;
	bool didUpdateBuffer = false;

	// Bind the instancing buffer to work on it.
	buffer->Realloc(nummeshslots);

	/* If the material use the transparency we must sort all mesh slots depending on the distance.
	 * This code share the code used in RAS_BucketManager to do the sort.
	 */
	if (managerData->m_sort && nummeshslots > 1) {
		RAS_CPU_PROFILE_SCOPE(RAS_CPU_DISPLAYARRAYBUCKET_INSTANCING_SORT);
		const mt::mat3x4& trans = managerData->m_trans;
		const mt::vec3 pnorm(trans[2], trans[5], trans[8]);
		const bool canReuseInstancingSort = hasFrameStamp &&
		                                    m_lastInstancingSortPnormValid &&
		                                    (m_lastInstancingSortFrameId == frameId) &&
		                                    (m_lastInstancingSortSequenceVersion == m_sequenceVersion) &&
		                                    (m_lastInstancingSortSlotsCount == nummeshslots) &&
		                                    (m_lastInstancingSortPnorm[0] == pnorm[0]) &&
		                                    (m_lastInstancingSortPnorm[1] == pnorm[1]) &&
		                                    (m_lastInstancingSortPnorm[2] == pnorm[2]) &&
		                                    (m_cachedMeshSlots.size() == nummeshslots);
		if (!canReuseInstancingSort) {
			m_cachedSortedMeshSlots.resize(nummeshslots);
			std::transform(m_activeMeshSlots.begin(), m_activeMeshSlots.end(), m_cachedSortedMeshSlots.begin(),
			               [&pnorm](RAS_MeshSlot *slot) {
				return SortedMeshSlot(slot, pnorm);
			});

			std::sort(m_cachedSortedMeshSlots.begin(), m_cachedSortedMeshSlots.end(), RAS_BucketManager::backtofront());
			m_cachedMeshSlots.resize(nummeshslots);
			for (unsigned int i = 0; i < nummeshslots; ++i) {
				m_cachedMeshSlots[i] = m_cachedSortedMeshSlots[i].m_ms;
			}
			if (hasFrameStamp) {
				m_lastInstancingSortFrameId = frameId;
				m_lastInstancingSortSequenceVersion = m_sequenceVersion;
				m_lastInstancingSortSlotsCount = nummeshslots;
				m_lastInstancingSortPnorm = pnorm;
				m_lastInstancingSortPnormValid = true;
			}
		}
		
		bool transformsDirty = false;
		{
			RAS_CPU_PROFILE_SCOPE(RAS_CPU_DISPLAYARRAYBUCKET_INSTANCING_CHECK_DIRTY);
			const bool canReuseAcrossFrames = m_cachedInstancingTransformVersionsValid &&
											 (m_lastInstancingUploadSequenceVersion == m_sequenceVersion) &&
											 (m_lastInstancingUploadSlotsCount == nummeshslots) &&
											 (m_lastInstancingUploadDrawingMode == materialData->m_drawingMode) &&
											 (m_lastInstancingUploadPassIndex == matPasIndex) &&
											 m_lastInstancingUploadSort &&
											 m_lastInstancingUploadPnormValid &&
											 (m_lastInstancingUploadPnorm[0] == pnorm[0]) &&
											 (m_lastInstancingUploadPnorm[1] == pnorm[1]) &&
											 (m_lastInstancingUploadPnorm[2] == pnorm[2]) &&
											 (m_cachedInstancingTransformVersions.size() == nummeshslots);

			transformsDirty = !canReuseAcrossFrames;
			if (!transformsDirty) {
				for (unsigned int i = 0; i < nummeshslots; ++i) {
					RAS_MeshUser *mu = m_cachedMeshSlots[i]->m_meshUser;
					if (m_cachedInstancingTransformVersions[i] != mu->GetTransformVersion()) {
						transformsDirty = true;
						break;
					}
				}
			}
		}

		if (transformsDirty) {
			RAS_CPU_PROFILE_SCOPE(RAS_CPU_INSTANCINGBUFFER_UPDATE);
			buffer->Update(rasty, materialData->m_drawingMode, matPasIndex, m_cachedMeshSlots);
			m_lastInstancingUploadSequenceVersion = m_sequenceVersion;
			m_lastInstancingUploadSlotsCount = nummeshslots;
			m_lastInstancingUploadDrawingMode = static_cast<RAS_Rasterizer::DrawType>(materialData->m_drawingMode);
			m_lastInstancingUploadPassIndex = matPasIndex;
			m_lastInstancingUploadSort = true;
			m_lastInstancingUploadPnorm = pnorm;
			m_lastInstancingUploadPnormValid = true;
			m_cachedInstancingTransformVersions.resize(nummeshslots);
			for (unsigned int i = 0; i < nummeshslots; ++i) {
				m_cachedInstancingTransformVersions[i] = m_cachedMeshSlots[i]->m_meshUser->GetTransformVersion();
			}
			m_cachedInstancingTransformVersionsValid = true;
			didUpdateBuffer = true;
		}
	}
	else {
		bool transformsDirty = false;
		{
			RAS_CPU_PROFILE_SCOPE(RAS_CPU_DISPLAYARRAYBUCKET_INSTANCING_CHECK_DIRTY);
			const bool canReuseAcrossFrames = m_cachedInstancingTransformVersionsValid &&
											 (m_lastInstancingUploadSequenceVersion == m_sequenceVersion) &&
											 (m_lastInstancingUploadSlotsCount == nummeshslots) &&
											 (m_lastInstancingUploadDrawingMode == materialData->m_drawingMode) &&
											 (m_lastInstancingUploadPassIndex == matPasIndex) &&
											 !m_lastInstancingUploadSort &&
											 (m_cachedInstancingTransformVersions.size() == nummeshslots);

			transformsDirty = !canReuseAcrossFrames;
			if (!transformsDirty) {
				for (unsigned int i = 0; i < nummeshslots; ++i) {
					RAS_MeshUser *mu = m_activeMeshSlots[i]->m_meshUser;
					if (m_cachedInstancingTransformVersions[i] != mu->GetTransformVersion()) {
						transformsDirty = true;
						break;
					}
				}
			}
		}

		if (transformsDirty) {
			RAS_CPU_PROFILE_SCOPE(RAS_CPU_INSTANCINGBUFFER_UPDATE);
			buffer->Update(rasty, materialData->m_drawingMode, matPasIndex, m_activeMeshSlots);
			m_lastInstancingUploadSequenceVersion = m_sequenceVersion;
			m_lastInstancingUploadSlotsCount = nummeshslots;
			m_lastInstancingUploadDrawingMode = static_cast<RAS_Rasterizer::DrawType>(materialData->m_drawingMode);
			m_lastInstancingUploadPassIndex = matPasIndex;
			m_lastInstancingUploadSort = false;
			m_lastInstancingUploadPnormValid = false;
			m_cachedInstancingTransformVersions.resize(nummeshslots);
			for (unsigned int i = 0; i < nummeshslots; ++i) {
				m_cachedInstancingTransformVersions[i] = m_activeMeshSlots[i]->m_meshUser->GetTransformVersion();
			}
			m_cachedInstancingTransformVersionsValid = true;
			didUpdateBuffer = true;
		}
	}

	RAS_AttributeArrayStorage *attribStorage = m_nodeData.m_attribStorage;
	{
		RAS_CPU_PROFILE_SCOPE(RAS_CPU_DISPLAYARRAYBUCKET_BINDATTRIBS);
		attribStorage->BindPrimitives();
	}

	buffer->Bind();

	// Bind all vertex attributs for the used material and the given buffer offset.
	{
		RAS_CPU_PROFILE_SCOPE(RAS_CPU_DISPLAYARRAYBUCKET_ACTIVATEINSTANCING);
		if (managerData->m_shaderOverride) {
			rasty->ActivateOverrideShaderInstancing(buffer);
		}
		else {
			material->ActivateInstancing(rasty, buffer);
		}
	}

	/* Because the geometry instancing use setting for all instances we use the original alpha blend.
	 * This requierd that the user use "alpha blend" mode if he will mutate object color alpha.
	 */
	rasty->SetAlphaBlend(material->GetAlphaBlend());

	/* It's a major issue of the geometry instancing : we can't manage face wise.
	 * To be sure we don't use the old face wise we force it to true. */
	rasty->SetFrontFace(true);

	// Unbind the buffer to avoid conflict with the render after.
	buffer->Unbind();

	{
		RAS_CPU_PROFILE_SCOPE(RAS_CPU_DISPLAYARRAYBUCKET_DRAWINSTANCED);
		m_arrayStorage->IndexPrimitivesInstancing(nummeshslots);
	}
	if (didUpdateBuffer) {
		buffer->SubmitFence();
	}

	// Unbind attributes, both array attributes and instancing attributes.
	attribStorage->UnbindPrimitives();
}

void RAS_DisplayArrayBucket::RunBatchingNode(const RAS_DisplayArrayNodeTuple& tuple)
{
	RAS_CPU_PROFILE_SCOPE(RAS_CPU_DISPLAYARRAYBUCKET_RUNBATCHINGNODE);
	RAS_ManagerNodeData *managerData = tuple.m_managerData;
	RAS_MaterialNodeData *materialData = tuple.m_materialData;

	RAS_IMaterial *material = materialData->m_material;
	const unsigned int nummeshslots = m_activeMeshSlots.size();
	if (nummeshslots == 0) {
		return;
	}

	RAS_BatchDisplayArray *batchArray = static_cast<RAS_BatchDisplayArray *>(m_displayArray);

	/* If the material use the transparency we must sort all mesh slots depending on the distance.
	 * This code share the code used in RAS_BucketManager to do the sort.
	 */
	if (managerData->m_sort && nummeshslots > 1) {
		KX_KetsjiEngine *engine = KX_GetActiveEngine();
		const bool hasFrameStamp = (engine != nullptr);
		const uint64_t frameId = hasFrameStamp ? engine->GetFrameCounter() : UINT64_MAX;
		const mt::mat3x4& trans = managerData->m_trans;
		const mt::vec3 pnorm(trans[2], trans[5], trans[8]);
		const bool canReuseSortedOrder = hasFrameStamp &&
		                                 m_lastBatchSortPnormValid &&
		                                 (m_lastBatchSortFrameId == frameId) &&
		                                 (m_lastBatchSortSequenceVersion == m_sequenceVersion) &&
		                                 (m_lastBatchSortSlotsCount == nummeshslots) &&
		                                 (m_lastBatchSortPnorm[0] == pnorm[0]) &&
		                                 (m_lastBatchSortPnorm[1] == pnorm[1]) &&
		                                 (m_lastBatchSortPnorm[2] == pnorm[2]);

		if (!canReuseSortedOrder) {
			m_cachedSortedMeshSlots.resize(nummeshslots);
			for (unsigned int i = 0; i < nummeshslots; ++i) {
				m_cachedSortedMeshSlots[i] = SortedMeshSlot(m_activeMeshSlots[i], pnorm);
			}
			std::sort(m_cachedSortedMeshSlots.begin(), m_cachedSortedMeshSlots.end(), RAS_BucketManager::backtofront());
			if (hasFrameStamp) {
				m_lastBatchSortFrameId = frameId;
				m_lastBatchSortSequenceVersion = m_sequenceVersion;
				m_lastBatchSortSlotsCount = nummeshslots;
				m_lastBatchSortPnorm = pnorm;
				m_lastBatchSortPnormValid = true;
			}
		}
		// Compute current sorted part indices and reuse previous mapping if unchanged.
		bool recomputeSorted = true;
		if (canReuseSortedOrder &&
		    m_lastSortedSlotsCount == nummeshslots &&
		    !m_cachedSortedCounts.empty() &&
		    !m_cachedSortedIndices.empty()) {
			recomputeSorted = false;
		}
		if (recomputeSorted) {
			m_currentPartIndicesTmp.resize(nummeshslots);
			for (unsigned int i = 0; i < nummeshslots; ++i) {
				m_currentPartIndicesTmp[i] = m_cachedSortedMeshSlots[i].m_ms->m_batchPartIndex;
			}
			if (m_lastSortedSlotsCount == nummeshslots &&
			    m_currentPartIndicesTmp == m_cachedSortedPartIndicesSnapshot &&
			    !m_cachedSortedCounts.empty() &&
			    !m_cachedSortedIndices.empty()) {
				recomputeSorted = false;
			}
		}
		if (recomputeSorted) {
			m_cachedSortedIndices.resize(nummeshslots);
			m_cachedSortedCounts.resize(nummeshslots);
			for (unsigned int i = 0; i < nummeshslots; ++i) {
				const short index = m_currentPartIndicesTmp[i];
				m_cachedSortedIndices[i] = batchArray->GetPartIndexOffset(index);
				m_cachedSortedCounts[i] = batchArray->GetPartIndexCount(index);
			}
			m_cachedSortedPartIndicesSnapshot = m_currentPartIndicesTmp;
			m_lastSortedSlotsCount = nummeshslots;
		}
	}
	else {
		bool recompute = true;
		if (m_batchNoSortCacheValid && (m_sequenceVersion == m_lastSequenceVersion) && !m_cachedCoalescedCounts.empty()) {
			recompute = false;
		}
		if (recompute) {
			m_currentPartIndicesTmp.resize(nummeshslots);
			for (unsigned int i = 0; i < nummeshslots; ++i) {
				m_currentPartIndicesTmp[i] = m_activeMeshSlots[i]->m_batchPartIndex;
			}
			if (m_batchNoSortCacheValid && !m_cachedCoalescedCounts.empty() &&
			    m_currentPartIndicesTmp == m_cachedPartIndicesSnapshot) {
				recompute = false;
			}
		}
		if (recompute) {
			m_cachedCoalescedIndices.clear();
			m_cachedCoalescedCounts.clear();
			if (nummeshslots > 0) {
				bool nonDecreasing = true;
				for (unsigned int i = 1; i < nummeshslots; ++i) {
					if (m_currentPartIndicesTmp[i] < m_currentPartIndicesTmp[i - 1]) {
						nonDecreasing = false;
						break;
					}
				}
				if (nonDecreasing) {
					const intptr_t indexStride = sizeof(unsigned int);
					short partIndex0 = m_currentPartIndicesTmp[0];
					intptr_t curOff = batchArray->GetPartIndexOffset(partIndex0);
					int curCount = batchArray->GetPartIndexCount(partIndex0);
					for (unsigned int i = 1; i < nummeshslots; ++i) {
						const short pidx = m_currentPartIndicesTmp[i];
						const intptr_t nextOff = batchArray->GetPartIndexOffset(pidx);
						const int nextCnt = batchArray->GetPartIndexCount(pidx);
						if (nextOff == curOff + static_cast<intptr_t>(curCount) * indexStride) {
							curCount += nextCnt;
						} else {
							m_cachedCoalescedIndices.push_back(curOff);
							m_cachedCoalescedCounts.push_back(curCount);
							curOff = nextOff;
							curCount = nextCnt;
						}
					}
					m_cachedCoalescedIndices.push_back(curOff);
					m_cachedCoalescedCounts.push_back(curCount);
				} else {
					m_cachedRanges.resize(nummeshslots);
					for (unsigned int i = 0; i < nummeshslots; ++i) {
						const short pidx = m_currentPartIndicesTmp[i];
						m_cachedRanges[i].first = batchArray->GetPartIndexOffset(pidx);
						m_cachedRanges[i].second = batchArray->GetPartIndexCount(pidx);
					}
					std::sort(m_cachedRanges.begin(), m_cachedRanges.end(),
					          [](const std::pair<intptr_t, int>& a, const std::pair<intptr_t, int>& b) {
						return a.first < b.first;
					});
					const intptr_t indexStride = sizeof(unsigned int);
					intptr_t curOff = m_cachedRanges[0].first;
					int curCount = m_cachedRanges[0].second;
					for (unsigned int i = 1; i < nummeshslots; ++i) {
						const intptr_t nextOff = m_cachedRanges[i].first;
						const int nextCnt = m_cachedRanges[i].second;
						if (nextOff == curOff + static_cast<intptr_t>(curCount) * indexStride) {
							curCount += nextCnt;
						} else {
							m_cachedCoalescedIndices.push_back(curOff);
							m_cachedCoalescedCounts.push_back(curCount);
							curOff = nextOff;
							curCount = nextCnt;
						}
					}
					m_cachedCoalescedIndices.push_back(curOff);
					m_cachedCoalescedCounts.push_back(curCount);
				}
			}
			m_cachedPartIndicesSnapshot.swap(m_currentPartIndicesTmp);
			m_batchNoSortCacheValid = true;
			m_lastSequenceVersion = m_sequenceVersion;
		}
	}

	RAS_Rasterizer *rasty = managerData->m_rasty;
    /*
	{
		const bool isSortPath = (managerData->m_sort != 0);
		const size_t segCount = isSortPath ? m_cachedSortedCounts.size() : m_cachedCoalescedCounts.size();
		std::fprintf(stderr,
		             "[Dbg] RunBatchingNode ENTER path=%s slots=%u segments=%zu\n",
		             isSortPath ? "sort" : "no-sort",
		             nummeshslots,
		             segCount);
		// Dump first up to 3 segments for inspection.
		const size_t dumpN = segCount < 3 ? segCount : 3;
		for (size_t i = 0; i < dumpN; ++i) {
			if (isSortPath) {
				std::fprintf(stderr, "[Dbg]  seg[%zu]: off=%lld cnt=%d\n",
				             i,
				             static_cast<long long>(m_cachedSortedIndices[i]),
				             m_cachedSortedCounts[i]);
			} else {
				std::fprintf(stderr, "[Dbg]  seg[%zu]: off=%lld cnt=%d\n",
				             i,
				             static_cast<long long>(m_cachedCoalescedIndices[i]),
				             m_cachedCoalescedCounts[i]);
			}
		}
	}
    */

	rasty->SetAlphaBlend(material->GetAlphaBlend());
	rasty->SetFrontFace(true);

	// Tenta usar o grupo cacheado se possível.
	RAS_BatchGroup *group = m_cachedBatchGroup;
	if (!group) {
		group = m_activeMeshSlots.front()->m_meshUser->GetBatchGroup();
		m_cachedBatchGroup = group;
		if (m_cachedBatchGroup) {
			m_cachedBatchGroup->AddMeshUser();
		}
	}

	if (group) {
		RAS_MeshUser *referenceMeshUser = group->GetReferenceMeshUser();
		const mt::mat4& objMatrix = referenceMeshUser->GetMatrix();
		const mt::vec4 color = referenceMeshUser->GetColor();
		const unsigned int layer = referenceMeshUser->GetLayer();
		const float random = referenceMeshUser->GetRandom();
		const short passIndex = referenceMeshUser->GetPassIndex();

		rasty->SetClientObject(referenceMeshUser->GetClientObject());

		const bool needsUpdate = rasty->NeedsMeshUserUpdate(material, objMatrix, color, layer, random, passIndex);
		if (needsUpdate) {
			material->ActivateMeshUser(referenceMeshUser, rasty, managerData->m_trans, true);
		}
	}

	RAS_AttributeArrayStorage *attribStorage = m_nodeData.m_attribStorage;
	attribStorage->BindPrimitives();


	if (!managerData->m_sort && nummeshslots == 1) {
		const short pidx = m_activeMeshSlots[0]->m_batchPartIndex;
		const intptr_t off = batchArray->GetPartIndexOffset(pidx);
		const int cnt = batchArray->GetPartIndexCount(pidx);
		m_cachedCoalescedIndices.resize(1);
		m_cachedCoalescedCounts.resize(1);
		m_cachedCoalescedIndices[0] = off;
		m_cachedCoalescedCounts[0] = cnt;
		m_arrayStorage->IndexPrimitivesBatching(m_cachedCoalescedIndices, m_cachedCoalescedCounts);
		attribStorage->UnbindPrimitives();
		return;
	}

	if (!managerData->m_sort) {

		m_arrayStorage->IndexPrimitivesBatching(m_cachedCoalescedIndices, m_cachedCoalescedCounts);
	} else {
		m_arrayStorage->IndexPrimitivesBatching(m_cachedSortedIndices, m_cachedSortedCounts);
	}

	attribStorage->UnbindPrimitives();
}

void RAS_DisplayArrayBucket::ChangeMaterialBucket(RAS_MaterialBucket *bucket)
{
	m_bucket = bucket;

	// Change of material update looking.
	RAS_IMaterial *material = bucket->GetMaterial();
	material->MoveUpdateClient(&m_materialUpdateClient, RAS_IMaterial::ATTRIBUTES_MODIFIED | RAS_IMaterial::SHADER_MODIFIED);

	// Instancing buffers are linked to material attributes, invalid them.
	for (unsigned short i = 0; i < RAS_Rasterizer::RAS_DRAW_MAX; ++i) {
		m_instancingBuffer[i].reset(nullptr);
	}
	std::fill_n(m_cachedAttribStorageComputed, RAS_Rasterizer::RAS_DRAW_MAX, false);
	m_batchNoSortCacheValid = false;
	++m_sequenceVersion;
	m_lastUpdateFrameId = UINT64_MAX;
}
