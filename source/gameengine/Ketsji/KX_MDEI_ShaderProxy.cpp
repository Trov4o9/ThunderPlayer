/*
 * KX_MDEI_ShaderProxy.cpp — Python-visible shader+uniform wrapper for MDEI objects.
 */

/* ── Debug flag ────────────────────────────────────────────────────────────
 * Set MDEI_PROXY_DEBUG to 1 to print setSource / setUniform calls to stderr.
 * Active by default as requested.
 * ─────────────────────────────────────────────────────────────────────── */
#ifndef MDEI_PROXY_DEBUG
#  define MDEI_PROXY_DEBUG 1
#endif

#ifdef WITH_PYTHON

#include "KX_MDEI_ShaderProxy.h"
#include "MDEI_Shader.h"

#include "EXP_PyObjectPlus.h"
#include "python_utildefines.h"
#include "KX_PyMath.h"         /* PyMatTo, PyVecTo */

#include "GPU_glew.h"          /* GLuint, GLuint64, GLEW_ARB_bindless_texture */
#include "GPU_texture.h"       /* GPUTexture* — needed for setSamplerArrayFromGPUTextures */

#include <cstdio>
#include <cstdint>             /* uintptr_t */
#include <cstring>
#include <vector>

/* ══════════════════════════════════════════════════════════════════════════
 * Python type table
 * ══════════════════════════════════════════════════════════════════════════ */

PyMethodDef KX_MDEI_ShaderProxy::Methods[] = {
	EXP_PYMETHODTABLE(KX_MDEI_ShaderProxy, setSource),
	EXP_PYMETHODTABLE(KX_MDEI_ShaderProxy, setSourceList),
	EXP_PYMETHODTABLE(KX_MDEI_ShaderProxy, isValid),
	/* scalar uniforms */
	EXP_PYMETHODTABLE(KX_MDEI_ShaderProxy, setUniform1f),
	EXP_PYMETHODTABLE(KX_MDEI_ShaderProxy, setUniform2f),
	EXP_PYMETHODTABLE(KX_MDEI_ShaderProxy, setUniform3f),
	EXP_PYMETHODTABLE(KX_MDEI_ShaderProxy, setUniform4f),
	EXP_PYMETHODTABLE(KX_MDEI_ShaderProxy, setUniform1i),
	EXP_PYMETHODTABLE(KX_MDEI_ShaderProxy, setUniform2i),
	EXP_PYMETHODTABLE(KX_MDEI_ShaderProxy, setUniform3i),
	EXP_PYMETHODTABLE(KX_MDEI_ShaderProxy, setUniform4i),
	/* list-based uniforms */
	EXP_PYMETHODTABLE(KX_MDEI_ShaderProxy, setUniformfv),
	EXP_PYMETHODTABLE(KX_MDEI_ShaderProxy, setUniformiv),
	/* matrix uniforms */
	EXP_PYMETHODTABLE(KX_MDEI_ShaderProxy, setUniformMatrix3),
	EXP_PYMETHODTABLE(KX_MDEI_ShaderProxy, setUniformMatrix4),
	/* ── classic samplers ─────────────────────────────────────────────── */
	EXP_PYMETHODTABLE(KX_MDEI_ShaderProxy, setSampler),
	EXP_PYMETHODTABLE(KX_MDEI_ShaderProxy, setSamplerArray),
	/* ── bindless samplers ────────────────────────────────────────────── */
	EXP_PYMETHODTABLE(KX_MDEI_ShaderProxy, setSamplerBindless),
	EXP_PYMETHODTABLE(KX_MDEI_ShaderProxy, setSamplerArrayFromGPUTextures),
	/* ── mipmap control ───────────────────────────────────────────────── */
	EXP_PYMETHODTABLE(KX_MDEI_ShaderProxy, setMipmapping),
	EXP_PYMETHODTABLE(KX_MDEI_ShaderProxy, updateMipmappingFilter),
	{nullptr, nullptr}
};

PyAttributeDef KX_MDEI_ShaderProxy::Attributes[] = {
	EXP_PYATTRIBUTE_NULL
};

PyTypeObject KX_MDEI_ShaderProxy::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_MDEI_ShaderProxy",
	sizeof(EXP_PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0, 0, 0, 0,
	py_base_repr,
	0, 0, 0, 0, 0, 0, 0, 0, 0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0, 0,
	&EXP_Value::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

/* ══════════════════════════════════════════════════════════════════════════
 * Constructor / Destructor
 * ══════════════════════════════════════════════════════════════════════════ */

KX_MDEI_ShaderProxy::KX_MDEI_ShaderProxy(MDEI_Shader *shader)
	: m_shader(shader)
{
#if MDEI_PROXY_DEBUG
	fprintf(stderr, "[MDEI_ShaderProxy] Created — shader=%p\n", (void *)shader);
#endif
}

KX_MDEI_ShaderProxy::~KX_MDEI_ShaderProxy()
{
#if MDEI_PROXY_DEBUG
	fprintf(stderr, "[MDEI_ShaderProxy] Destroyed\n");
#endif
}

std::string KX_MDEI_ShaderProxy::GetName()
{
	return "KX_MDEI_ShaderProxy";
}

/* ── Helper macro: guard against missing/invalid shader ─────────────────── */
#define MDEI_PROXY_GUARD(method_name) \
	if (!m_shader) { \
		PyErr_SetString(PyExc_RuntimeError, \
		    "KX_MDEI_ShaderProxy." method_name ": no MDEI_Shader attached"); \
		return nullptr; \
	}

/* ══════════════════════════════════════════════════════════════════════════
 * setSource / setSourceList / isValid
 * ══════════════════════════════════════════════════════════════════════════ */

EXP_PYMETHODDEF_DOC(KX_MDEI_ShaderProxy, setSource,
"setSource(vertexSrc, fragmentSrc)\n"
"Replace the MDEI solid shader with custom GLSL.\n"
"The SSBO instancing block is injected automatically.\n")
{
	MDEI_PROXY_GUARD("setSource");
	const char *vertSrc = nullptr, *fragSrc = nullptr;
	if (!PyArg_ParseTuple(args, "ss:setSource", &vertSrc, &fragSrc))
		return nullptr;

#if MDEI_PROXY_DEBUG
	fprintf(stderr, "[MDEI_ShaderProxy] setSource vert=%zu frag=%zu\n",
	        vertSrc ? strlen(vertSrc) : 0u, fragSrc ? strlen(fragSrc) : 0u);
#endif
	if (vertSrc && vertSrc[0]) m_shader->SetVertexSource(std::string(vertSrc));
	if (fragSrc && fragSrc[0]) m_shader->SetFragmentSource(std::string(fragSrc));
	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_MDEI_ShaderProxy, setSourceList,
"setSourceList({\"vertex\":str, \"fragment\":str})\n"
"Dict-based variant.  Both keys are required.\n")
{
	MDEI_PROXY_GUARD("setSourceList");
	PyObject *d = nullptr;
	if (!PyArg_ParseTuple(args, "O!:setSourceList", &PyDict_Type, &d))
		return nullptr;

	PyObject *pv = PyDict_GetItemString(d, "vertex");
	PyObject *pf = PyDict_GetItemString(d, "fragment");
	if (!pv || !PyUnicode_Check(pv)) {
		PyErr_SetString(PyExc_ValueError, "setSourceList: 'vertex' missing or not a string");
		return nullptr;
	}
	if (!pf || !PyUnicode_Check(pf)) {
		PyErr_SetString(PyExc_ValueError, "setSourceList: 'fragment' missing or not a string");
		return nullptr;
	}
	const char *vs = _PyUnicode_AsString(pv);
	const char *fs = _PyUnicode_AsString(pf);
	if (vs && vs[0]) m_shader->SetVertexSource(std::string(vs));
	if (fs && fs[0]) m_shader->SetFragmentSource(std::string(fs));
	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_MDEI_ShaderProxy, isValid,
"isValid() -> bool\n"
"True when the underlying MDEI_Shader is compiled and ready.\n")
{
	return PyBool_FromLong(m_shader && m_shader->IsReady() ? 1 : 0);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Scalar / vector float uniforms
 * ══════════════════════════════════════════════════════════════════════════ */

EXP_PYMETHODDEF_DOC(KX_MDEI_ShaderProxy, setUniform1f,
"setUniform1f(name, v)\n")
{
	MDEI_PROXY_GUARD("setUniform1f");
	const char *n; float v;
	if (!PyArg_ParseTuple(args, "sf:setUniform1f", &n, &v)) return nullptr;
	m_shader->SetUniform1f(n, v);
	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_MDEI_ShaderProxy, setUniform2f,
"setUniform2f(name, x, y)\n")
{
	MDEI_PROXY_GUARD("setUniform2f");
	const char *n; float x, y;
	if (!PyArg_ParseTuple(args, "sff:setUniform2f", &n, &x, &y)) return nullptr;
	m_shader->SetUniform2f(n, x, y);
	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_MDEI_ShaderProxy, setUniform3f,
"setUniform3f(name, x, y, z)\n")
{
	MDEI_PROXY_GUARD("setUniform3f");
	const char *n; float x, y, z;
	if (!PyArg_ParseTuple(args, "sfff:setUniform3f", &n, &x, &y, &z)) return nullptr;
	m_shader->SetUniform3f(n, x, y, z);
	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_MDEI_ShaderProxy, setUniform4f,
"setUniform4f(name, x, y, z, w)\n")
{
	MDEI_PROXY_GUARD("setUniform4f");
	const char *n; float x, y, z, w;
	if (!PyArg_ParseTuple(args, "sffff:setUniform4f", &n, &x, &y, &z, &w)) return nullptr;
	m_shader->SetUniform4f(n, x, y, z, w);
	Py_RETURN_NONE;
}

/* ── Scalar / vector int uniforms ────────────────────────────────────── */

EXP_PYMETHODDEF_DOC(KX_MDEI_ShaderProxy, setUniform1i,
"setUniform1i(name, v)\n")
{
	MDEI_PROXY_GUARD("setUniform1i");
	const char *n; int v;
	if (!PyArg_ParseTuple(args, "si:setUniform1i", &n, &v)) return nullptr;
	m_shader->SetUniform1i(n, v);
	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_MDEI_ShaderProxy, setUniform2i,
"setUniform2i(name, x, y)\n")
{
	MDEI_PROXY_GUARD("setUniform2i");
	const char *n; int x, y;
	if (!PyArg_ParseTuple(args, "sii:setUniform2i", &n, &x, &y)) return nullptr;
	m_shader->SetUniform2i(n, x, y);
	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_MDEI_ShaderProxy, setUniform3i,
"setUniform3i(name, x, y, z)\n")
{
	MDEI_PROXY_GUARD("setUniform3i");
	const char *n; int x, y, z;
	if (!PyArg_ParseTuple(args, "siii:setUniform3i", &n, &x, &y, &z)) return nullptr;
	m_shader->SetUniform3i(n, x, y, z);
	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_MDEI_ShaderProxy, setUniform4i,
"setUniform4i(name, x, y, z, w)\n")
{
	MDEI_PROXY_GUARD("setUniform4i");
	const char *n; int x, y, z, w;
	if (!PyArg_ParseTuple(args, "siiii:setUniform4i", &n, &x, &y, &z, &w)) return nullptr;
	m_shader->SetUniform4i(n, x, y, z, w);
	Py_RETURN_NONE;
}

/* ══════════════════════════════════════════════════════════════════════════
 * List-based float / int uniforms
 * ══════════════════════════════════════════════════════════════════════════ */

EXP_PYMETHODDEF_DOC(KX_MDEI_ShaderProxy, setUniformfv,
"setUniformfv(name, list)\n"
"list must have 2, 3 or 4 float elements.\n")
{
	MDEI_PROXY_GUARD("setUniformfv");
	const char *n; PyObject *lst;
	if (!PyArg_ParseTuple(args, "sO:setUniformfv", &n, &lst)) return nullptr;
	if (!PySequence_Check(lst)) {
		PyErr_SetString(PyExc_TypeError, "setUniformfv: second argument must be a sequence");
		return nullptr;
	}
	int sz = (int)PySequence_Size(lst);
	if (sz < 2 || sz > 4) {
		PyErr_SetString(PyExc_ValueError, "setUniformfv: list must have 2, 3 or 4 elements");
		return nullptr;
	}
	float d[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	for (int i = 0; i < sz; i++) {
		PyObject *item = PySequence_GetItem(lst, i);
		d[i] = (float)PyFloat_AsDouble(item);
		Py_DECREF(item);
	}
	m_shader->SetUniformfv(n, d, sz);
	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_MDEI_ShaderProxy, setUniformiv,
"setUniformiv(name, list)\n"
"list must have 2, 3 or 4 int elements.\n")
{
	MDEI_PROXY_GUARD("setUniformiv");
	const char *n; PyObject *lst;
	if (!PyArg_ParseTuple(args, "sO:setUniformiv", &n, &lst)) return nullptr;
	if (!PySequence_Check(lst)) {
		PyErr_SetString(PyExc_TypeError, "setUniformiv: second argument must be a sequence");
		return nullptr;
	}
	int sz = (int)PySequence_Size(lst);
	if (sz < 2 || sz > 4) {
		PyErr_SetString(PyExc_ValueError, "setUniformiv: list must have 2, 3 or 4 elements");
		return nullptr;
	}
	int d[4] = {0, 0, 0, 0};
	for (int i = 0; i < sz; i++) {
		PyObject *item = PySequence_GetItem(lst, i);
		d[i] = (int)PyLong_AsLong(item);
		Py_DECREF(item);
	}
	m_shader->SetUniformiv(n, d, sz);
	Py_RETURN_NONE;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Matrix uniforms
 * ══════════════════════════════════════════════════════════════════════════ */

EXP_PYMETHODDEF_DOC(KX_MDEI_ShaderProxy, setUniformMatrix3,
"setUniformMatrix3(name, list9or3x3, transpose=False)\n"
"Pass a flat list of 9 floats or a 3x3 mathutils.Matrix.\n")
{
	MDEI_PROXY_GUARD("setUniformMatrix3");
	const char *n; PyObject *mat; int transp = 0;
	if (!PyArg_ParseTuple(args, "sO|i:setUniformMatrix3", &n, &mat, &transp)) return nullptr;

	/* Accept flat list of 9 or a Matrix object */
	float m9[9] = {};
	if (PySequence_Check(mat)) {
		int sz = (int)PySequence_Size(mat);
		if (sz == 9) {
			for (int i = 0; i < 9; i++) {
				PyObject *it = PySequence_GetItem(mat, i);
				m9[i] = (float)PyFloat_AsDouble(it);
				Py_DECREF(it);
			}
		}
		else {
			/* Try as 3x3 (nested) */
			mt::mat3 mm;
			if (!PyMatTo(mat, mm)) {
				PyErr_SetString(PyExc_TypeError,
				    "setUniformMatrix3: expected a flat list of 9 floats or a 3x3 matrix");
				return nullptr;
			}
			mm.Pack(m9);
		}
	}
	else {
		mt::mat3 mm;
		if (!PyMatTo(mat, mm)) {
			PyErr_SetString(PyExc_TypeError,
			    "setUniformMatrix3: expected a flat list of 9 floats or a 3x3 matrix");
			return nullptr;
		}
		mm.Pack(m9);
	}
	m_shader->SetUniformMatrix3(n, m9, transp != 0);
	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_MDEI_ShaderProxy, setUniformMatrix4,
"setUniformMatrix4(name, list16or4x4, transpose=False)\n"
"Pass a flat list of 16 floats or a 4x4 mathutils.Matrix.\n")
{
	MDEI_PROXY_GUARD("setUniformMatrix4");
	const char *n; PyObject *mat; int transp = 0;
	if (!PyArg_ParseTuple(args, "sO|i:setUniformMatrix4", &n, &mat, &transp)) return nullptr;

	float m16[16] = {};
	if (PySequence_Check(mat) && PySequence_Size(mat) == 16) {
		for (int i = 0; i < 16; i++) {
			PyObject *it = PySequence_GetItem(mat, i);
			m16[i] = (float)PyFloat_AsDouble(it);
			Py_DECREF(it);
		}
	}
	else {
		mt::mat4 mm;
		if (!PyMatTo(mat, mm)) {
			PyErr_SetString(PyExc_TypeError,
			    "setUniformMatrix4: expected a flat list of 16 floats or a 4x4 matrix");
			return nullptr;
		}
		memcpy(m16, mm.Data(), sizeof(m16));
	}
	m_shader->SetUniformMatrix4(n, m16, transp != 0);
	Py_RETURN_NONE;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Classic samplers
 * ══════════════════════════════════════════════════════════════════════════ */

EXP_PYMETHODDEF_DOC(KX_MDEI_ShaderProxy, setSampler,
"setSampler(name, glTexId, unit)\n"
"Bind a 2D texture to a named sampler uniform using a classic texture unit.\n"
"glTexId: raw OpenGL texture id (int from e.g. bgl.glGenTextures or GPU_texture_opengl_bindcode).\n"
"unit:    GL texture unit (8-31 recommended to avoid GPUMaterial slots 0-7).\n")
{
	MDEI_PROXY_GUARD("setSampler");
	const char *n; unsigned int glTex; int unit;
	if (!PyArg_ParseTuple(args, "sIi:setSampler", &n, &glTex, &unit)) return nullptr;
#if MDEI_PROXY_DEBUG
	fprintf(stderr, "[MDEI_ShaderProxy] setSampler '%s' glTex=%u unit=%d\n", n, glTex, unit);
#endif
	m_shader->SetSampler(n, (GLuint)glTex, unit);
	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_MDEI_ShaderProxy, setSamplerArray,
"setSamplerArray(name, [glTexId, ...], mipmap=False)\n"
"Assemble a GL_TEXTURE_2D_ARRAY from plain 2D GL texture ids.\n"
"All source textures must have the same resolution.\n"
"The texture unit is allocated automatically (first free slot >= 8).\n"
"mipmap: generate mipmaps respecting the global GPU mipmap setting.\n")
{
	MDEI_PROXY_GUARD("setSamplerArray");
	const char *n;
	PyObject *lst;
	PyObject *mipmapObj = Py_False;

	if (!PyArg_ParseTuple(args, "sO|O:setSamplerArray", &n, &lst, &mipmapObj))
		return nullptr;
	if (!PySequence_Check(lst)) {
		PyErr_SetString(PyExc_TypeError, "setSamplerArray: second argument must be a sequence");
		return nullptr;
	}
	bool useMipmap = (PyObject_IsTrue(mipmapObj) == 1);
	int count = (int)PySequence_Size(lst);
	if (count <= 0) {
		PyErr_SetString(PyExc_ValueError, "setSamplerArray: texture list is empty");
		return nullptr;
	}
	std::vector<GLuint> ids;
	ids.reserve((size_t)count);
	for (int i = 0; i < count; i++) {
		PyObject *item = PySequence_GetItem(lst, i);
		ids.push_back((GLuint)PyLong_AsUnsignedLong(item));
		Py_DECREF(item);
	}
	if (PyErr_Occurred()) {
		PyErr_SetString(PyExc_TypeError, "setSamplerArray: list items must be ints (GL texture ids)");
		return nullptr;
	}
#if MDEI_PROXY_DEBUG
	fprintf(stderr, "[MDEI_ShaderProxy] setSamplerArray '%s' count=%d mipmap=%d\n",
	        n, count, (int)useMipmap);
#endif
	m_shader->SetSamplerArray(n, ids.data(), count, useMipmap);
	Py_RETURN_NONE;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Bindless samplers (GL_ARB_bindless_texture)
 * ══════════════════════════════════════════════════════════════════════════ */

EXP_PYMETHODDEF_DOC(KX_MDEI_ShaderProxy, setSamplerBindless,
"setSamplerBindless(name, handle)\n"
"Upload a 64-bit bindless texture handle to the named sampler uniform.\n"
"Mirrors BL_Shader.setSampler / KX_BlenderMaterial.ApplyTextures() path.\n"
"The handle must already be resident (GPU_texture_make_bindless_resident).\n"
"handle: Python int holding the GLuint64 value.\n")
{
	MDEI_PROXY_GUARD("setSamplerBindless");
	const char *n;
	unsigned long long handle = 0;
	if (!PyArg_ParseTuple(args, "sK:setSamplerBindless", &n, &handle))
		return nullptr;
#if MDEI_PROXY_DEBUG
	fprintf(stderr, "[MDEI_ShaderProxy] setSamplerBindless '%s' handle=0x%llx\n",
	        n, (unsigned long long)handle);
#endif
	m_shader->SetSamplerBindless(n, (GLuint64)handle);
	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_MDEI_ShaderProxy, setSamplerArrayFromGPUTextures,
"setSamplerArrayFromGPUTextures(name, [gpuTexPtr, ...], mipmap=False)\n"
"Build a GL_TEXTURE_2D_ARRAY from GPUTexture* pointers.\n"
"Each element of the list is a Python int holding the GPUTexture* address\n"
"(e.g. obtained via ctypes.addressof or from engine C extensions).\n"
"The texture unit is allocated automatically.\n"
"mipmap: generate mipmaps respecting the global GPU mipmap setting.\n")
{
	MDEI_PROXY_GUARD("setSamplerArrayFromGPUTextures");
	const char *n;
	PyObject *lst;
	PyObject *mipmapObj = Py_False;

	if (!PyArg_ParseTuple(args, "sO|O:setSamplerArrayFromGPUTextures", &n, &lst, &mipmapObj))
		return nullptr;
	if (!PySequence_Check(lst)) {
		PyErr_SetString(PyExc_TypeError,
		    "setSamplerArrayFromGPUTextures: second argument must be a sequence");
		return nullptr;
	}
	bool useMipmap = (PyObject_IsTrue(mipmapObj) == 1);
	int count = (int)PySequence_Size(lst);
	if (count <= 0) {
		PyErr_SetString(PyExc_ValueError,
		    "setSamplerArrayFromGPUTextures: texture list is empty");
		return nullptr;
	}
	std::vector<GPUTexture *> textures;
	textures.reserve((size_t)count);
	for (int i = 0; i < count; i++) {
		PyObject *item = PySequence_GetItem(lst, i);
		/* Accept Python int as raw pointer value */
		uintptr_t ptr = (uintptr_t)PyLong_AsUnsignedLongLong(item);
		Py_DECREF(item);
		if (PyErr_Occurred()) {
			PyErr_SetString(PyExc_TypeError,
			    "setSamplerArrayFromGPUTextures: list items must be ints (GPUTexture* as int)");
			return nullptr;
		}
		textures.push_back(reinterpret_cast<GPUTexture *>(ptr));
	}
#if MDEI_PROXY_DEBUG
	fprintf(stderr, "[MDEI_ShaderProxy] setSamplerArrayFromGPUTextures '%s' count=%d mipmap=%d\n",
	        n, count, (int)useMipmap);
#endif
	m_shader->SetSamplerArrayFromGPUTextures(n, textures.data(), count, useMipmap);
	Py_RETURN_NONE;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Mipmap control
 * ══════════════════════════════════════════════════════════════════════════ */

EXP_PYMETHODDEF_DOC(KX_MDEI_ShaderProxy, setMipmapping,
"setMipmapping(enabled, glFilterType=-1)\n"
"Enable or disable mipmapping on ALL sampler array bindings.\n"
"Forces a full texture array rebuild on next BindSolid().\n"
"enabled:      True/False.\n"
"glFilterType: GL filter constant (e.g. bgl.GL_LINEAR_MIPMAP_LINEAR).\n"
"              Pass -1 (default) to auto-select based on the global\n"
"              GPU mipmap setting (bge.render.setMipmapping).\n")
{
	MDEI_PROXY_GUARD("setMipmapping");
	PyObject *enabledObj;
	int glFilterType = -1;
	if (!PyArg_ParseTuple(args, "O|i:setMipmapping", &enabledObj, &glFilterType))
		return nullptr;
	int enabled = PyObject_IsTrue(enabledObj);
	if (enabled < 0) {
		PyErr_SetString(PyExc_TypeError, "setMipmapping: first argument must be bool");
		return nullptr;
	}
#if MDEI_PROXY_DEBUG
	fprintf(stderr, "[MDEI_ShaderProxy] setMipmapping enabled=%d filter=%d\n",
	        enabled, glFilterType);
#endif
	m_shader->SetMipmapping(enabled != 0, glFilterType);
	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_MDEI_ShaderProxy, updateMipmappingFilter,
"updateMipmappingFilter(glFilterType, slot=-1)\n"
"Re-apply a mipmap filter to already-assembled sampler array textures.\n"
"Does NOT rebuild the pixel data — only calls glTexParameteri.\n"
"glFilterType: GL constant (e.g. bgl.GL_LINEAR_MIPMAP_NEAREST).\n"
"slot:         index of the specific sampler array, or -1 for all.\n")
{
	MDEI_PROXY_GUARD("updateMipmappingFilter");
	int glFilterType;
	int slot = -1;
	if (!PyArg_ParseTuple(args, "i|i:updateMipmappingFilter", &glFilterType, &slot))
		return nullptr;
#if MDEI_PROXY_DEBUG
	fprintf(stderr, "[MDEI_ShaderProxy] updateMipmappingFilter filter=%d slot=%d\n",
	        glFilterType, slot);
#endif
	m_shader->UpdateMipmappingFilter(glFilterType, slot);
	Py_RETURN_NONE;
}

#endif /* WITH_PYTHON */
