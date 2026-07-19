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
 * The Original Code is: all of this file.
 *
 * Contributor(s): Porteries Tristan.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file RAS_MeshUser.cpp
 *  \ingroup bgerast
 */

#include "RAS_MeshUser.h"
#include "RAS_DisplayArrayBucket.h"
#include "RAS_MaterialBucket.h"
#include "RAS_IMaterial.h"
#include "RAS_BoundingBox.h"
#include "RAS_BatchGroup.h"
#include "RAS_Deformer.h"

#include "BLI_hash.h"
#include <cstring>

RAS_MeshUser::RAS_MeshUser(void *clientobj, RAS_BoundingBox *boundingBox, RAS_Deformer *deformer)
	:m_layer((1 << 20) - 1),
	m_passIndex(0),
	m_random(BLI_hash_int_2d((uintptr_t)clientobj, 0) / ((float)0xFFFFFFFF)),
	m_frontFace(true),
	m_color(mt::zero4),
	m_boundingBox(boundingBox),
	m_clientObject(clientobj),
	m_activationCacheValid(false),
	m_batchGroup(nullptr),
	m_deformer(deformer)
{
	BLI_assert(m_boundingBox);
	m_boundingBox->AddUser();
	m_transformVersion = 1;
	m_cachedPackedTransformNormalVersion = 0;
}

RAS_MeshUser::~RAS_MeshUser()
{
	for (RAS_MeshSlot& slot : m_meshSlots) {
		if (slot.m_displayArrayBucket) {
			slot.m_displayArrayBucket->RemoveActiveMeshSlot(&slot);
			slot.m_displayArrayBucket = nullptr;
		}
		slot.m_meshUser = nullptr;
	}

	m_meshSlots.clear();

	m_boundingBox->RemoveUser();

	if (m_batchGroup) {
		// Has the side effect to deference the batch group.
		m_batchGroup->SplitMeshUser(this);
	}
}

void RAS_MeshUser::NewMeshSlot(RAS_DisplayArrayBucket *arrayBucket)
{
	m_meshSlots.emplace_back(this, arrayBucket);
	m_activationCacheValid = false;
}

void RAS_MeshUser::InvalidateActivationCache()
{
	m_activationCacheValid = false;
}

unsigned int RAS_MeshUser::GetLayer() const
{
	return m_layer;
}

short RAS_MeshUser::GetPassIndex() const
{
	return m_passIndex;
}

float RAS_MeshUser::GetRandom() const
{
	return m_random;
}

bool RAS_MeshUser::GetFrontFace() const
{
	return m_frontFace;
}

const mt::vec4& RAS_MeshUser::GetColor() const
{
	return m_color;
}

const mt::mat4& RAS_MeshUser::GetMatrix() const
{
	return m_matrix;
}

const RAS_MeshUser::PackedTransform& RAS_MeshUser::GetPackedTransformNormal() const
{
	if (m_cachedPackedTransformNormalVersion == m_transformVersion) {
		return m_cachedPackedTransformNormal;
	}

	float mat[16];
	m_matrix.Pack(mat);

	m_cachedPackedTransformNormal.matrix[0] = mat[0];
	m_cachedPackedTransformNormal.matrix[1] = mat[4];
	m_cachedPackedTransformNormal.matrix[2] = mat[8];
	m_cachedPackedTransformNormal.matrix[3] = mat[1];
	m_cachedPackedTransformNormal.matrix[4] = mat[5];
	m_cachedPackedTransformNormal.matrix[5] = mat[9];
	m_cachedPackedTransformNormal.matrix[6] = mat[2];
	m_cachedPackedTransformNormal.matrix[7] = mat[6];
	m_cachedPackedTransformNormal.matrix[8] = mat[10];

	m_cachedPackedTransformNormal.position[0] = mat[12];
	m_cachedPackedTransformNormal.position[1] = mat[13];
	m_cachedPackedTransformNormal.position[2] = mat[14];

	m_cachedPackedTransformNormalVersion = m_transformVersion;
	return m_cachedPackedTransformNormal;
}

RAS_BoundingBox *RAS_MeshUser::GetBoundingBox() const
{
	return m_boundingBox;
}

void *RAS_MeshUser::GetClientObject() const
{
	return m_clientObject;
}

std::deque<RAS_MeshSlot>& RAS_MeshUser::GetMeshSlots()
{
	return m_meshSlots;
}

RAS_BatchGroup *RAS_MeshUser::GetBatchGroup() const
{
	return m_batchGroup;
}

RAS_Deformer *RAS_MeshUser::GetDeformer()
{
	return m_deformer.get();
}

void RAS_MeshUser::SetLayer(unsigned int layer)
{
	if (m_layer == layer)
		return;
	m_layer = layer;
}

void RAS_MeshUser::SetPassIndex(short index)
{
	if (m_passIndex == index)
		return;
	m_passIndex = index;
}

void RAS_MeshUser::SetFrontFace(bool frontFace)
{
	if (m_frontFace == frontFace)
		return;
	m_frontFace = frontFace;
}

void RAS_MeshUser::SetColor(const mt::vec4& color)
{
	if (m_color == color)
		return;
	m_color = color;
}

void RAS_MeshUser::SetMatrix(const mt::mat4& matrix)
{
	const bool changed = (std::memcmp(&m_matrix, &matrix, sizeof(mt::mat4)) != 0);
	if (m_batchGroup && changed) {
		m_batchGroup->SplitMeshUser(this);
	}
	if (!changed) {
		return;
	}
	m_matrix = matrix;
	++m_transformVersion;
}

void RAS_MeshUser::SetBatchGroup(RAS_BatchGroup *batchGroup)
{
	if (m_batchGroup) {
		m_batchGroup->RemoveMeshUser();
	}

	m_batchGroup = batchGroup;

	if (m_batchGroup) {
		m_batchGroup->AddMeshUser();
	}
}

void RAS_MeshUser::ActivateMeshSlots()
{
	if (m_meshSlots.empty()) {
		return;
	}

	for (RAS_MeshSlot& ms : m_meshSlots) {
		ms.m_displayArrayBucket->ActivateMesh(&ms);
	}
}

void RAS_MeshUser::ActivateShadowMeshSlots()
{
	printf("[ActivateShadowMeshSlots] Called, batchGroup=%p, cacheValid=%d\n",
	       (void*)m_batchGroup, m_activationCacheValid);
	
	if (m_batchGroup) {
		const std::vector<int>& indices = m_batchGroup->GetShadowSlotsIndices();
		printf("[ActivateShadowMeshSlots]   Using batch group, shadow slot count: %d\n", (int)indices.size());
		
		if (indices.empty()) {
			printf("[ActivateShadowMeshSlots]   WARNING: No shadow slots in batch group!\n");
			return;
		}
		
		for (int idx : indices) {
			printf("[ActivateShadowMeshSlots]     Activating slot %d\n", idx);
			m_meshSlots[idx].m_displayArrayBucket->ActivateMesh(&m_meshSlots[idx]);
		}
		return;
	}

	if (!m_activationCacheValid) {
		printf("[ActivateShadowMeshSlots]   Cache invalid, rebuilding...\n");
		BuildActivationCache();
	}

	printf("[ActivateShadowMeshSlots]   Cached shadow slots: %d (total slots: %d)\n",
	       (int)m_cachedShadowSlotIndices.size(), (int)m_meshSlots.size());
	
	if (m_cachedShadowSlotIndices.empty()) {
		printf("[ActivateShadowMeshSlots]   WARNING: No cached shadow slots!\n");
	}

	for (int idx : m_cachedShadowSlotIndices) {
		printf("[ActivateShadowMeshSlots]     Activating cached slot %d\n", idx);
		RAS_MeshSlot& ms = m_meshSlots[idx];
		ms.m_displayArrayBucket->ActivateMesh(&ms);
	}
}

void RAS_MeshUser::ActivateMeshSlotsNoOnlyShadow()
{
	if (m_meshSlots.empty()) {
		return;
	}

	if (!m_activationCacheValid) {
		BuildActivationCache();
	}

	for (int idx : m_cachedNoOnlyShadowSlotIndices) {
		RAS_MeshSlot& ms = m_meshSlots[idx];
		ms.m_displayArrayBucket->ActivateMesh(&ms);
	}
}

void RAS_MeshUser::BuildActivationCache()
{
	printf("[BuildActivationCache] Building cache for %d slots\n", (int)m_meshSlots.size());
	
	m_cachedShadowSlotIndices.clear();
	m_cachedNoOnlyShadowSlotIndices.clear();

	const size_t n = m_meshSlots.size();
	m_cachedShadowSlotIndices.reserve(n);
	m_cachedNoOnlyShadowSlotIndices.reserve(n);

	for (size_t i = 0; i < n; ++i) {
		RAS_MeshSlot& ms = m_meshSlots[i];
		RAS_MaterialBucket *bucket = ms.m_displayArrayBucket->GetBucket();
		RAS_IMaterial *mat = bucket ? bucket->GetMaterial() : nullptr;
		
		if (mat) {
			bool castsShadows = mat->CastsShadows();
			bool onlyShadow = mat->OnlyShadow();
			printf("[BuildActivationCache]   Slot %d: castsShadows=%d, onlyShadow=%d\n",
			       (int)i, castsShadows, onlyShadow);
			
			if (castsShadows) {
				m_cachedShadowSlotIndices.push_back(static_cast<int>(i));
			}
			if (!onlyShadow) {
				m_cachedNoOnlyShadowSlotIndices.push_back(static_cast<int>(i));
			}
		}
		else {
			printf("[BuildActivationCache]   Slot %d: no material!\n", (int)i);
			m_cachedNoOnlyShadowSlotIndices.push_back(static_cast<int>(i));
		}
	}
	
	printf("[BuildActivationCache] Cache built: shadowSlots=%d, noOnlyShadowSlots=%d\n",
	       (int)m_cachedShadowSlotIndices.size(), (int)m_cachedNoOnlyShadowSlotIndices.size());

	m_activationCacheValid = true;
}
