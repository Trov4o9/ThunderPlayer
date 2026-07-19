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

/** \file gameengine/Rasterizer/RAS_MaterialBucket.cpp
 *  \ingroup bgerast
 */

#include "RAS_MaterialBucket.h"
#include "RAS_IMaterial.h"
#include "RAS_Rasterizer.h"
#include "RAS_Mesh.h"
#include "RAS_MeshUser.h"
#include "RAS_Deformer.h"
#include "RAS_BatchGroup.h"
#include "RAS_BucketManager.h"

#include "KX_BlenderMaterial.h"
#include "BL_BlenderShader.h"

#include "CM_List.h"
#include "CM_Message.h"
#include <algorithm>
#include <functional>

#ifdef _MSC_VER
#  pragma warning (disable:4786)
#endif

#ifdef WIN32
#  include <windows.h>
#endif // WIN32

RAS_MaterialBucket::RAS_MaterialBucket(RAS_IMaterial *mat, RAS_BucketManager* manager)
	:m_manager(manager),
	m_material(mat),
	m_lastActiveDisplayArrayBucketCount(0),
	m_isActive(false),
	m_downwardNode(this, &m_nodeData, &RAS_MaterialBucket::BindNode, &RAS_MaterialBucket::UnbindNode),
	m_upwardNode(this, &m_nodeData, &RAS_MaterialBucket::BindNode, &RAS_MaterialBucket::UnbindNode)
{
	m_nodeData.m_material = m_material;
	m_nodeData.m_drawingMode = m_material->GetDrawingMode();
	m_nodeData.m_cullFace = m_material->IsCullFace();
	m_nodeData.m_zsort = m_material->IsZSort();
	m_nodeData.m_text = m_material->IsText();
	m_nodeData.m_zoffset = m_material->GetZOffset();
	m_nodeData.m_instancing = m_material->UseInstancing();
}

RAS_MaterialBucket::~RAS_MaterialBucket()
{
}

RAS_IMaterial *RAS_MaterialBucket::GetMaterial() const
{
	return m_material;
}

bool RAS_MaterialBucket::IsAlpha() const
{
	return m_nodeData.m_material->IsAlpha();
}

bool RAS_MaterialBucket::IsZSort() const
{
	return m_nodeData.m_zsort;
}

bool RAS_MaterialBucket::IsWire() const
{
	return m_nodeData.m_material->IsWire();
}

bool RAS_MaterialBucket::UseInstancing() const
{
	return m_material->UseInstancing();
}

void RAS_MaterialBucket::RemoveActiveMeshSlots()
{
	if (!m_isActive) {
		return;
	}

	for (auto *arrayBucket : m_activeDisplayArrayBucketList) {
		arrayBucket->RemoveActiveMeshSlots();
	}
	m_lastActiveDisplayArrayBucketCount = m_activeDisplayArrayBucketList.size();
	m_activeDisplayArrayBucketList.clear();

	m_isActive = false;
}

bool RAS_MaterialBucket::IsActive() const
{
	return m_isActive;
}

void RAS_MaterialBucket::MarkActive()
{
	if (!m_isActive) {
		m_isActive = true;
		m_manager->MarkBucketActive(this);
	}
}

void RAS_MaterialBucket::MarkDisplayArrayBucketActive(RAS_DisplayArrayBucket *bucket)
{
	if (m_activeDisplayArrayBucketList.empty() && m_lastActiveDisplayArrayBucketCount > 0) {
		m_activeDisplayArrayBucketList.reserve(m_lastActiveDisplayArrayBucketCount);
	}
	m_activeDisplayArrayBucketList.push_back(bucket);
}

void RAS_MaterialBucket::ActivateMaterial(RAS_Rasterizer *rasty)
{
	RAS_CPU_PROFILE_SCOPE(RAS_CPU_MATERIALBUCKET_ACTIVATE);
	m_material->Activate(rasty);
}

void RAS_MaterialBucket::DesactivateMaterial(RAS_Rasterizer *rasty)
{
	m_material->Desactivate(rasty);
}

void RAS_MaterialBucket::GenerateTree(RAS_ManagerDownwardNode& downwardRoot, 
                                      RAS_ManagerUpwardNode& upwardRoot,
                                      RAS_UpwardTreeLeafs& upwardLeafs, 
                                      RAS_Rasterizer::DrawType drawingMode, 
                                      bool sort)
{
	RAS_CPU_PROFILE_SCOPE(RAS_CPU_MATERIALBUCKET_GENERATETREE);
    if (!m_isActive || m_activeDisplayArrayBucketList.empty()) {
        return;
    }

    m_nodeData.m_instancing = m_material->UseInstancing();
    const bool instancing = m_nodeData.m_instancing;

    for (RAS_DisplayArrayBucket *displayArrayBucket : m_activeDisplayArrayBucketList) {
        displayArrayBucket->GenerateTree(m_downwardNode, m_upwardNode, upwardLeafs, drawingMode, sort, instancing);
    }

    downwardRoot.AddChild(&m_downwardNode);

    if (sort) {
        m_upwardNode.SetParent(&upwardRoot);
    }
}

void RAS_MaterialBucket::BindNode(const RAS_MaterialNodeTuple& tuple)
{
	RAS_ManagerNodeData *managerData = tuple.m_managerData;
	RAS_Rasterizer *rasty = managerData->m_rasty;
	rasty->SetCullFace(m_nodeData.m_cullFace);
	rasty->SetPolygonOffset(managerData->m_drawingMode, -m_nodeData.m_zoffset, 0.0f);

	if (!managerData->m_shaderOverride) {
		if (!managerData->m_materialPrepared) {
			m_material->Prepare(managerData->m_rasty);
		}
		ActivateMaterial(managerData->m_rasty);
	}
	else {
		// In override mode (shadow rendering), we need to:
		// 1. Update the current GPUMaterial so the correct shadow shader is selected
		// 2. Rebind the shadow shader for THIS material
		
		// Get the GPUMaterial from the KX_BlenderMaterial and set it as current.
		KX_BlenderMaterial* blenderMat = dynamic_cast<KX_BlenderMaterial*>(m_material);
		if (blenderMat) {
			if (blenderMat->GetBlenderShader()) {
				GPUMaterial* gpuMat = blenderMat->GetBlenderShader()->GetGPUMaterial();
				rasty->SetCurrentGPUMaterial(gpuMat);
				
				// CRITICAL: Rebind the override shader for THIS material
				// This ensures each material gets its own shadow shader (custom or builtin)
				rasty->SetOverrideShader(rasty->GetOverrideShader());
			}
			else {
				rasty->SetCurrentGPUMaterial(nullptr);
				rasty->SetOverrideShader(rasty->GetOverrideShader());
			}
		}
		else {
			// Material without custom shader (or not a Blender material)
			rasty->SetCurrentGPUMaterial(nullptr);
			rasty->SetOverrideShader(rasty->GetOverrideShader());
		}
	}
}

void RAS_MaterialBucket::UnbindNode(const RAS_MaterialNodeTuple& tuple)
{
	RAS_ManagerNodeData *managerData = tuple.m_managerData;
	if (!managerData->m_shaderOverride) {
		DesactivateMaterial(managerData->m_rasty);
	}
}

void RAS_MaterialBucket::AddDisplayArrayBucket(RAS_DisplayArrayBucket *bucket)
{
	m_displayArrayBucketList.push_back(bucket);
}

void RAS_MaterialBucket::RemoveDisplayArrayBucket(RAS_DisplayArrayBucket *bucket)
{
	CM_ListRemoveIfFound(m_displayArrayBucketList, bucket);
	CM_ListRemoveIfFound(m_activeDisplayArrayBucketList, bucket);
}

RAS_DisplayArrayBucketList& RAS_MaterialBucket::GetDisplayArrayBucketList()
{
	return m_displayArrayBucketList;
}

void RAS_MaterialBucket::MoveDisplayArrayBucket(RAS_MeshMaterial *meshmat, RAS_MaterialBucket *bucket)
{
    auto it = std::remove_if(m_displayArrayBucketList.begin(), m_displayArrayBucketList.end(),
        [&](RAS_DisplayArrayBucket* dab) {
            if (dab->GetMeshMaterial() == meshmat) {
                dab->ChangeMaterialBucket(bucket);
                bucket->AddDisplayArrayBucket(dab);
                return true;
            }
            return false;
        });
    m_displayArrayBucketList.erase(it, m_displayArrayBucketList.end());
}

RAS_BatchGroup* RAS_MaterialBucket::GetOrCreateFrameBatchGroup(size_t key, RAS_MeshUser* refUser)
{
	return m_manager->GetOrCreateFrameBatchGroup(key, refUser);
}

static inline void hash_combine(size_t& seed, size_t v)
{
	seed ^= v + 0x9e3779b97f4a7c15ULL + (seed<<6) + (seed>>2);
}

size_t RAS_MaterialBucket::ComputeBatchKey(RAS_Mesh *mesh, RAS_DisplayArray *array, RAS_IMaterial *mat)
{
	size_t seed = 0;
	hash_combine(seed, reinterpret_cast<size_t>(mesh));
	if (array) {
		hash_combine(seed, static_cast<size_t>(array->GetPrimitiveType()));
		const RAS_DisplayArray::Format& fmt = array->GetFormat();
		hash_combine(seed, static_cast<size_t>(fmt.uvSize));
		hash_combine(seed, static_cast<size_t>(fmt.colorSize));
	}
	if (mat) {
		hash_combine(seed, static_cast<size_t>(mat->GetPassIndex()));
	}
	return seed;
}
