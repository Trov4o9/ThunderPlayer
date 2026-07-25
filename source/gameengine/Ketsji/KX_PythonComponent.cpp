/**
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
 * Contributor(s): Mitchell Stokes, Diego Lopes, Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#ifdef WITH_PYTHON

#include "KX_PythonComponent.h"
#include "KX_GameObject.h"

#include "CM_Message.h"

#include "DNA_python_component_types.h"

#include "BKE_python_component.h"

KX_PythonComponent::KX_PythonComponent(const std::string& name)
	:m_pc(nullptr),
	m_gameobj(nullptr),
	m_name(name),
	m_startFunc(nullptr),
	m_updateFunc(nullptr),
	m_disposeFunc(nullptr),
	m_init(false),
	m_disposeCalled(false),
	m_disposeFinished(false)
{
}

KX_PythonComponent::~KX_PythonComponent()
{
	// Dispose may have already been called explicitly (from NewRemoveObject).
	// Only call here as fallback -- at this point references may be stale.
	if (!m_disposeFinished) {
		Dispose();
	}
	Py_XDECREF(m_startFunc);
	Py_XDECREF(m_updateFunc);
	Py_XDECREF(m_disposeFunc);
}

std::string KX_PythonComponent::GetName()
{
	return m_name;
}

EXP_Value *KX_PythonComponent::GetReplica()
{
	KX_PythonComponent *replica = new KX_PythonComponent(*this);
	replica->ProcessReplica();

	// Subclass the python component.
	PyTypeObject *type = Py_TYPE(GetProxy());
	if (!py_base_new(type, PyTuple_Pack(1, replica->GetProxy()), nullptr)) {
		CM_Error("failed replicate component: \"" << m_name << "\"");
		delete replica;
		return nullptr;
	}

	return replica;
}

void KX_PythonComponent::ProcessReplica()
{
	EXP_Value::ProcessReplica();
	m_gameobj = nullptr;
	m_startFunc = nullptr;
	m_updateFunc = nullptr;
	m_disposeFunc = nullptr;
	m_init = false;
	m_disposeCalled = false;
	m_disposeFinished = false;
}

KX_GameObject *KX_PythonComponent::GetGameObject() const
{
	return m_gameobj;
}

void KX_PythonComponent::SetGameObject(KX_GameObject *gameobj)
{
	m_gameobj = gameobj;
}

void KX_PythonComponent::SetBlenderPythonComponent(PythonComponent *pc)
{
	m_pc = pc;
}

void KX_PythonComponent::Start()
{
	// Cache start function on first call
	if (!m_startFunc) {
		m_startFunc = PyObject_GetAttrString(GetProxy(), "start");
		if (!m_startFunc || !PyCallable_Check(m_startFunc)) {
			Py_XDECREF(m_startFunc);
			m_startFunc = nullptr;
			PyErr_Clear();
			return;
		}
	}

	PyObject *arg_dict = (PyObject *)BKE_python_component_argument_dict_new(m_pc);
	PyObject *ret = PyObject_CallFunctionObjArgs(m_startFunc, arg_dict, nullptr);

	if (PyErr_Occurred()) {
		PyErr_Print();
	}

	Py_XDECREF(arg_dict);
	Py_XDECREF(ret);
}

void KX_PythonComponent::Update()
{
	if (!m_init) {
		Start();
		m_init = true;
	}

	// Cache update function on first call
	if (!m_updateFunc) {
		m_updateFunc = PyObject_GetAttrString(GetProxy(), "update");
		if (!m_updateFunc || !PyCallable_Check(m_updateFunc)) {
			Py_XDECREF(m_updateFunc);
			m_updateFunc = nullptr;
			PyErr_Clear();
			return;
		}
	}

	PyObject *ret = PyObject_CallFunctionObjArgs(m_updateFunc, nullptr);
	if (!ret) {
		PyErr_Print();
	}
	Py_XDECREF(ret);
}

void KX_PythonComponent::Dispose()
{
	// Guard: already finished or currently executing (re-entrancy protection)
	if (m_disposeFinished) {
		return;
	}
	if (m_disposeCalled) {
		return;
	}


	if (!m_init) {
		m_disposeFinished = true;
		return;
	}
	// Mark as in-progress to prevent re-entrancy
	m_disposeCalled = true;

	// Cache dispose function
	if (!m_disposeFunc) {
		m_disposeFunc = PyObject_GetAttrString(GetProxy(), "dispose");
		if (!m_disposeFunc || !PyCallable_Check(m_disposeFunc)) {
			Py_XDECREF(m_disposeFunc);
			m_disposeFunc = nullptr;
			PyErr_Clear();
			m_disposeFinished = true;
			return;
		}
	}

	// Call Python dispose()
	PyObject *ret = PyObject_CallFunctionObjArgs(m_disposeFunc, nullptr);
	if (!ret) {
		PyErr_Print();
	}
	Py_XDECREF(ret);

	// Only after Python call completes, mark fully finished
	m_disposeFinished = true;
}

PyObject *KX_PythonComponent::py_component_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
	KX_PythonComponent *comp = new KX_PythonComponent(type->tp_name);

	PyObject *proxy = py_base_new(type, PyTuple_Pack(1, comp->GetProxy()), kwds);
	if (!proxy) {
		delete comp;
		return nullptr;
	}

	return proxy;
}

PyTypeObject KX_PythonComponent::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_PythonComponent",
	sizeof(EXP_PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0, 0, 0, 0, 0, 0, 0, 0, 0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&EXP_PyObjectPlus::Type,
	0, 0, 0, 0, 0, 0,
	py_component_new
};

PyMethodDef KX_PythonComponent::Methods[] = {
	{nullptr, nullptr} // Sentinel
};

PyAttributeDef KX_PythonComponent::Attributes[] = {
	EXP_PYATTRIBUTE_RO_FUNCTION("object", KX_PythonComponent, pyattr_get_object),
	EXP_PYATTRIBUTE_NULL // Sentinel
};

PyObject *KX_PythonComponent::pyattr_get_object(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_PythonComponent *self = static_cast<KX_PythonComponent *>(self_v);
	KX_GameObject *gameobj = self->GetGameObject();

	if (gameobj) {
		return gameobj->GetProxy();
	}
	else {
		Py_RETURN_NONE;
	}
}
#endif
