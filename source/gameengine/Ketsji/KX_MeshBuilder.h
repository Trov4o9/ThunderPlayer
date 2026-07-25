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
 * Contributor(s): Tristan Porteries / Range Engine.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#ifndef __KX_MESH_BUILDER_H__
#define __KX_MESH_BUILDER_H__

#include "KX_Mesh.h"
#include "RAS_DisplayArray.h"

#include "EXP_ListValue.h"

class KX_BlenderMaterial;

class KX_MeshBuilderSlot : public EXP_Value
{
	Py_Header

private:
	KX_BlenderMaterial *m_material;
	RAS_DisplayArray *m_array;
	unsigned int& m_origIndexCounter;

public:
	KX_MeshBuilderSlot(KX_BlenderMaterial *material, RAS_DisplayArray::PrimitiveType primitiveType,
			const RAS_DisplayArray::Format& format, unsigned int& origIndexCounter);
	KX_MeshBuilderSlot(RAS_MeshMaterial *meshmat, const RAS_DisplayArray::Format& format, unsigned int& origIndexCounter);
	~KX_MeshBuilderSlot();

	virtual std::string GetName();

	KX_BlenderMaterial *GetMaterial() const;
	void SetMaterial(KX_BlenderMaterial *material);

	bool Invalid() const;

	RAS_DisplayArray *GetDisplayArray() const;
	RAS_DisplayArray *ExtractDisplayArray();

#ifdef WITH_PYTHON

	using GetSizeFunc = unsigned int (RAS_DisplayArray::*)() const;
	using GetIndexFunc = unsigned int (RAS_DisplayArray::*)(const unsigned int) const;

	template <GetSizeFunc Func>
	unsigned int get_size_cb();
	PyObject *get_item_vertices_cb(unsigned int index);
	template <GetIndexFunc Func>
	PyObject *get_item_indices_cb(unsigned int index);

	static PyObject *pyattr_get_vertices(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_indices(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_triangleIndices(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_material(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static int pyattr_set_material(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value);
	static PyObject *pyattr_get_uvCount(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_colorCount(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_primitive(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);

	EXP_PYMETHOD(KX_MeshBuilderSlot, AddVertex);
	EXP_PYMETHOD_VARARGS(KX_MeshBuilderSlot, RemoveVertex);
	EXP_PYMETHOD_O(KX_MeshBuilderSlot, AddIndex);
	EXP_PYMETHOD_O(KX_MeshBuilderSlot, AddPrimitiveIndex);
	EXP_PYMETHOD_VARARGS(KX_MeshBuilderSlot, RemovePrimitiveIndex);
	EXP_PYMETHOD_O(KX_MeshBuilderSlot, AddTriangleIndex);
	EXP_PYMETHOD_VARARGS(KX_MeshBuilderSlot, RemoveTriangleIndex);
	EXP_PYMETHOD_NOARGS(KX_MeshBuilderSlot, RecalculateNormals);

#endif
};

class KX_MeshBuilder : public EXP_Value
{
	Py_Header

private:
	std::string m_name;

	EXP_ListValue<KX_MeshBuilderSlot> m_slots;
	RAS_Mesh::LayersInfo m_layersInfo;
	RAS_DisplayArray::Format m_format;

	KX_Scene *m_scene;

	unsigned int m_origIndexCounter;

public:
	KX_MeshBuilder(const std::string& name, KX_Scene *scene, const RAS_Mesh::LayersInfo& layersInfo,
			const RAS_DisplayArray::Format& format);
	KX_MeshBuilder(const std::string& name, KX_Mesh *mesh);
	~KX_MeshBuilder();

	virtual std::string GetName();

	EXP_ListValue<KX_MeshBuilderSlot>& GetSlots();

#ifdef WITH_PYTHON

	static PyObject *pyattr_get_slots(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);

	EXP_PYMETHOD(KX_MeshBuilder, AddSlot);
	EXP_PYMETHOD_NOARGS(KX_MeshBuilder, Finish);

#endif
};

#endif
