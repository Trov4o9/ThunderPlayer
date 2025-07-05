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

/** \file gameengine/Ketsji/RAS_Shader.cpp
 *  \ingroup bgerast
 */

#include "RAS_Shader.h"
#include "RAS_Rasterizer.h"
#include "RAS_2DFilterOffScreen.h"
#include "../../../intern/ghost/intern/GHOST_ContextWGL.h"
#include "../../../intern/ghost/GHOST_Types.h"
#include "../../../source/blender/gpu/GPU_texture.h"
#include "../../../source/blender/gpu/GPU_framebuffer.h"
#include "../../Ketsji/BL_shader.h"

#include <vector>
#include <string>
#include <utility>

extern std::vector<std::pair<std::string, float>> g_ShaderUniform1fBindings;
extern std::vector<std::pair<std::string, int>> g_ShaderSamplerBindings;




#include "BLI_utildefines.h"
#include "MEM_guardedalloc.h"
#include <GL/glew.h>

#include "GPU_shader.h"

#include <cstring> // for memcpy

#include "CM_Message.h"
#include <fstream>
#include <stdio.h>
#include <thread>
#include <chrono>

GPUTexture *m_custom_texture = nullptr;
GPUTexture *m_custom_texture_in = nullptr;
GPUTexture *m_custom_texture_out = nullptr;

#define CM_Log(msg) fprintf(stdout, "[CM_LOG] %s\n", msg)
#define CM_Logf(fmt, ...) fprintf(stdout, "[CM_LOG] " fmt "\n", __VA_ARGS__)


RAS_Shader::RAS_Uniform::RAS_Uniform(int data_size)
	:m_loc(-1),
	m_count(1),
	m_dirty(true),
	m_type(UNI_NONE),
	m_dataLen(data_size)
{
#ifdef SORT_UNIFORMS
	m_data = (void *)MEM_mallocN(m_dataLen, "shader-uniform-alloc");
#endif
}

RAS_Shader::RAS_Uniform::~RAS_Uniform()
{
#ifdef SORT_UNIFORMS
	if (m_data) {
		MEM_freeN(m_data);
		m_data = nullptr;
	}
#endif
}

void RAS_Shader::RAS_Uniform::Apply(RAS_Shader *shader)
{
#ifdef SORT_UNIFORMS
	BLI_assert(m_type > UNI_NONE && m_type < UNI_MAX && m_data);

	if (!m_dirty) {
		return;
	}

	GPUShader *gpushader = shader->GetGPUShader();
	switch (m_type) {
		case UNI_FLOAT:
		{
			float *f = (float *)m_data;
			GPU_shader_uniform_vector(gpushader, m_loc, 1, m_count, (float *)f);
			break;
		}
		case UNI_INT:
		{
			int *f = (int *)m_data;
			GPU_shader_uniform_vector_int(gpushader, m_loc, 1, m_count, (int *)f);
			break;
		}
		case UNI_FLOAT2:
		{
			float *f = (float *)m_data;
			GPU_shader_uniform_vector(gpushader, m_loc, 2, m_count, (float *)f);
			break;
		}
		case UNI_FLOAT3:
		{
			float *f = (float *)m_data;
			GPU_shader_uniform_vector(gpushader, m_loc, 3, m_count, (float *)f);
			break;
		}
		case UNI_FLOAT4:
		{
			float *f = (float *)m_data;
			GPU_shader_uniform_vector(gpushader, m_loc, 4, m_count, (float *)f);
			break;
		}
		case UNI_INT2:
		{
			int *f = (int *)m_data;
			GPU_shader_uniform_vector_int(gpushader, m_loc, 2, m_count, (int *)f);
			break;
		}
		case UNI_INT3:
		{
			int *f = (int *)m_data;
			GPU_shader_uniform_vector_int(gpushader, m_loc, 3, m_count, (int *)f);
			break;
		}
		case UNI_INT4:
		{
			int *f = (int *)m_data;
			GPU_shader_uniform_vector_int(gpushader, m_loc, 4, m_count, (int *)f);
			break;
		}
		case UNI_MAT4:
		{
			float *f = (float *)m_data;
			GPU_shader_uniform_vector(gpushader, m_loc, 16, m_count, (float *)f);
			break;
		}
		case UNI_MAT3:
		{
			float *f = (float *)m_data;
			GPU_shader_uniform_vector(gpushader, m_loc, 9, m_count, (float *)f);
			break;
		}
	}
	m_dirty = false;
#endif
}

void RAS_Shader::RAS_Uniform::SetData(int location, int type, unsigned int count, bool transpose)
{
#ifdef SORT_UNIFORMS
	m_type = type;
	m_loc = location;
	m_count = count;
	m_dirty = true;
#endif
}

int RAS_Shader::RAS_Uniform::GetLocation()
{
	return m_loc;
}

void *RAS_Shader::RAS_Uniform::GetData()
{
	return m_data;
}

RAS_Shader::UniformInfo::UniformInfo(const std::string& name, GPUShader *shader)
	:nameHash(std::hash<std::string>()(name)),
	location(GPU_shader_get_uniform(shader, name.c_str()))
{
}

bool RAS_Shader::Ok() const
{
	return (m_shader && m_use);
}

bool RAS_Shader::Ok2() const
{
	return (m_shader && m_use2);
}

RAS_Shader::RAS_Shader()
	: m_shader(nullptr),
	  m_use(false),
	  m_use2(false),
	  m_error(false),
	  m_dirty(true),
	  m_custom_fbo(0),
	  m_data_texture(0)
{
	for (unsigned short i = 0; i < MAX_PROGRAM; ++i) {
		m_progs[i] = "";
	}
}


RAS_Shader::~RAS_Shader()
{
	ClearUniforms();

	DeleteShader();
}

void RAS_Shader::ClearUniforms()
{
	for (RAS_Uniform *uni : m_uniforms) {
		delete uni;
	}
	m_uniforms.clear();

	for (RAS_DefUniform *uni : m_preDef) {
		delete uni;
	}
	m_preDef.clear();
}

RAS_Shader::RAS_Uniform *RAS_Shader::FindUniform(const int location)
{
#ifdef SORT_UNIFORMS
	for (RAS_Uniform *uni : m_uniforms) {
		if (uni->GetLocation() == location) {
			return uni;
		}
	}
#endif
	return nullptr;
}

void RAS_Shader::SetUniformfv(int location, int type, const float *param, int size, unsigned int count, bool transpose)
{
#ifdef SORT_UNIFORMS
	RAS_Uniform *uni = FindUniform(location);

	if (uni) {
		memcpy(uni->GetData(), param, size);
		uni->SetData(location, type, count, transpose);
	}
	else {
		uni = new RAS_Uniform(size);
		memcpy(uni->GetData(), param, size);
		uni->SetData(location, type, count, transpose);
		m_uniforms.push_back(uni);
	}

	m_dirty = true;
#endif
}

void RAS_Shader::SetUniformiv(int location, int type, const int *param, int size, unsigned int count, bool transpose)
{
#ifdef SORT_UNIFORMS
	RAS_Uniform *uni = FindUniform(location);

	if (uni) {
		memcpy(uni->GetData(), param, size);
		uni->SetData(location, type, count, transpose);
	}
	else {
		uni = new RAS_Uniform(size);
		m_uniforms.push_back(uni);
		memcpy(uni->GetData(), param, size);
		uni->SetData(location, type, count, transpose);
	}

	m_dirty = true;
#endif
}

void RAS_Shader::ApplyShader()
{
#ifdef SORT_UNIFORMS
	if (!m_dirty) {
		return;
	}

	for (unsigned int i = 0; i < m_uniforms.size(); i++) {
		m_uniforms[i]->Apply(this);
	}

	m_dirty = false;
#endif
}

void RAS_Shader::UnloadShader()
{
	//
}

void RAS_Shader::DeleteShader()
{
	if (m_shader) {
		GPU_shader_free(m_shader);
		m_shader = nullptr;
	}
}

std::string RAS_Shader::GetParsedProgram(ProgramType type) const
{
	std::string prog = m_progs[type];
	if (prog.empty()) {
		return prog;
	}

	if (type != COMPUTE_PROGRAM) {
		const unsigned int pos = prog.find("#version");
		if (pos != -1) {
			CM_Warning("found redundant #version directive in shader program, directive ignored.");
			const unsigned int nline = prog.find("\n", pos);
			prog.erase(pos, nline - pos);
		}
		prog.insert(0, "#line 0\n");
	}
	else {
		CM_Debug("Compute shader detectado: mantendo a diretiva #version.");
		// Inserir #line 0 APÓS a linha #version
		const unsigned int versionEnd = prog.find("\n", prog.find("#version"));
		if (versionEnd != std::string::npos) {
			prog.insert(versionEnd + 1, "#line 0\n");
		}
	}

	return prog;
}

bool RAS_Shader::LinkProgram()
{
    std::string vert;
    std::string frag;
    std::string geom;
    std::string comp;

    const bool has_vert = !m_progs[VERTEX_PROGRAM].empty();
    const bool has_frag = !m_progs[FRAGMENT_PROGRAM].empty();
    const bool has_geom = !m_progs[GEOMETRY_PROGRAM].empty();
    const bool has_comp = !m_progs[COMPUTE_PROGRAM].empty();

    CM_Debug(std::string("has_comp? ") + (has_comp ? "SIM" : "NÃO"));
    CM_Debug(std::string("Conteúdo de m_progs[COMPUTE_PROGRAM]: '") + m_progs[COMPUTE_PROGRAM] + "'");

    // === PARTE GRÁFICA ===
    if (has_vert || has_frag) {
        if (!has_vert || !has_frag) {
            CM_Error("Missing vertex or fragment shader.");
            m_error = true;
            return false;
        }

        vert = GetParsedProgram(VERTEX_PROGRAM);
        frag = GetParsedProgram(FRAGMENT_PROGRAM);
        if (has_geom) {
            geom = GetParsedProgram(GEOMETRY_PROGRAM);
        }

        m_shader = GPU_shader_create(
            vert.c_str(),
            frag.c_str(),
            has_geom ? geom.c_str() : nullptr,
            nullptr,
            nullptr,
            0, 0, 0
        );

        if (!m_shader) {
            CM_Error("Failed to compile graphics shader.");
            m_error = true;
            return false;
        }

        ExtractUniformInfos();
    }

    // === VERIFICAR E CRIAR TEXTURAS ===
    const int texSize = 1024;
    char err_out[256] = { 0 };

    auto check_and_create_texture = [&](GPUTexture*& texture, const char* name) {
        bool invalid = !texture || GPU_texture_opengl_bindcode(texture) == 0;

        if (invalid) {
            if (texture) {
                GPU_texture_free(texture);
                texture = nullptr;
                CM_Debug(std::string("Liberando textura inválida: ") + name);
            }

            texture = GPU_texture_create_2D(
                texSize, texSize,
                nullptr,
                GPU_HDR_NONE,
                err_out
            );

            if (!texture) {
                CM_Error(std::string("Failed to create ") + name + ": " + err_out);
                m_error = true;
                return false;
            }
            CM_Debug(std::string("Textura criada com sucesso: ") + name);
        }
        return true;
    };

    if (!check_and_create_texture(m_custom_texture, "CustomTexture") ||
        !check_and_create_texture(m_custom_texture_in, "CustomTextureIn") ||
        !check_and_create_texture(m_custom_texture_out, "CustomTextureOut")) {
        return false;
    }

    // === BIND DOS UNIFORMS ===
    if (m_shader) {
        GPU_shader_bind(m_shader);

        if (BGL_GLOBAL_DEPTH_TEXTURE) {
            GPU_texture_bind(BGL_GLOBAL_DEPTH_TEXTURE, 15);
            int depthTexLoc = GPU_shader_get_uniform(m_shader, "DepthTexture");
            if (depthTexLoc != -1) {
                GPU_shader_uniform_texture(m_shader, depthTexLoc, BGL_GLOBAL_DEPTH_TEXTURE);
            }
        }

        // CustomTexture
        GPU_texture_bind(m_custom_texture, 16);
        int customTexLoc = GPU_shader_get_uniform(m_shader, "CustomTexture");
        if (customTexLoc != -1) {
            GPU_shader_uniform_texture(m_shader, customTexLoc, m_custom_texture);
            CM_Debug("Custom texture vinculada ao uniform 'CustomTexture'.");
        }

        // CustomTextureIN
        GPU_texture_bind(m_custom_texture_in, 17);
        int customTexInLoc = GPU_shader_get_uniform(m_shader, "CustomTextureIn");
        if (customTexInLoc != -1) {
            GPU_shader_uniform_texture(m_shader, customTexInLoc, m_custom_texture_in);
            CM_Debug("CustomTextureIn vinculada ao uniform 'CustomTextureIn'.");
        }

        // CustomTextureOUT
        GPU_texture_bind(m_custom_texture_out, 18);
        int customTexOutLoc = GPU_shader_get_uniform(m_shader, "CustomTextureOut");
        if (customTexOutLoc != -1) {
            GPU_shader_uniform_texture(m_shader, customTexOutLoc, m_custom_texture_out);
            CM_Debug("CustomTextureOut vinculada ao uniform 'CustomTextureOut'.");
        }
    }

    // === PARTE DO COMPUTE SHADER ===
    if (has_comp) {
        comp = GetParsedProgram(COMPUTE_PROGRAM);

        GLuint texID1 = GPU_texture_opengl_bindcode(m_custom_texture);
        GLuint texID2 = GPU_texture_opengl_bindcode(m_custom_texture_in);
        GLuint texID3 = GPU_texture_opengl_bindcode(m_custom_texture_out);

        bool useFluidSim = g_apply2_value;

        // Conversão de std::pair para ComputeUniform
        std::vector<ComputeUniform> convertedUniforms;
        for (const auto& pair : g_ShaderUniform1fBindings) {
            convertedUniforms.push_back({ pair.first, pair.second });
        }

        CM_Debug("---- Compute Shader: Chamando runComputeShader ----");
        if (runComputeShader(comp.c_str(), texID1, texID2, texID3, useFluidSim, convertedUniforms) != GHOST_kSuccess) {
            CM_Error("Failed to compile/execute compute shader.");
            m_error = true;
            return false;
        }
    }

    m_error = false;
    return true;
}



void RAS_Shader::ValidateProgram()
{
	char *log = GPU_shader_validate(m_shader);
	if (log) {
		CM_Debug("---- GLSL Validation ----\n" << log);
		MEM_freeN(log);
	}
}



void RAS_Shader::ExtractUniformInfos()
{
	m_uniformInfos.clear();

	GPUUniformInfo *infos;
	const unsigned int count = GPU_shader_get_uniform_infos(m_shader, &infos);

	for (unsigned short i = 0; i < count; ++i) {
		const GPUUniformInfo& gpuinfo = infos[i];
		// Simple uniforms.
		if (gpuinfo.size == 1) {
			m_uniformInfos.emplace_back(gpuinfo.name, m_shader);
		}
		// Array uniforms.
		else {
			// Store the uniform base name.
			const std::string baseName(gpuinfo.name, 0, strlen(gpuinfo.name) - 3);
			m_uniformInfos.emplace_back(baseName, m_shader);

			// Store location of each uniform items: name[i].
			for (unsigned short i = 0; i < gpuinfo.size; ++i) {
				const std::string name = baseName + '[' + std::to_string(i) + ']';
				m_uniformInfos.emplace_back(name, m_shader);
			}
		}
	}

	if (infos) {
		MEM_freeN(infos);
	}

	// Sort uniforms per name hash for fast search.
	std::sort(m_uniformInfos.begin(), m_uniformInfos.end());
}

bool RAS_Shader::GetError()
{
	return m_error;
}

unsigned int RAS_Shader::GetProg()
{
	return GPU_shader_program(m_shader);
}

GPUShader *RAS_Shader::GetGPUShader()
{
	return m_shader;
}

void RAS_Shader::SetSampler(int loc, int unit)
{
	GPU_shader_uniform_int(m_shader, loc, unit);
}

void RAS_Shader::BindProg()
{
	GPU_shader_bind(m_shader);
}

void RAS_Shader::UnbindProg()
{
	GPU_shader_unbind();
}

void RAS_Shader::SetEnabled(bool enabled)
{
	m_use = enabled;
	m_use2 = enabled;
}

bool RAS_Shader::GetEnabled2() const
{
	return m_use2;
}

bool RAS_Shader::GetEnabled() const
{
	return m_use;
}

bool RAS_Shader::autoRunCompute = false;

void RAS_Shader::Update(RAS_Rasterizer *rasty, const mt::mat4 &model)
{
    if (forceV) {
        CM_Debug(std::string("Conteúdo de m_progs[COMPUTE_PROGRAM]: '") + m_progs[COMPUTE_PROGRAM] + "'");

        const int texSize = 1024;
        char err_out[256] = { 0 };

        auto check_and_create_texture = [&](GPUTexture*& texture, const char* name) {
            bool invalid = !texture || GPU_texture_opengl_bindcode(texture) == 0;
            if (invalid) {
                if (texture) {
                    GPU_texture_free(texture);
                    texture = nullptr;
                    CM_Log(std::string("Liberando textura inválida: ") + name);
                }
                texture = GPU_texture_create_2D(
                    texSize, texSize,
                    nullptr,
                    GPU_HDR_NONE,
                    err_out
                );
                if (!texture) {
                    CM_Error(std::string("Failed to create ") + name + ": " + err_out);
                    m_error = true;
                    return;
                }
                CM_Log(std::string("Textura criada com sucesso: ") + name);
            }
        };

        // ✔️ Verificar e criar as texturas
        check_and_create_texture(m_custom_texture, "CustomTexture");
        check_and_create_texture(m_custom_texture_in, "CustomTextureIn");
        check_and_create_texture(m_custom_texture_out, "CustomTextureOut");

        // ✔️ Executar Compute Shader
        const std::string& comp = m_progs[COMPUTE_PROGRAM];

        GLuint texID1 = GPU_texture_opengl_bindcode(m_custom_texture);
        GLuint texID2 = GPU_texture_opengl_bindcode(m_custom_texture_in);
        GLuint texID3 = GPU_texture_opengl_bindcode(m_custom_texture_out);

        bool useFluidSim = g_apply2_value;

        std::vector<ComputeUniform> convertedUniforms;
        for (const auto& pair : g_ShaderUniform1fBindings) {
            convertedUniforms.push_back({ pair.first, pair.second });
        }

        if (runComputeShader(comp.c_str(), texID1, texID2, texID3, useFluidSim, convertedUniforms) != GHOST_kSuccess) {
            CM_Error("Failed to execute compute shader.");
            m_error = true;
            return;
        }

        CM_Log("Compute shader executado no Update.");

        // ✔️ Vincular texturas ao shader gráfico (.frag)
        if (m_shader) {
            GPU_shader_bind(m_shader);

            if (BGL_GLOBAL_DEPTH_TEXTURE) {
                GPU_texture_bind(BGL_GLOBAL_DEPTH_TEXTURE, 15);
                int depthTexLoc = GPU_shader_get_uniform(m_shader, "DepthTexture");
                if (depthTexLoc != -1) {
                    GPU_shader_uniform_texture(m_shader, depthTexLoc, BGL_GLOBAL_DEPTH_TEXTURE);
                }
            }

            GPU_texture_bind(m_custom_texture, 16);
            int customTexLoc = GPU_shader_get_uniform(m_shader, "CustomTexture");
            if (customTexLoc != -1) {
                GPU_shader_uniform_texture(m_shader, customTexLoc, m_custom_texture);
                CM_Debug("CustomTexture vinculada ao uniform 'CustomTexture'.");
            }

            GPU_texture_bind(m_custom_texture_in, 17);
            int customTexInLoc = GPU_shader_get_uniform(m_shader, "CustomTextureIn");
            if (customTexInLoc != -1) {
                GPU_shader_uniform_texture(m_shader, customTexInLoc, m_custom_texture_in);
                CM_Debug("CustomTextureIn vinculada ao uniform 'CustomTextureIn'.");
            }

            GPU_texture_bind(m_custom_texture_out, 18);
            int customTexOutLoc = GPU_shader_get_uniform(m_shader, "CustomTextureOut");
            if (customTexOutLoc != -1) {
                GPU_shader_uniform_texture(m_shader, customTexOutLoc, m_custom_texture_out);
                CM_Debug("CustomTextureOut vinculada ao uniform 'CustomTextureOut'.");
            }
        }
    }

    // ✔️ Continuação: Atualização dos uniforms padrão (matrizes, tempo, etc.)
    if (!Ok() || m_preDef.empty()) {
        return;
    }

    const mt::mat4 &view = rasty->GetViewMatrix();

    for (RAS_DefUniform *uni : m_preDef) {
        if (uni->m_loc == -1) {
            continue;
        }

        switch (uni->m_type) {
            case MODELMATRIX:
                SetUniform(uni->m_loc, model);
                break;
            case MODELMATRIX_TRANSPOSE:
                SetUniform(uni->m_loc, model, true);
                break;
            case MODELMATRIX_INVERSE:
                SetUniform(uni->m_loc, model.Inverse());
                break;
            case MODELMATRIX_INVERSETRANSPOSE:
                SetUniform(uni->m_loc, model.Inverse(), true);
                break;
            case MODELVIEWMATRIX:
                SetUniform(uni->m_loc, view * model);
                break;
            case MODELVIEWMATRIX_TRANSPOSE: {
                mt::mat4 mat(view * model);
                SetUniform(uni->m_loc, mat, true);
                break;
            }
            case MODELVIEWMATRIX_INVERSE: {
                mt::mat4 mat(view * model);
                SetUniform(uni->m_loc, mat.Inverse());
                break;
            }
            case MODELVIEWMATRIX_INVERSETRANSPOSE: {
                mt::mat4 mat(view * model);
                SetUniform(uni->m_loc, mat.Inverse(), true);
                break;
            }
            case CAM_POS: {
                mt::vec3 pos(rasty->GetCameraPosition());
                SetUniform(uni->m_loc, pos);
                break;
            }
            case VIEWMATRIX:
                SetUniform(uni->m_loc, view);
                break;
            case VIEWMATRIX_TRANSPOSE:
                SetUniform(uni->m_loc, view, true);
                break;
            case VIEWMATRIX_INVERSE:
                SetUniform(uni->m_loc, view.Inverse());
                break;
            case VIEWMATRIX_INVERSETRANSPOSE:
                SetUniform(uni->m_loc, view.Inverse(), true);
                break;
            case CONSTANT_TIMER:
                SetUniform(uni->m_loc, (float)rasty->GetTime());
                break;
            case EYE:
                SetUniform(uni->m_loc, (rasty->GetEye() == RAS_Rasterizer::RAS_STEREO_LEFTEYE) ? 0.0f : 0.5f);
                break;
            default:
                break;
        }
    }
}






int RAS_Shader::GetAttribLocation(const std::string& name)
{
	return GPU_shader_get_attribute(m_shader, name.c_str());
}

void RAS_Shader::BindAttribute(const std::string& attr, int loc)
{
	GPU_shader_bind_attribute(m_shader, loc, attr.c_str());
}

int RAS_Shader::GetUniformLocation(const std::string& name, bool debug)
{
	BLI_assert(m_shader != nullptr);

	const size_t hash = std::hash<std::string>()(name);
	// Use binary search based on hashed name.
	std::vector<UniformInfo>::const_iterator it = std::lower_bound(m_uniformInfos.begin(), m_uniformInfos.end(), hash,
		[](const UniformInfo& info, size_t hash){ return (info.nameHash < hash); });

	if (it == m_uniformInfos.end() || it->nameHash != hash) {
		if (debug) {
			CM_Error("invalid uniform value: " << name << ".");
		}
		return -1;
	}
	return it->location;
}

void RAS_Shader::SetUniform(int uniform, const mt::vec2 &vec)
{
	GPU_shader_uniform_vector(m_shader, uniform, 2, 1, vec.Data());
}

void RAS_Shader::SetUniform(int uniform, const mt::vec3 &vec)
{
	GPU_shader_uniform_vector(m_shader, uniform, 3, 1, vec.Data());
}

void RAS_Shader::SetUniform(int uniform, const mt::vec4 &vec)
{
	GPU_shader_uniform_vector(m_shader, uniform, 4, 1, vec.Data());
}

void RAS_Shader::SetUniform(int uniform, const unsigned int &val)
{
	GPU_shader_uniform_int(m_shader, uniform, val);
}

void RAS_Shader::SetUniform(int uniform, const int val)
{
	GPU_shader_uniform_int(m_shader, uniform, val);
}

void RAS_Shader::SetUniform(int uniform, const float &val)
{
	GPU_shader_uniform_float(m_shader, uniform, val);
}

void RAS_Shader::SetUniform(int uniform, const mt::mat4 &vec, bool transpose)
{
	GPU_shader_uniform_vector(m_shader, uniform, 16, 1, (float *)vec.Data());
}

void RAS_Shader::SetUniform(int uniform, const mt::mat3 &vec, bool transpose)
{
	float value[9];
	vec.Pack(value);
	GPU_shader_uniform_vector(m_shader, uniform, 9, 1, value);
}

void RAS_Shader::SetUniform(int uniform, const float *val, int len)
{
	if (len >= 2 && len <= 4) {
		GPU_shader_uniform_vector(m_shader, uniform, len, 1, (float *)val);
	}
	else {
		BLI_assert(0);
	}
}

void RAS_Shader::SetUniform(int uniform, const int *val, int len)
{
	if (len >= 2 && len <= 4) {
		GPU_shader_uniform_vector_int(m_shader, uniform, len, 1, (int *)val);
	}
	else {
		BLI_assert(0);
	}
}