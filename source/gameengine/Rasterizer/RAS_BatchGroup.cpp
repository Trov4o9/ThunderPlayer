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
 * Contributor(s): Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Rasterizer/RAS_BatchGroup.cpp
 *  \ingroup bgerast
 */

#include "RAS_BatchGroup.h"
#include "RAS_BatchDisplayArray.h"
#include "RAS_IMaterial.h"
#include "RAS_MaterialBucket.h"
#include "RAS_MeshUser.h"
#include "RAS_MeshSlot.h"

#include "CM_Message.h"
#include "CM_List.h"

#include <algorithm>

RAS_BatchGroup::Batch::Batch()
	:m_displayArrayBucket(nullptr),
	m_displayArray(nullptr)
{
}

RAS_BatchGroup::RAS_BatchGroup()
	:m_users(0),
	m_referenceMeshUser(nullptr)
{
}

RAS_BatchGroup::~RAS_BatchGroup()
{
	for (RAS_MeshUser *mu : m_meshUsers) {
		mu->SetBatchGroup(nullptr);
	}
	m_meshUsers.clear();

	for (const auto& pair : m_batchs) {
		const Batch& batch = pair.second;
		delete batch.m_displayArrayBucket;
	}
}

RAS_BatchGroup *RAS_BatchGroup::AddMeshUser()
{
	++m_users;
	return this;
}

RAS_BatchGroup *RAS_BatchGroup::RemoveMeshUser()
{
	--m_users;
	if (m_users == 0) {
		Reset();
	}
	return this;
}

void RAS_BatchGroup::Reset()
{
	m_meshUsers.clear();

	for (auto& pair : m_batchs) {
		delete pair.second.m_displayArrayBucket;
	}
	m_batchs.clear();

	m_shadowSlotsIndices.clear();
	m_referenceMeshUser = nullptr;
	m_users = 0;
	m_castsShadows = false;
}

RAS_MeshUser *RAS_BatchGroup::GetReferenceMeshUser() const
{
	return m_referenceMeshUser;
}

void RAS_BatchGroup::SetReferenceMeshUser(RAS_MeshUser *meshUser)
{
	m_referenceMeshUser = meshUser;
}

bool RAS_BatchGroup::MergeMeshSlot(RAS_BatchGroup::Batch& batch, RAS_MeshSlot& slot, const mt::mat4& mat)
{
	RAS_DisplayArrayBucket *origArrayBucket = slot.m_displayArrayBucket;
	RAS_DisplayArray *origArray = origArrayBucket->GetDisplayArray();
	RAS_DisplayArrayBucket *arrayBucket = batch.m_displayArrayBucket;
	RAS_BatchDisplayArray *array = batch.m_displayArray;

	if (batch.m_originalDisplayArrayBucketList.find(&slot) != batch.m_originalDisplayArrayBucketList.end()) {
		CM_Error("could not merge twice a mesh");
		return false;
	}

	// Don't merge if the vertex format or pimitive type is not the same.
	if (origArray->GetFormat() != array->GetFormat() || origArray->GetPrimitiveType() != array->GetPrimitiveType()) {
		CM_Error("could not merge incompatible vertex format or primitive type")
		return false;
	}

	// Store original display array bucket.
	batch.m_originalDisplayArrayBucketList[&slot] = origArrayBucket;
	batch.m_meshSlots.push_back(&slot);

	// Merge display array.
	const unsigned int index = array->Merge(origArray, mat);
	slot.m_batchPartIndex = index;

	slot.SetDisplayArrayBucket(arrayBucket);

	return true;
}

void RAS_BatchGroup::ActivateShadowForGroup()
{
	for (RAS_MeshUser *mu : m_meshUsers) {
		if (mu) {
			mu->ActivateShadowMeshSlots();
		}
	}
}

bool RAS_BatchGroup::SplitMeshSlot(RAS_MeshSlot& slot)
{
	RAS_MaterialBucket *bucket = slot.m_displayArrayBucket->GetBucket();
	RAS_IMaterial *material = bucket->GetMaterial();

	auto bit = m_batchs.find(material);
	if (bit == m_batchs.end()) {
		CM_Error("could not found corresponding material \"" << material->GetName() << "\"");
		return false;
	}

	Batch& batch = bit->second;

	const auto origIt = batch.m_originalDisplayArrayBucketList.find(&slot);
	if (origIt == batch.m_originalDisplayArrayBucketList.end() || !origIt->second) {
		CM_Error("could not restore mesh");
		return false;
	}
	RAS_DisplayArrayBucket *origArrayBucket = origIt->second;

	slot.SetDisplayArrayBucket(origArrayBucket);

	const int removedPartIndex = slot.m_batchPartIndex;
	batch.m_displayArray->Split(removedPartIndex);

	batch.m_originalDisplayArrayBucketList.erase(origIt);

	slot.m_batchPartIndex = -1;

	const size_t prevSize = batch.m_meshSlots.size();
	size_t startIndex = (removedPartIndex >= 0 && static_cast<size_t>(removedPartIndex) < prevSize &&
	                     batch.m_meshSlots[removedPartIndex] == &slot) ?
	                        static_cast<size_t>(removedPartIndex) :
	                        static_cast<size_t>(std::distance(batch.m_meshSlots.begin(),
	                                                          std::find(batch.m_meshSlots.begin(), batch.m_meshSlots.end(), &slot)));
	if (startIndex >= prevSize) {
		CM_Error("could not find mesh slot in batch");
		return false;
	}
	batch.m_meshSlots.erase(batch.m_meshSlots.begin() + startIndex);
	for (RAS_MeshSlotList::iterator it = batch.m_meshSlots.begin() + startIndex, end = batch.m_meshSlots.end(); it != end; ++it) {
		RAS_MeshSlot *meshSlot = *it;
		--meshSlot->m_batchPartIndex;
	}

	return true;
}

bool RAS_BatchGroup::MergeMeshUser(RAS_MeshUser *meshUser, const mt::mat4& mat)
{
	m_shadowSlotsIndices.clear();
	m_castsShadows = false;

	std::deque<RAS_MeshSlot>& meshSlots = meshUser->GetMeshSlots();
	m_shadowSlotsIndices.reserve(meshSlots.size());

	int slotIdx = 0;
	for (RAS_MeshSlot& meshSlot : meshSlots) {
		RAS_DisplayArrayBucket *arrayBucket = meshSlot.m_displayArrayBucket;
		RAS_MaterialBucket *bucket = arrayBucket->GetBucket();
		RAS_IMaterial *material = bucket->GetMaterial();

		if (material && material->CastsShadows()) {
			m_shadowSlotsIndices.push_back(slotIdx);
			m_castsShadows = true;
		}

		auto insertResult = m_batchs.emplace(material, Batch());
		Batch& batch = insertResult.first->second;
		// Create the batch if it is empty.
		if (!batch.m_displayArray && !batch.m_displayArrayBucket) {
			RAS_DisplayArray *origarray = arrayBucket->GetDisplayArray();
			batch.m_displayArray = new RAS_BatchDisplayArray(origarray->GetPrimitiveType(), origarray->GetFormat());
			batch.m_displayArrayBucket = new RAS_DisplayArrayBucket(bucket, batch.m_displayArray, arrayBucket->GetMesh(),
			                                                        arrayBucket->GetMeshMaterial(), nullptr);
		}

		if (!MergeMeshSlot(batch, meshSlot, mat)) {
			return false;
		}
		slotIdx++;
	}

	meshUser->SetBatchGroup(this);
	m_meshUsers.push_back(meshUser);

	return true;
}

bool RAS_BatchGroup::SplitMeshUser(RAS_MeshUser *meshUser)
{
	for (RAS_MeshSlot& meshSlot : meshUser->GetMeshSlots()) {
		if (!SplitMeshSlot(meshSlot)) {
			return false;
		}
	}

	// Deference batch groups by setting it to nullptr.
	meshUser->SetBatchGroup(nullptr);
	CM_ListRemoveIfFound(m_meshUsers, meshUser);

	return true;
}

void RAS_BatchGroup::Destruct()
{
	/* Add an user to be sure the batch group will not be deleted. Indeed all
	 * the mesh user will deference the batch group and then in the last iteration
	 * the batch will be deleted before leave the function. This caused to break
	 * the loop which needs to access m_batchs end iterator.
	 */
	AddMeshUser();

	for (auto& pair : m_batchs) {
		Batch& batch = pair.second;
		for (RAS_MeshSlot *slot : batch.m_meshSlots) {
			RAS_DisplayArrayBucket *origArrayBucket = batch.m_originalDisplayArrayBucketList[slot];

			slot->SetDisplayArrayBucket(origArrayBucket);

			slot->m_meshUser->SetBatchGroup(nullptr);

			slot->m_batchPartIndex = -1;
		}
	}

	m_batchs.clear();

	// Release here and destruct the batch group.
	RemoveMeshUser();
}
