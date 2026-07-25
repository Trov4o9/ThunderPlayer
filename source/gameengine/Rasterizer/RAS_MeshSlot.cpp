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

/** \file gameengine/Rasterizer/RAS_MeshSlot.cpp
 *  \ingroup bgerast
 */

#include "RAS_MeshSlot.h"
#include "RAS_MeshUser.h"
#include "RAS_IMaterial.h"
#include "RAS_DisplayArray.h"
#include "RAS_DisplayArrayStorage.h"
#include "RAS_Mesh.h"

#ifdef _MSC_VER
#  pragma warning (disable:4786)
#endif

#ifdef WIN32
#  include <windows.h>
#endif // WIN32

static RAS_DummyNodeData dummyNodeData;

SortedMeshSlot::SortedMeshSlot(RAS_MeshSlot *ms, const mt::vec3& pnorm)
	:m_ms(ms)
{
	// would be good to use the actual bounding box center instead
	const mt::vec3 pos = m_ms->m_meshUser->GetMatrix().TranslationVector3D();

	m_z = mt::dot(pnorm, pos);
}

SortedMeshSlot::SortedMeshSlot(RAS_MeshSlotUpwardNode *node, const mt::vec3& pnorm)
	:m_node(node)
{
	RAS_MeshSlot *ms = m_node->GetOwner();
	// would be good to use the actual bounding box center instead
	const mt::vec3 pos = ms->m_meshUser->GetMatrix().TranslationVector3D();

	m_z = mt::dot(pnorm, pos);
}

// mesh slot
RAS_MeshSlot::RAS_MeshSlot(RAS_MeshUser *meshUser, RAS_DisplayArrayBucket *arrayBucket)
	:m_node(this, &dummyNodeData, &RAS_MeshSlot::RunNode, nullptr),
	m_displayArrayBucket(arrayBucket),
	m_meshUser(meshUser),
	m_batchPartIndex(-1)
{
}

RAS_MeshSlot::~RAS_MeshSlot()
{
}

RAS_MeshSlot::RAS_MeshSlot(const RAS_MeshSlot& other)
	:m_node(this, &dummyNodeData, &RAS_MeshSlot::RunNode, nullptr),
	m_displayArrayBucket(other.m_displayArrayBucket),
	m_meshUser(other.m_meshUser),
	m_batchPartIndex(other.m_batchPartIndex)
{
}

void RAS_MeshSlot::SetDisplayArrayBucket(RAS_DisplayArrayBucket *arrayBucket)
{
	m_displayArrayBucket = arrayBucket;
	if (m_meshUser) {
		m_meshUser->InvalidateActivationCache();
	}
}

void RAS_MeshSlot::GenerateTree(RAS_DisplayArrayUpwardNode& root, RAS_UpwardTreeLeafs& leafs)
{
	m_node.SetParent(&root);
	leafs.push_back(&m_node);
}

void RAS_MeshSlot::RunNode(const RAS_MeshSlotNodeTuple& tuple)
{
    auto *managerData = tuple.m_managerData;
    auto *materialData = tuple.m_materialData;
    auto *displayArrayData = tuple.m_displayArrayData;
    auto *rasty = managerData->m_rasty;

    RAS_MeshUser *mu = m_meshUser;
    RAS_IMaterial *mat = materialData->m_material;

    rasty->SetClientObject(mu->GetClientObject());
    rasty->SetFrontFace(mu->GetFrontFace());

    RAS_DisplayArrayStorage *storage = displayArrayData->m_arrayStorage;
    const mt::mat4& objMatrix = mu->GetMatrix();

    const auto color = mu->GetColor();
    const auto layer = mu->GetLayer();
    const auto random = mu->GetRandom();
    const auto pass = mu->GetPassIndex();

    const bool needsUpdate = rasty->NeedsMeshUserUpdate(mat, objMatrix, color, layer, random, pass);

    if (!managerData->m_shaderOverride) {
        if (needsUpdate) {
            mat->ActivateMeshUser(mu, rasty, managerData->m_trans, true);
        }

        if (needsUpdate && materialData->m_zsort && storage) {
            displayArrayData->m_array->SortPolygons(
                managerData->m_trans * mt::mat4::ToAffineTransform(objMatrix),
                storage->GetIndexMap());
            storage->FlushIndexMap();
        }
    }

    if (materialData->m_text) {
        rasty->PushMatrix();
        rasty->IndexPrimitivesText(this);
        rasty->PopMatrix();
    }
    else if (storage) {
        if (displayArrayData->m_applyMatrix) {
            rasty->PushMatrix();
            static thread_local float matBuf[16];
            if (needsUpdate) {
                rasty->GetTransform(objMatrix, materialData->m_drawingMode, matBuf);
            }
            rasty->MultMatrix(matBuf);
            storage->IndexPrimitives();
            rasty->PopMatrix();
        }
        else {
            storage->IndexPrimitives();
        }
    }
}
