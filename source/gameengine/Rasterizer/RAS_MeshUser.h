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

/** \file RAS_MeshUser.h
 *  \ingroup bgerast
 */

#ifndef __RAS_MESH_USER_H__
#define __RAS_MESH_USER_H__

#include "RAS_MeshSlot.h"

#include <cstdint>
#include <deque>
#include <memory>

class RAS_BoundingBox;
class RAS_BatchGroup;
class RAS_Deformer;

class RAS_MeshUser : public mt::SimdClassAllocator
{
public:
	struct PackedTransform
	{
		float matrix[9];
		float position[3];
	};

private:
	/// Lamp layer.
	unsigned int m_layer;
	/// Object pass index.
	short m_passIndex;
	/// Random value of this user.
	float m_random;
	/// OpenGL face wise.
	bool m_frontFace;
	/// Object color.
	mt::vec4 m_color;
	/// Object transformation matrix.
	mt::mat4 m_matrix;
	/// Versão do transform para cache de dados empacotados (incrementa quando m_matrix muda).
	std::uint32_t m_transformVersion;

	mutable PackedTransform m_cachedPackedTransformNormal;
	mutable std::uint32_t m_cachedPackedTransformNormalVersion;
	/// Bounding box corresponding to a mesh or deformer.
	RAS_BoundingBox *m_boundingBox;
	/// Client object owner of this mesh user.
	void *m_clientObject;
	/// Unique mesh slots used for render of this object.
	std::deque<RAS_MeshSlot> m_meshSlots;
	std::vector<int> m_cachedShadowSlotIndices;
	std::vector<int> m_cachedNoOnlyShadowSlotIndices;
	bool m_activationCacheValid;
	/// Possible batching groups shared between mesh users.
	RAS_BatchGroup *m_batchGroup;
	/// Deformer of this mesh user modifying the display array of the mesh slots.
	std::unique_ptr<RAS_Deformer> m_deformer;

	/// Persistent SSBO slot index (-1 if not allocated)
	int m_persistentSlot;
	/// Transform dirty flag
	bool m_transformDirty;
	/// Color dirty flag
	bool m_colorDirty;
	/// Last update frame
	unsigned long long m_lastUpdateFrame;

public:
	RAS_MeshUser(void *clientobj, RAS_BoundingBox *boundingBox, RAS_Deformer *deformer);
	virtual ~RAS_MeshUser();

	void NewMeshSlot(RAS_DisplayArrayBucket *arrayBucket);
	void InvalidateActivationCache();
	unsigned int GetLayer() const;
	short GetPassIndex() const;
	float GetRandom() const;
	bool GetFrontFace() const;
	const mt::vec4& GetColor() const;
	const mt::mat4& GetMatrix() const;
	const PackedTransform& GetPackedTransformNormal() const;
	inline std::uint32_t GetTransformVersion() const { return m_transformVersion; }
	RAS_BoundingBox *GetBoundingBox() const;
	void *GetClientObject() const;
	std::deque<RAS_MeshSlot>& GetMeshSlots();
	RAS_BatchGroup *GetBatchGroup() const;
	RAS_Deformer *GetDeformer();

	void SetLayer(unsigned int layer);
	void SetPassIndex(short index);
	void SetFrontFace(bool frontFace);
	void SetColor(const mt::vec4& color);
	void SetMatrix(const mt::mat4& matrix);
	void SetBatchGroup(RAS_BatchGroup *batchGroup);

	void ActivateMeshSlots();
	void ActivateShadowMeshSlots();
	void ActivateMeshSlotsNoOnlyShadow();
	
	// Persistent slot management
	int GetPersistentSlot() const { return m_persistentSlot; }
	void SetPersistentSlot(int slot) { m_persistentSlot = slot; }
	
	// Dirty tracking
	bool IsTransformDirty() const { return m_transformDirty; }
	bool IsColorDirty() const { return m_colorDirty; }
	void MarkTransformDirty() { m_transformDirty = true; }
	void MarkColorDirty() { m_colorDirty = true; }
	void MarkClean() { m_transformDirty = false; m_colorDirty = false; }
	
	unsigned long long GetLastUpdateFrame() const { return m_lastUpdateFrame; }
	void SetLastUpdateFrame(unsigned long long frame) { m_lastUpdateFrame = frame; }

private:
	void BuildActivationCache();
};

#endif  // __RAS_MESH_USER_H__
