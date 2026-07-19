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
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Ketsji/BL_Shader.cpp
 *  \ingroup ketsji
 */

#define CM_Log(msg) fprintf(stdout, "[CM_LOG] %s\n", msg)
#define CM_Logf(fmt, ...) fprintf(stdout, "[CM_LOG] " fmt "\n", __VA_ARGS__)

#include "BL_Shader.h"

#include <vector>
#include <string>

#include "../../../source/blender/gpu/GPU_shader.h"


#include "RAS_MeshSlot.h"
#include "RAS_MeshUser.h"
#include "RAS_IMaterial.h" 
#include "RAS_MaterialBucket.h"
#include "RAS_DisplayArrayBucket.h"
#include "RAS_Shader.h"
#include "../../../source/blender/gpu/GPU_texture.h"
#include "../../../source/blender/gpu/GPU_draw.h"
#include "../../../source/blender/gpu/GPU_glew.h"

#include "KX_PyMath.h"
#include "KX_PythonInit.h"
#include "KX_GameObject.h"
#include "KX_Globals.h"
#include "KX_Scene.h"
#include "KX_LightObject.h"

#include "DNA_material_types.h"
#include "DNA_lamp_types.h"
#include "RAS_ILightObject.h"

#ifdef WITH_PYTHON
#  include "EXP_PythonCallBack.h"
#endif  // WITH_PYTHON

#include <boost/format.hpp>

#include "CM_Message.h"

std::vector<std::pair<std::string, int>> g_ShaderSamplerBindings;
std::vector<std::pair<std::string, float>> g_ShaderUniform1fBindings;

BL_Shader::BL_Shader(CM_UpdateServer<RAS_IMaterial> *materialUpdateServer)
	:m_attr(SHD_NONE),
	m_materialUpdateServer(materialUpdateServer),
	m_lastSamplerUpdateFrame(0xFFFFFFFF),
	m_lastSamplerMaterial(nullptr)
{
	m_shadowLights.fill(nullptr);
	m_shadowTextureUnits.fill(-1);
	m_samplerSlotLocs.fill(-1);
#ifdef WITH_PYTHON
	for (unsigned short i = 0; i < CALLBACKS_MAX; ++i) {
		m_callbacks[i] = PyList_New(0);
	}
#endif  // WITH_PYTHON
}

BL_Shader::~BL_Shader()
{
#ifdef WITH_PYTHON
	for (unsigned short i = 0; i < CALLBACKS_MAX; ++i) {
		Py_XDECREF(m_callbacks[i]);
	}
#endif  // WITH_PYTHON
}

int BL_Shader::GetSamplerSlotLoc(unsigned short slot) const
{
	if (slot >= RAS_Texture::MaxUnits) return -1;
	return m_samplerSlotLocs[slot];
}

bool BL_Shader::LinkProgram()
{
	// Can be null in case of filter shaders.
	if (m_materialUpdateServer) {
		// Notify all clients tracking this shader that shader is recompiled and attributes are invalidated.
		m_materialUpdateServer->NotifyUpdate(RAS_IMaterial::SHADER_MODIFIED | RAS_IMaterial::ATTRIBUTES_MODIFIED);
	}

	return RAS_Shader::LinkProgram();
}

std::string BL_Shader::GetName()
{
	return "BL_Shader";
}

std::string BL_Shader::GetText()
{
    return (boost::format(
        "BL_Shader\n"
        "\tvertex shader: %s\n"
        "\tfragment shader: %s\n"
        "\tgeometry shader: %s\n"
        "\tcompute shader: %s\n"
        "\ttess control shader: %s\n"
        "\ttess evaluation shader: %s\n")
        % m_progs[VERTEX_PROGRAM]
        % m_progs[FRAGMENT_PROGRAM]
        % m_progs[GEOMETRY_PROGRAM]
        % m_progs[COMPUTE_PROGRAM]
        % m_progs[TESS_CONTROL_PROGRAM]
        % m_progs[TESS_EVALUATION_PROGRAM]
    ).str();
}

#ifdef WITH_PYTHON

PyObject *BL_Shader::GetCallbacks(BL_Shader::CallbacksType type)
{
	return m_callbacks[type];
}

void BL_Shader::SetCallbacks(BL_Shader::CallbacksType type, PyObject *callbacks)
{
	Py_XDECREF(m_callbacks[type]);
	Py_INCREF(callbacks);
	m_callbacks[type] = callbacks;
}

#endif  // WITH_PYTHON

RAS_AttributeArray::AttribList BL_Shader::GetAttribs(const RAS_Mesh::LayersInfo& layersInfo,
                                                     RAS_Texture *const textures[RAS_Texture::MaxUnits]) const
{
	RAS_AttributeArray::AttribList attribs;
	// Initialize textures attributes.
	for (unsigned short i = 0; i < RAS_Texture::MaxUnits; ++i) {
		RAS_Texture *texture = textures[i];
		/* Here textures can return false to Ok() because we're looking only at
		 * texture attributes and not texture bind id like for the binding and
		 * unbinding of textures. A nullptr RAS_Texture means that the corresponding
		 * mtex is nullptr too (see KX_BlenderMaterial::InitTextures).*/
		if (texture) {
			MTex *mtex = texture->GetMTex();
			if (mtex->texco & (TEXCO_OBJECT | TEXCO_REFL)) {
				attribs.push_back({i, RAS_AttributeArray::RAS_ATTRIB_POS, true, 0});
			}
			else if (mtex->texco & (TEXCO_ORCO | TEXCO_GLOB)) {
				attribs.push_back({i, RAS_AttributeArray::RAS_ATTRIB_POS, true, 0});
			}
			else if (mtex->texco & TEXCO_UV) {
				// UV layer not specified, use default layer.
				if (strlen(mtex->uvname) == 0) {
					attribs.push_back({i, RAS_AttributeArray::RAS_ATTRIB_UV, true, layersInfo.activeUv});
				}

				// Search for the UV layer index used by the texture.
				for (const RAS_Mesh::Layer& layer : layersInfo.uvLayers) {
					if (layer.name == mtex->uvname) {
						attribs.push_back({i, RAS_AttributeArray::RAS_ATTRIB_UV, true, layer.index});
						break;
					}
				}
			}
			else if (mtex->texco & TEXCO_NORM) {
				attribs.push_back({i, RAS_AttributeArray::RAS_ATTRIB_NORM, true, 0});
			}
			else if (mtex->texco & TEXCO_TANGENT) {
				attribs.push_back({i, RAS_AttributeArray::RAS_ATTRIB_TANGENT, true, 0});
			}
		}
	}

	if (m_attr == SHD_TANGENT) {
		attribs.push_back({1, RAS_AttributeArray::RAS_ATTRIB_TANGENT, false, 0});
	}

	return attribs;
}

void BL_Shader::BindProg()
{
#ifdef WITH_PYTHON
	if (PyList_GET_SIZE(m_callbacks[CALLBACKS_BIND]) > 0) {
		EXP_RunPythonCallBackList(m_callbacks[CALLBACKS_BIND], nullptr, 0, 0);
	}
#endif  // WITH_PYTHON

	RAS_Shader::BindProg();
}

int BL_Shader::GetFreeTextureUnit()
{
	// Procura uma unidade livre a partir do MaxUnits (geralmente 8)
	// Evita conflitos com material padrão (0-7) e unidades reservadas da engine (15-18)
	for (int unit = RAS_Texture::MaxUnits; unit < 32; unit++) {
		// Pular unidades reservadas da engine (DepthTexture=15, CustomTexture=16, 17, 18)
		if (unit >= 15 && unit <= 18) continue;

		bool used = false;
		// Verificar se já foi usado por setSampler global
		for (const auto& binding : g_ShaderSamplerBindings) {
			if (binding.second == unit) {
				used = true;
				break;
			}
		}
		if (used) continue;

		// Verificar se já foi usado por outros sampler arrays neste shader
		for (const auto& binding : m_samplerArrays) {
			if (binding.unit == unit) {
				used = true;
				break;
			}
		}
		if (used) continue;

		for (const int shadowUnit : m_shadowTextureUnits) {
			if (shadowUnit == unit) {
				used = true;
				break;
			}
		}
		if (used) continue;

		return unit;
	}
	return -1;
}

void BL_Shader::Update(RAS_Rasterizer *rasty, RAS_MeshUser *meshUser)
{
#ifdef WITH_PYTHON
	if (PyList_GET_SIZE(m_callbacks[CALLBACKS_OBJECT]) > 0) {
		KX_GameObject *gameobj = KX_GameObject::GetClientObject((KX_ClientObjectInfo *)meshUser->GetClientObject());
		PyObject *args[] = {gameobj->GetProxy()};
		EXP_RunPythonCallBackList(m_callbacks[CALLBACKS_OBJECT], args, 0, ARRAY_SIZE(args));
	}
#endif  // WITH_PYTHON

	KX_GameObject *gameobj = KX_GameObject::GetClientObject((KX_ClientObjectInfo *)meshUser->GetClientObject());
	if (gameobj && Ok()) {
		KX_Scene *scene = KX_GetActiveScene();
		if (scene) {
			std::array<RAS_ILightObject *, 8> newLights;
			std::array<int, 8> newUnits;
			newLights.fill(nullptr);
			newUnits.fill(-1);

			int shadowCount = 0;
			EXP_ListValue<KX_LightObject> *lightlist = scene->GetLightList();
			const int objectLayer = gameobj->GetLayer();

			for (KX_LightObject *light : lightlist) {
				if (shadowCount >= (int)newLights.size()) {
					break;
				}
				if (!light->GetVisible()) {
					continue;
				}
				if ((light->GetLayer() & objectLayer) == 0) {
					continue;
				}

				RAS_ILightObject *raslight = light->GetLightData();
				if (!raslight || !raslight->HasShadowBuffer()) {
					continue;
				}

				const int bindCode = raslight->GetShadowBindCode();
				if (bindCode < 0) {
					continue;
				}

				int unit = -1;
				for (size_t i = 0; i < m_shadowLights.size(); ++i) {
					if (m_shadowLights[i] == raslight) {
						unit = m_shadowTextureUnits[i];
						break;
					}
				}
				if (unit < 0) {
					unit = GetFreeTextureUnit();
				}
				if (unit < 0) {
					continue;
				}

				newLights[shadowCount] = raslight;
				newUnits[shadowCount] = unit;

				glActiveTexture(GL_TEXTURE0 + unit);
				glBindTexture(GL_TEXTURE_2D, (GLuint)bindCode);

				const bool isVsm = (raslight->m_shadowmaptype == LA_SHADMAP_VARIANCE);

				{
					const std::string name = std::string("bgl_ShadowMatrix[") + std::to_string(shadowCount) + "]";
					const int loc = GetUniformLocation(name, false);
					if (loc != -1) {
						SetUniform(loc, raslight->GetShadowMatrix());
					}
				}

				{
					const std::string name = std::string("bgl_ShadowIsVSM[") + std::to_string(shadowCount) + "]";
					const int loc = GetUniformLocation(name, false);
					if (loc != -1) {
						const int v = isVsm ? 1 : 0;
						SetUniformiv(loc, RAS_Uniform::UNI_INT, &v, (int)sizeof(int), 1);
					}
				}

				{
					const std::string name = std::string("bgl_ShadowBias[") + std::to_string(shadowCount) + "]";
					const int loc = GetUniformLocation(name, false);
					if (loc != -1) {
						const float v = raslight->m_shadowbias;
						SetUniformfv(loc, RAS_Uniform::UNI_FLOAT, &v, (int)sizeof(float), 1);
					}
				}

				{
					const std::string name = std::string("bgl_ShadowBleedBias[") + std::to_string(shadowCount) + "]";
					const int loc = GetUniformLocation(name, false);
					if (loc != -1) {
						const float v = raslight->m_shadowbleedbias;
						SetUniformfv(loc, RAS_Uniform::UNI_FLOAT, &v, (int)sizeof(float), 1);
					}
				}

				{
					const std::string name = std::string("bgl_ShadowColor[") + std::to_string(shadowCount) + "]";
					const int loc = GetUniformLocation(name, false);
					if (loc != -1) {
						SetUniform(loc, raslight->m_shadowcolor);
					}
				}

				if (isVsm) {
					const std::string name = std::string("bgl_ShadowMapVSM[") + std::to_string(shadowCount) + "]";
					const int loc = GetUniformLocation(name, false);
					if (loc != -1) {
						SetUniformiv(loc, RAS_Uniform::UNI_INT, &unit, (int)sizeof(int), 1);
					}
				}
				else {
					const std::string name = std::string("bgl_ShadowMap[") + std::to_string(shadowCount) + "]";
					const int loc = GetUniformLocation(name, false);
					if (loc != -1) {
						SetUniformiv(loc, RAS_Uniform::UNI_INT, &unit, (int)sizeof(int), 1);
					}
				}

				++shadowCount;
			}

			{
				const int loc = GetUniformLocation("bgl_ShadowCount", false);
				if (loc != -1) {
					SetUniformiv(loc, RAS_Uniform::UNI_INT, &shadowCount, (int)sizeof(int), 1);
				}
			}

			m_shadowLights = newLights;
			m_shadowTextureUnits = newUnits;
		}
	}

	if (!m_samplerArrays.empty()) {
		auto& slots = meshUser->GetMeshSlots();
		if (!slots.empty()) {
			RAS_IMaterial *mat = slots[0].m_displayArrayBucket->GetBucket()->GetMaterial();
			if (mat) {
				const unsigned int currentFrame = rasty->GetFrameCount();
				const bool sameFrameAndMaterial = (m_lastSamplerUpdateFrame == currentFrame && m_lastSamplerMaterial == mat);

				if (!sameFrameAndMaterial) {
					for (auto& binding : m_samplerArrays) {
						bool changed = false;
						std::vector<GPUTexture *> current_sources;

						for (int idx : binding.indices) {
							if (idx >= 0 && idx < 8) { // RAS_Texture::MaxUnits
								RAS_Texture *ras_tex = mat->GetTexture(idx);
								if (ras_tex) {
									GPUTexture *gpu_tex = ras_tex->GetGPUTexture();
									if (gpu_tex) {
										int tw = GPU_texture_width(gpu_tex);
										int th = GPU_texture_height(gpu_tex);
										if (tw > 4 && th > 4) {
											current_sources.push_back(gpu_tex);
										}
									}
								}
							}
						}

						// Se nem todas as texturas estão prontas, tenta no próximo frame.
						if (current_sources.size() != binding.indices.size()) {
							binding.dirty = true;
							continue;
						}

						// Detect if sources changed
						if (current_sources.size() != binding.sources.size()) {
							changed = true;
						}
						else {
							for (size_t i = 0; i < current_sources.size(); i++) {
								if (current_sources[i] != binding.sources[i]) {
									changed = true;
									break;
								}
							}
						}

						if ((changed || binding.dirty) && !current_sources.empty()) {
							binding.dirty = false;
							binding.sources = current_sources;

							if (binding.texture) {
								GPU_texture_free(binding.texture);
								binding.texture = nullptr;
							}

							int w = GPU_texture_width(current_sources[0]);
							int h = GPU_texture_height(current_sources[0]);

							// Verify all textures have the same size
							bool size_mismatch = false;
							for (size_t i = 1; i < current_sources.size(); i++) {
								if (GPU_texture_width(current_sources[i]) != w || GPU_texture_height(current_sources[i]) != h) {
									size_mismatch = true;
									break;
								}
							}

							if (size_mismatch) {
								CM_Warning("setSamplerArray: texture size mismatch for uniform " << binding.name);
								continue;
							}

							int depth = (int)current_sources.size();

							binding.texture = GPU_texture_create_2D_array(w, h, depth, 4, nullptr);
							if (binding.texture) {
								GLuint dst_id = (GLuint)GPU_texture_opengl_bindcode(binding.texture);

								glBindTexture(GL_TEXTURE_2D_ARRAY, dst_id);
								for (int i = 0; i < depth; i++) {
									GLuint src_id = (GLuint)GPU_texture_opengl_bindcode(current_sources[i]);

									glBindTexture(GL_TEXTURE_2D, src_id);
									void *pixels = malloc(w * h * 4);
									glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

									glBindTexture(GL_TEXTURE_2D_ARRAY, dst_id);
									glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, w, h, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
									free(pixels);
								}

								if (binding.useMipmap && GPU_get_mipmap()) {
									glGenerateMipmap(GL_TEXTURE_2D_ARRAY);

									if (GPU_get_linear_mipmap()) {
										glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
									}
									else {
										glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
									}
									glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
								}
								else {
									glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
									glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
								}

								if (GLEW_EXT_texture_filter_anisotropic) {
									glTexParameterf(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_ANISOTROPY_EXT, GPU_get_anisotropic());
								}

								glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
							}
						}
					}

					// Bind textures only when material or frame changes
					for (auto& binding : m_samplerArrays) {
						if (binding.texture) {
							GPU_texture_bind(binding.texture, (int)binding.unit);
						}
					}

					m_lastSamplerUpdateFrame = currentFrame;
					m_lastSamplerMaterial = mat;
				}
			}
		}
	}

	RAS_Shader::Update(rasty, meshUser->GetMatrix());
}

#ifdef WITH_PYTHON
PyMethodDef BL_Shader::Methods[] = {
	// creation
	EXP_PYMETHODTABLE(BL_Shader, setSource),
	EXP_PYMETHODTABLE(BL_Shader, setSourceList),
	EXP_PYMETHODTABLE(BL_Shader, delSource),
	EXP_PYMETHODTABLE(BL_Shader, getVertexProg),
	EXP_PYMETHODTABLE(BL_Shader, getFragmentProg),
	EXP_PYMETHODTABLE(BL_Shader, getComputeProg),
	EXP_PYMETHODTABLE(BL_Shader, validate),
	// access functions
	EXP_PYMETHODTABLE(BL_Shader, isValid),
	EXP_PYMETHODTABLE(BL_Shader, setUniformEyef),
	EXP_PYMETHODTABLE(BL_Shader, setUniform1f),
	EXP_PYMETHODTABLE(BL_Shader, setUniform2f),
	EXP_PYMETHODTABLE(BL_Shader, setUniform3f),
	EXP_PYMETHODTABLE(BL_Shader, setUniform4f),
	EXP_PYMETHODTABLE(BL_Shader, setUniform1i),
	EXP_PYMETHODTABLE(BL_Shader, setUniform2i),
	EXP_PYMETHODTABLE(BL_Shader, setUniform3i),
	EXP_PYMETHODTABLE(BL_Shader, setUniform4i),
	EXP_PYMETHODTABLE(BL_Shader, setAttrib),
	EXP_PYMETHODTABLE(BL_Shader, setUniformfv),
	EXP_PYMETHODTABLE(BL_Shader, setUniformiv),
	EXP_PYMETHODTABLE(BL_Shader, setUniformDef),
	EXP_PYMETHODTABLE(BL_Shader, setSampler),
	EXP_PYMETHODTABLE(BL_Shader, setSamplerArray),
	EXP_PYMETHODTABLE(BL_Shader, setUniformMatrix4),
	EXP_PYMETHODTABLE(BL_Shader, setUniformMatrix3),
	{nullptr, nullptr} //Sentinel
};

PyAttributeDef BL_Shader::Attributes[] = {
	EXP_PYATTRIBUTE_RW_FUNCTION("enabled", BL_Shader, pyattr_get_enabled, pyattr_set_enabled),
	EXP_PYATTRIBUTE_RW_FUNCTION("bindCallbacks", BL_Shader, pyattr_get_callbacks, pyattr_set_callbacks),
	EXP_PYATTRIBUTE_RW_FUNCTION("objectCallbacks", BL_Shader, pyattr_get_callbacks, pyattr_set_callbacks),
	EXP_PYATTRIBUTE_NULL //Sentinel
};

PyTypeObject BL_Shader::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"BL_Shader",
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
	py_base_new
};

PyObject *BL_Shader::pyattr_get_enabled(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Shader *self = static_cast<BL_Shader *>(self_v);
	return PyBool_FromLong(self->GetEnabled());
}

int BL_Shader::pyattr_set_enabled(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Shader *self = static_cast<BL_Shader *>(self_v);
	int param = PyObject_IsTrue(value);
	if (param == -1) {
		PyErr_SetString(PyExc_AttributeError, "shader.enabled = bool: BL_Shader, expected True or False");
		return PY_SET_ATTR_FAIL;
	}

	self->SetEnabled(param);
	return PY_SET_ATTR_SUCCESS;
}

static std::map<const std::string, BL_Shader::CallbacksType> callbacksTable = {
	{"bindCallbacks", BL_Shader::CALLBACKS_BIND},
	{"objectCallbacks", BL_Shader::CALLBACKS_OBJECT}
};

PyObject *BL_Shader::pyattr_get_callbacks(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	BL_Shader *self = static_cast<BL_Shader *>(self_v);
	PyObject *callbacks = self->GetCallbacks(callbacksTable[attrdef->m_name]);
	Py_INCREF(callbacks);
	return callbacks;
}

int BL_Shader::pyattr_set_callbacks(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	BL_Shader *self = static_cast<BL_Shader *>(self_v);
	if (!PyList_CheckExact(value)) {
		PyErr_Format(PyExc_AttributeError, "shader.%s = bool: BL_Shader, expected a list", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	self->SetCallbacks(callbacksTable[attrdef->m_name], value);
	return PY_SET_ATTR_SUCCESS;
}

int g_apply2_value = 0;
int forceV = 0;

EXP_PYMETHODDEF_DOC(BL_Shader, setSource,
" setSource(vertexProgram, fragmentProgram, computeProgram, tessControlProgram, tessEvalProgram, apply, apply2, force)")
{
    char* v = nullptr;
    char* f = nullptr;
    char* c = nullptr;
    char* tc = nullptr;
    char* te = nullptr;
    int apply = 0;
    int apply2 = 0;
    int force = 0;

    CM_Log("[setSource] === Início da função ===");

    // Permitir 5 ou 8 argumentos (para compatibilidade)
    if (PyArg_ParseTuple(args, "sss|ssiii:setSource", &v, &f, &c, &tc, &te, &apply, &apply2, &force)) {
        
        m_progs[VERTEX_PROGRAM] = std::string(v ? v : "");
        m_progs[FRAGMENT_PROGRAM] = std::string(f ? f : "");
        m_progs[COMPUTE_PROGRAM] = std::string(c ? c : "");
        m_progs[TESS_CONTROL_PROGRAM] = std::string(tc ? tc : "");
        m_progs[TESS_EVALUATION_PROGRAM] = std::string(te ? te : "");
        m_progs[GEOMETRY_PROGRAM] = "";

        g_apply2_value = apply2;
        forceV = force;

        const bool has_vert = !m_progs[VERTEX_PROGRAM].empty();
        const bool has_frag = !m_progs[FRAGMENT_PROGRAM].empty();
        const bool has_comp = !m_progs[COMPUTE_PROGRAM].empty();

        if (force != 0 && has_comp) {
            RAS_Shader::autoRunCompute = true;
            CM_Log("[setSource] AutoRunCompute ATIVADO. Compute shader será executado automaticamente no Update().");
        }
        else {
            RAS_Shader::autoRunCompute = false;
        }

        if (LinkProgram()) {
            m_use = (apply != 0);
            CM_Log("[setSource] Shader compilado e ativado com sucesso.");
        }
        else {
            // Resetar shaders em caso de falha
            for (int i = 0; i < MAX_PROGRAM; ++i) {
                m_progs[i] = "";
            }
            m_use = false;
            CM_Error("[setSource] Falha ao compilar shader. Resetando shaders.");
        }
    }
    else {
        CM_Error("[setSource] Erro ao fazer parse dos argumentos Python.");
    }

    CM_Log("[setSource] === Fim da função ===");
    Py_RETURN_NONE;
}


EXP_PYMETHODDEF_DOC(BL_Shader, setSourceList, 
" setSourceList(shaderDict, apply, apply2)")
{
    if (m_shader) {
        Py_RETURN_NONE;
    }

    PyObject *pydict;
    int apply = 0;
    int apply2 = 0;

    if (!PyArg_ParseTuple(args, "O!ii:setSourceList", &PyDict_Type, &pydict, &apply, &apply2)) {
        return nullptr;
    }

    bool error = false;

    static const char *progname[MAX_PROGRAM] = {
        "vertex", "fragment", "geometry", "compute", "tesscontrol", "tesseval"
    };
    static const bool optional[MAX_PROGRAM] = {
        false, false, true, true, true, true
    };

    for (unsigned short i = 0; i < MAX_PROGRAM; ++i) {
        PyObject *pyprog = PyDict_GetItemString(pydict, progname[i]);
        if (!optional[i]) {
            if (!pyprog) {
                error = true;
                PyErr_Format(PyExc_SystemError, "setSourceList: missing required %s program", progname[i]);
                break;
            }
            else if (!PyUnicode_Check(pyprog)) {
                error = true;
                PyErr_Format(PyExc_SystemError, "setSourceList: %s program is not a string", progname[i]);
                break;
            }
        }
        if (pyprog) {
            m_progs[i] = std::string(_PyUnicode_AsString(pyprog));
        }
    }

    if (!error && LinkProgram()) {
        m_use = apply != 0;
    }

    Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(BL_Shader, getComputeProg, "getComputeProg()")
{
	return PyUnicode_FromString(m_progs[COMPUTE_PROGRAM].c_str());
}


EXP_PYMETHODDEF_DOC(BL_Shader, delSource, "delSource( )")
{
	ClearUniforms();
	DeleteShader();

	for (auto& binding : m_samplerArrays) {
		if (binding.texture) {
			GPU_texture_free(binding.texture);
			binding.texture = nullptr;
		}
	}
	m_samplerArrays.clear();

	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(BL_Shader, isValid, "isValid()")
{
	return PyBool_FromLong(m_shader != nullptr);
}

EXP_PYMETHODDEF_DOC(BL_Shader, getVertexProg, "getVertexProg( )")
{
	return PyUnicode_FromStdString(m_progs[VERTEX_PROGRAM]);
}

EXP_PYMETHODDEF_DOC(BL_Shader, getFragmentProg, "getFragmentProg( )")
{
	return PyUnicode_FromStdString(m_progs[FRAGMENT_PROGRAM]);
}

EXP_PYMETHODDEF_DOC(BL_Shader, validate, "validate()")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	if (!m_shader) {
		PyErr_SetString(PyExc_TypeError, "shader.validate(): BL_Shader, invalid shader object");
		return nullptr;
	}

	ValidateProgram();

	Py_RETURN_NONE;
}


EXP_PYMETHODDEF_DOC(BL_Shader, setSampler, "setSampler(name, index)")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	int index = -1;

	if (PyArg_ParseTuple(args, "si:setSampler", &uniform, &index)) {
		// Log global - verifica duplicatas
		bool found = false;
		for (auto& item : g_ShaderSamplerBindings) {
			if (item.first == uniform) {
				item.second = index;
				found = true;
				break;
			}
		}
		if (!found) {
			g_ShaderSamplerBindings.emplace_back(uniform, index);
		}

		int loc = GetUniformLocation(uniform);

		if (loc != -1) {
			if (index >= RAS_Texture::MaxUnits || index < 0) {
				CM_Warning("invalid texture sample index: " << index);
			}
			else {
				/* Bindless: store loc for use in ApplyTextures via GetSamplerSlotLoc.
				 * Do NOT call GPU_shader_uniform_int — that would send a unit index
				 * which is meaningless in bindless mode. */
				m_samplerSlotLocs[index] = loc;
			}
		}
		Py_RETURN_NONE;
	}
	return nullptr;
}


EXP_PYMETHODDEF_DOC(BL_Shader, setSamplerArray, "setSamplerArray(name, indices, mipmap=False)")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	PyObject *listPtr = nullptr;
	PyObject *mipmapObj = Py_False;

	if (PyArg_ParseTuple(args, "sO|O:setSamplerArray", &uniform, &listPtr, &mipmapObj)) {
		if (PySequence_Check(listPtr)) {
			bool useMipmap = PyObject_IsTrue(mipmapObj);

			// Verifica se já existe um binding para este uniform
			int unit = -1;
			for (auto& b : m_samplerArrays) {
				if (b.name == uniform) {
					unit = b.unit;
					// Limpa os índices antigos se estiver atualizando
					b.indices.clear();
					b.dirty = true;
					b.useMipmap = useMipmap;
					int size = PySequence_Size(listPtr);
					for (int i = 0; i < size; i++) {
						PyObject *item = PySequence_GetItem(listPtr, i);
						b.indices.push_back(PyLong_AsLong(item));
						Py_DECREF(item);
					}
					goto set_uniform;
				}
			}

			// Se não existir, aloca uma nova unidade
			unit = GetFreeTextureUnit();
			if (unit == -1) {
				CM_Error("setSamplerArray: No free texture units available!");
				Py_RETURN_NONE;
			}

			// Adiciona ao log global para seguir o mesmo fluxo de setSampler
			g_ShaderSamplerBindings.emplace_back(uniform, unit);

			{
				BL_ShaderSamplerArray binding;
				binding.name = uniform;
				binding.unit = unit;
				binding.useMipmap = useMipmap;

				int size = PySequence_Size(listPtr);
				for (int i = 0; i < size; i++) {
					PyObject *item = PySequence_GetItem(listPtr, i);
					binding.indices.push_back(PyLong_AsLong(item));
					Py_DECREF(item);
				}

				m_samplerArrays.push_back(binding);
			}

set_uniform:
			int loc = GetUniformLocation(uniform);
			if (loc != -1) {
#ifdef SORT_UNIFORMS
				SetUniformiv(loc, RAS_Uniform::UNI_INT, &unit, (sizeof(int)), 1);
#else
				this->SetUniform(loc, (int)unit);
#endif
			}
		}
		Py_RETURN_NONE;
	}
	return nullptr;
}


EXP_PYMETHODDEF_DOC(BL_Shader, setUniform1f, "setUniform1f(name, fx)")
{
    if (!m_shader) {
        Py_RETURN_NONE;
    }

    const char *uniform;
    float value = 0.0f;

    if (PyArg_ParseTuple(args, "sf:setUniform1f", &uniform, &value)) {
        bool found = false;

        // Percorre a lista para verificar duplicatas
        for (auto &item : g_ShaderUniform1fBindings) {
            if (strcmp(item.first.c_str(), uniform) == 0) {
                found = true;
                // Se valor for diferente, atualiza
                if (item.second != value) {
                    item.second = value;
                }
                // Se nome e valor são iguais, não faz nada
                break;
            }
        }

        // Se não encontrou, adiciona ao final (mantém ordem)
        if (!found) {
            g_ShaderUniform1fBindings.emplace_back(uniform, value);
        }

        // Aplica diretamente no shader ativo (opcional para imediatismo)
        int loc = GetUniformLocation(uniform);

        if (loc != -1) {
#ifdef SORT_UNIFORMS
            SetUniformfv(loc, RAS_Uniform::UNI_FLOAT, &value, sizeof(float), 1);
#else
            SetUniform(loc, value);
#endif
        }

        Py_RETURN_NONE;
    }
    return nullptr;
}



EXP_PYMETHODDEF_DOC(BL_Shader, setUniform2f, "setUniform2f(name, fx, fy)")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	float array[2] = {0.0f, 0.0f};

	if (PyArg_ParseTuple(args, "sff:setUniform2f", &uniform, &array[0], &array[1])) {
		int loc = GetUniformLocation(uniform);

		if (loc != -1) {
#ifdef SORT_UNIFORMS
			SetUniformfv(loc, RAS_Uniform::UNI_FLOAT2, array, (sizeof(float) * 2), 1);
#else
			SetUniform(loc, array, 2);
#endif
		}
		Py_RETURN_NONE;
	}
	return nullptr;
}

EXP_PYMETHODDEF_DOC(BL_Shader, setUniform3f, "setUniform3f(name, fx,fy,fz) ")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	float array[3] = {0.0f, 0.0f, 0.0f};

	if (PyArg_ParseTuple(args, "sfff:setUniform3f", &uniform, &array[0], &array[1], &array[2])) {
		int loc = GetUniformLocation(uniform);

		if (loc != -1) {
#ifdef SORT_UNIFORMS
			SetUniformfv(loc, RAS_Uniform::UNI_FLOAT3, array, (sizeof(float) * 3), 1);
#else
			SetUniform(loc, array, 3);
#endif
		}
		Py_RETURN_NONE;
	}
	return nullptr;
}

EXP_PYMETHODDEF_DOC(BL_Shader, setUniform4f, "setUniform4f(name, fx,fy,fz, fw) ")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	float array[4] = {0.0f, 0.0f, 0.0f, 0.0f};

	if (PyArg_ParseTuple(args, "sffff:setUniform4f", &uniform, &array[0], &array[1], &array[2], &array[3])) {
		int loc = GetUniformLocation(uniform);

		if (loc != -1) {
#ifdef SORT_UNIFORMS
			SetUniformfv(loc, RAS_Uniform::UNI_FLOAT4, array, (sizeof(float) * 4), 1);
#else
			SetUniform(loc, array, 4);
#endif
		}
		Py_RETURN_NONE;
	}
	return nullptr;
}

EXP_PYMETHODDEF_DOC(BL_Shader, setUniformEyef, "setUniformEyef(name)")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}
	const char *uniform;
	if (PyArg_ParseTuple(args, "s:setUniformEyef", &uniform)) {
		int loc = GetUniformLocation(uniform);
		if (loc != -1) {
			bool defined = false;
			for (RAS_DefUniform *defuni : m_preDef) {
				if (defuni->m_loc == loc) {
					defined = true;
					break;
				}
			}

			if (defined) {
				Py_RETURN_NONE;
			}

			RAS_DefUniform *uni = new RAS_DefUniform();
			uni->m_loc = loc;
			uni->m_type = EYE;
			uni->m_flag = 0;
			m_preDef.push_back(uni);
		}
		Py_RETURN_NONE;
	}
	return nullptr;
}

EXP_PYMETHODDEF_DOC(BL_Shader, setUniform1i, "setUniform1i(name, ix)")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	int value = 0;

	if (PyArg_ParseTuple(args, "si:setUniform1i", &uniform, &value)) {
		int loc = GetUniformLocation(uniform);

		if (loc != -1) {
#ifdef SORT_UNIFORMS
			SetUniformiv(loc, RAS_Uniform::UNI_INT, &value, sizeof(int), 1);
#else
			SetUniform(loc, (int)value);
#endif
		}
		Py_RETURN_NONE;
	}
	return nullptr;
}

EXP_PYMETHODDEF_DOC(BL_Shader, setUniform2i, "setUniform2i(name, ix, iy)")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	int array[2] = {0, 0};

	if (PyArg_ParseTuple(args, "sii:setUniform2i", &uniform, &array[0], &array[1])) {
		int loc = GetUniformLocation(uniform);

		if (loc != -1) {
#ifdef SORT_UNIFORMS
			SetUniformiv(loc, RAS_Uniform::UNI_INT2, array, sizeof(int) * 2, 1);
#else
			SetUniform(loc, array, 2);
#endif
		}
		Py_RETURN_NONE;
	}
	return nullptr;
}

EXP_PYMETHODDEF_DOC(BL_Shader, setUniform3i, "setUniform3i(name, ix,iy,iz) ")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	int array[3] = {0, 0, 0};

	if (PyArg_ParseTuple(args, "siii:setUniform3i", &uniform, &array[0], &array[1], &array[2])) {
		int loc = GetUniformLocation(uniform);

		if (loc != -1) {
#ifdef SORT_UNIFORMS
			SetUniformiv(loc, RAS_Uniform::UNI_INT3, array, sizeof(int) * 3, 1);
#else
			SetUniform(loc, array, 3);
#endif
		}
		Py_RETURN_NONE;
	}
	return nullptr;
}

EXP_PYMETHODDEF_DOC(BL_Shader, setUniform4i, "setUniform4i(name, ix,iy,iz, iw) ")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	int array[4] = {0, 0, 0, 0};

	if (PyArg_ParseTuple(args, "siiii:setUniform4i", &uniform, &array[0], &array[1], &array[2], &array[3])) {
		int loc = GetUniformLocation(uniform);

		if (loc != -1) {
#ifdef SORT_UNIFORMS
			SetUniformiv(loc, RAS_Uniform::UNI_INT4, array, sizeof(int) * 4, 1);
#else
			SetUniform(loc, array, 4);
#endif
		}
		Py_RETURN_NONE;
	}
	return nullptr;
}

EXP_PYMETHODDEF_DOC(BL_Shader, setUniformfv, "setUniformfv(float (list2 or list3 or list4))")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	const char *uniform = "";
	PyObject *listPtr = nullptr;
	float array_data[4] = {0.0f, 0.0f, 0.0f, 0.0f};

	if (PyArg_ParseTuple(args, "sO:setUniformfv", &uniform, &listPtr)) {
		int loc = GetUniformLocation(uniform);
		if (loc != -1) {
			if (PySequence_Check(listPtr)) {
				unsigned int list_size = PySequence_Size(listPtr);

				for (unsigned int i = 0; (i < list_size && i < 4); i++) {
					PyObject *item = PySequence_GetItem(listPtr, i);
					array_data[i] = (float)PyFloat_AsDouble(item);
					Py_DECREF(item);
				}

				switch (list_size) {
					case 2:
					{
						float array2[2] = {array_data[0], array_data[1]};
#ifdef SORT_UNIFORMS
						SetUniformfv(loc, RAS_Uniform::UNI_FLOAT2, array2, sizeof(float) * 2, 1);
#else
						SetUniform(loc, array2, 2);
#endif
						Py_RETURN_NONE;
						break;
					}
					case 3:
					{
						float array3[3] = {array_data[0], array_data[1], array_data[2]};
#ifdef SORT_UNIFORMS
						SetUniformfv(loc, RAS_Uniform::UNI_FLOAT3, array3, sizeof(float) * 3, 1);
#else
						SetUniform(loc, array3, 3);
#endif
						Py_RETURN_NONE;
						break;
					}
					case 4:
					{
						float array4[4] = {array_data[0], array_data[1], array_data[2], array_data[3]};
#ifdef SORT_UNIFORMS
						SetUniformfv(loc, RAS_Uniform::UNI_FLOAT4, array4, sizeof(float) * 4, 1);
#else
						SetUniform(loc, array4, 4);
#endif
						Py_RETURN_NONE;
						break;
					}
					default:
					{
						PyErr_SetString(PyExc_TypeError,
						                "shader.setUniform4i(name, ix,iy,iz, iw): BL_Shader. invalid list size");
						return nullptr;
						break;
					}
				}
			}
		}
	}
	return nullptr;
}

EXP_PYMETHODDEF_DOC(BL_Shader, setUniformiv, "setUniformiv(uniform_name, (list2 or list3 or list4))")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	const char *uniform = "";
	PyObject *listPtr = nullptr;
	int array_data[4] = {0, 0, 0, 0};

	if (!PyArg_ParseTuple(args, "sO:setUniformiv", &uniform, &listPtr)) {
		return nullptr;
	}

	int loc = GetUniformLocation(uniform);

	if (loc == -1) {
		PyErr_SetString(PyExc_TypeError,
		                "shader.setUniformiv(...): BL_Shader, first string argument is not a valid uniform value");
		return nullptr;
	}

	if (!PySequence_Check(listPtr)) {
		PyErr_SetString(PyExc_TypeError, "shader.setUniformiv(...): BL_Shader, second argument is not a sequence");
		return nullptr;
	}

	unsigned int list_size = PySequence_Size(listPtr);

	for (unsigned int i = 0; (i < list_size && i < 4); i++) {
		PyObject *item = PySequence_GetItem(listPtr, i);
		array_data[i] = PyLong_AsLong(item);
		Py_DECREF(item);
	}

	if (PyErr_Occurred()) {
		PyErr_SetString(PyExc_TypeError,
		                "shader.setUniformiv(...): BL_Shader, one or more values in the list is not an int");
		return nullptr;
	}

	// Sanity checks done!
	switch (list_size) {
		case 2:
		{
			int array2[2] = {array_data[0], array_data[1]};
#ifdef SORT_UNIFORMS
			SetUniformiv(loc, RAS_Uniform::UNI_INT2, array2, sizeof(int) * 2, 1);
#else
			SetUniform(loc, array2, 2);
#endif
			Py_RETURN_NONE;
			break;
		}
		case 3:
		{
			int array3[3] = {array_data[0], array_data[1], array_data[2]};
#ifdef SORT_UNIFORMS
			SetUniformiv(loc, RAS_Uniform::UNI_INT3, array3, sizeof(int) * 3, 1);
#else
			SetUniform(loc, array3, 3);
#endif
			Py_RETURN_NONE;
			break;
		}
		case 4:
		{
			int array4[4] = {array_data[0], array_data[1], array_data[2], array_data[3]};
#ifdef SORT_UNIFORMS
			SetUniformiv(loc, RAS_Uniform::UNI_INT4, array4, sizeof(int) * 4, 1);
#else
			SetUniform(loc, array4, 4);
#endif
			Py_RETURN_NONE;
			break;
		}
		default:
		{
			PyErr_SetString(PyExc_TypeError,
			                "shader.setUniformiv(...): BL_Shader, second argument, invalid list size, expected an int "
			                "list between 2 and 4");
			return nullptr;
			break;
		}
	}
	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(BL_Shader, setUniformMatrix4,
                    "setUniformMatrix4(uniform_name, mat-4x4, transpose(row-major=true, col-major=false)")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	PyObject *matrix = nullptr;
	int transp = 0; // python use column major by default, so no transpose....

	if (!PyArg_ParseTuple(args, "sO|i:setUniformMatrix4", &uniform, &matrix, &transp)) {
		return nullptr;
	}

	int loc = GetUniformLocation(uniform);

	if (loc == -1) {
		PyErr_SetString(PyExc_TypeError,
		                "shader.setUniformMatrix4(...): BL_Shader, first string argument is not a valid uniform value");
		return nullptr;
	}

	mt::mat4 mat;

	if (!PyMatTo(matrix, mat)) {
		PyErr_SetString(PyExc_TypeError,
		                "shader.setUniformMatrix4(...): BL_Shader, second argument cannot be converted into a 4x4 matrix");
		return nullptr;
	}

	// Sanity checks done!
#ifdef SORT_UNIFORMS
	SetUniformfv(loc, RAS_Uniform::UNI_MAT4, (float *)mat.Data(), (sizeof(float) * 16), 1, (transp != 0));
#else
	SetUniform(loc, mat, (transp != 0));
#endif
	Py_RETURN_NONE;
}


EXP_PYMETHODDEF_DOC(BL_Shader, setUniformMatrix3,
                    "setUniformMatrix3(uniform_name, list[3x3], transpose(row-major=true, col-major=false)")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	PyObject *matrix = nullptr;
	int transp = 0; // python use column major by default, so no transpose....

	if (!PyArg_ParseTuple(args, "sO|i:setUniformMatrix3", &uniform, &matrix, &transp)) {
		return nullptr;
	}

	int loc = GetUniformLocation(uniform);

	if (loc == -1) {
		PyErr_SetString(PyExc_TypeError,
		                "shader.setUniformMatrix3(...): BL_Shader, first string argument is not a valid uniform value");
		return nullptr;
	}

	mt::mat3 mat;

	if (!PyMatTo(matrix, mat)) {
		PyErr_SetString(PyExc_TypeError,
		                "shader.setUniformMatrix3(...): BL_Shader, second argument cannot be converted into a 3x3 matrix");
		return nullptr;
	}

#ifdef SORT_UNIFORMS
	float matr[9];
	mat.Pack(matr);
	SetUniformfv(loc, RAS_Uniform::UNI_MAT3, matr, (sizeof(float) * 9), 1, (transp != 0));
#else
	SetUniform(loc, mat, (transp != 0));
#endif
	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(BL_Shader, setAttrib, "setAttrib(enum)")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	int attr = 0;

	if (!PyArg_ParseTuple(args, "i:setAttrib", &attr)) {
		return nullptr;
	}

	attr = SHD_TANGENT; // user input is ignored for now, there is only 1 attr

	if (!m_shader) {
		PyErr_SetString(PyExc_ValueError, "shader.setAttrib() BL_Shader, invalid shader object");
		return nullptr;
	}

	// Avoid redundant attributes reconstruction.
	if (attr == m_attr) {
		Py_RETURN_NONE;
	}

	m_attr = (AttribTypes)attr;

	// Can be null in case of filter shaders.
	if (m_materialUpdateServer) {
		// Notify all clients tracking this shader that attributes are modified.
		m_materialUpdateServer->NotifyUpdate(RAS_IMaterial::ATTRIBUTES_MODIFIED);
	}

	BindAttribute("Tangent", m_attr);
	Py_RETURN_NONE;
}


EXP_PYMETHODDEF_DOC(BL_Shader, setUniformDef, "setUniformDef(name, enum)")
{
	if (!m_shader) {
		Py_RETURN_NONE;
	}

	const char *uniform;
	int nloc = 0;
	if (PyArg_ParseTuple(args, "si:setUniformDef", &uniform, &nloc)) {
		int loc = GetUniformLocation(uniform);

		if (loc != -1) {
			bool defined = false;
			for (RAS_DefUniform *defuni : m_preDef) {
				if (defuni->m_loc == loc) {
					defined = true;
					break;
				}
			}

			if (defined) {
				Py_RETURN_NONE;
			}

			RAS_DefUniform *uni = new RAS_DefUniform();
			uni->m_loc = loc;
			uni->m_type = nloc;
			uni->m_flag = 0;
			m_preDef.push_back(uni);
			Py_RETURN_NONE;
		}
	}
	return nullptr;
}

#endif // WITH_PYTHON
