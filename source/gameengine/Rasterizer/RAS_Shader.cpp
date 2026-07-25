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
#include "RAS_OffScreen.h"
#include "RAS_2DFilterOffScreen.h"
#include "../../../intern/ghost/intern/GHOST_ContextWGL.h"
#include "../../../intern/ghost/GHOST_Types.h"
#include "../../../source/blender/gpu/GPU_texture.h"
#include "../../../source/blender/gpu/GPU_glew.h"
#include "../../../source/blender/gpu/GPU_framebuffer.h"
#include "../../../source/blender/gpu/GPU_shader.h"
#include "../../Ketsji/BL_shader.h"

#include <vector>
#include <string>
#include <utility>
#include <cstring>

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
	  m_use(true),
	  m_use2(true),
	  m_error(false),
	  m_dirty(true),
	  m_lastFrameCount(0xFFFFFFFF),
	  m_lastEye(-1),
	  m_custom_fbo(0),
	  m_data_texture(0),
	  m_viewInverseValid(false),
	  m_viewTransposeValid(false),
	  m_viewInverseTransposeValid(false)
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
		if (memcmp(uni->GetData(), param, size) == 0) {
			return;
		}
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
		if (memcmp(uni->GetData(), param, size) == 0) {
			return;
		}
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

struct GPUShader {
	GLuint program;   /* handle for full program (links shader stages below) */

	GLuint vertex;    /* handle for vertex shader */
	GLuint geometry;  /* handle for geometry shader */
	GLuint fragment;  /* handle for fragment shader */
	GLuint compute;   /* handle for compute shader (ThunderPlayer) */

	GLuint tess_control;     /* handle for tessellation control shader */
	GLuint tess_evaluation;  /* handle for tessellation evaluation shader */

	int totattrib;    /* total number of attributes */
	int uniforms;     /* required uniforms */

	void *uniform_interface; /* cached uniform interface for shader. Data depends on shader */
};

std::string RAS_Shader::GetParsedProgram(ProgramType type) const
{
	std::string prog = m_progs[type];
	if (prog.empty()) {
		return prog;
	}

	const bool keepVersion =
		(type == COMPUTE_PROGRAM) ||
		(type == TESS_CONTROL_PROGRAM) ||
		(type == TESS_EVALUATION_PROGRAM);

	if (!keepVersion) {
		size_t pos = prog.find("#version");
		if (pos != std::string::npos) {
			size_t nline = prog.find('\n', pos);
			if (nline == std::string::npos) {
				nline = prog.size();
			} else {
				nline += 1;
			}

			CM_Warning("found redundant #version directive in non-tessellation shader, directive ignored.");
			prog.erase(pos, nline - pos);
		}

		prog.insert(0, "#line 0\n");
	}
	else {
		switch (type) {
			case COMPUTE_PROGRAM:
				CM_Debug("Compute shader detectado: mantendo diretiva #version.");
				break;
			case TESS_CONTROL_PROGRAM:
				CM_Debug("Tessellation Control Shader detectado: mantendo diretiva #version.");
				break;
			case TESS_EVALUATION_PROGRAM:
				CM_Debug("Tessellation Evaluation Shader detectado: mantendo diretiva #version.");
				break;
			default:
				break;
		}

		size_t versionPos = prog.find("#version");
		if (versionPos != std::string::npos) {
			size_t versionEnd = prog.find('\n', versionPos);
			if (versionEnd == std::string::npos)
				versionEnd = prog.size() - 1;
			prog.insert(versionEnd + 1, "\n#line 0\n");
		} else {
			CM_Warning("Shader de tessellation/compute sem #version detectado — inserindo manualmente #version 410 core.");
			prog.insert(0, "#version 410 core\n#line 0\n");
		}
	}

	return prog;
}

#define CM_Log(msg) fprintf(stdout, "[CM_LOG] %s\n", msg)
#define CM_Logf(fmt, ...) fprintf(stdout, "[CM_LOG] " fmt "\n", __VA_ARGS__)
#define CM_Errorf(fmt, ...) fprintf(stderr, "[CM_ERROR] " fmt "\n", __VA_ARGS__)

bool RAS_Shader::RunTessellationShader(const char *tessCtrlSrc, const char *tessEvalSrc)
{
	if (!m_shader || !m_shader->program) {
		CM_Error("RunTessellationShader: GPUShader inválido.");
		return false;
	}

	GLuint tcs = glCreateShader(GL_TESS_CONTROL_SHADER);
	glShaderSource(tcs, 1, &tessCtrlSrc, NULL);
	glCompileShader(tcs);

	GLint success = 0;
	glGetShaderiv(tcs, GL_COMPILE_STATUS, &success);
	if (!success) {
		char log[512];
		glGetShaderInfoLog(tcs, 512, NULL, log);
		CM_Errorf("Erro ao compilar TCS:\n%s", log);
		glDeleteShader(tcs);
		return false;
	}

	GLuint tes = glCreateShader(GL_TESS_EVALUATION_SHADER);
	glShaderSource(tes, 1, &tessEvalSrc, NULL);
	glCompileShader(tes);
	glGetShaderiv(tes, GL_COMPILE_STATUS, &success);
	if (!success) {
		char log[512];
		glGetShaderInfoLog(tes, 512, NULL, log);
		CM_Errorf("Erro ao compilar TES:\n%s", log);
		glDeleteShader(tcs);
		glDeleteShader(tes);
		return false;
	}

	glAttachShader(m_shader->program, tcs);
	glAttachShader(m_shader->program, tes);
	glLinkProgram(m_shader->program);

	GLint linkStatus = 0;
	glGetProgramiv(m_shader->program, GL_LINK_STATUS, &linkStatus);
	if (!linkStatus) {
		char log[512];
		glGetProgramInfoLog(m_shader->program, 512, NULL, log);
		CM_Errorf("Erro ao relinkar GPUShader com tessellation:\n%s", log);
		glDeleteShader(tcs);
		glDeleteShader(tes);
		return false;
	}

	glDeleteShader(tcs);
	glDeleteShader(tes);

	CM_Log("Shaders de tessellation anexados e linkados com sucesso.");
	return true;
}

bool RAS_Shader::LinkProgram()
{
    std::string vert, frag, geom, comp, tessCtrl, tessEval;

    const bool has_vert = !m_progs[VERTEX_PROGRAM].empty();
    const bool has_frag = !m_progs[FRAGMENT_PROGRAM].empty();
    const bool has_geom = !m_progs[GEOMETRY_PROGRAM].empty();
    const bool has_comp = !m_progs[COMPUTE_PROGRAM].empty();
    const bool has_tess_ctrl = !m_progs[TESS_CONTROL_PROGRAM].empty();
    const bool has_tess_eval = !m_progs[TESS_EVALUATION_PROGRAM].empty();

    CM_Debug(std::string("has_comp? ") + (has_comp ? "SIM" : "NÃO"));
    CM_Debug(std::string("has_tessellation? ") + ((has_tess_ctrl && has_tess_eval) ? "SIM" : "NÃO"));

    // === PARTE GRÁFICA ===
    if (has_vert || has_frag || has_tess_ctrl || has_tess_eval) {
        if (!has_vert || !has_frag) {
            CM_Error("Missing vertex or fragment shader.");
            m_error = true;
            return false;
        }

        vert = GetParsedProgram(VERTEX_PROGRAM);
        frag = GetParsedProgram(FRAGMENT_PROGRAM);
        if (has_geom) geom = GetParsedProgram(GEOMETRY_PROGRAM);
        if (has_tess_ctrl) tessCtrl = GetParsedProgram(TESS_CONTROL_PROGRAM);
        if (has_tess_eval) tessEval = GetParsedProgram(TESS_EVALUATION_PROGRAM);

        if (has_tess_ctrl && has_tess_eval) {
            glPatchParameteri(GL_PATCH_VERTICES, 3);
            CM_Debug("Definido glPatchParameteri(GL_PATCH_VERTICES, 3);");
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
            CM_Error("Failed to compile graphics shader (vertex/fragment/tessellation).");
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

            texture = GPU_texture_create_2D(texSize, texSize, nullptr, GPU_HDR_NONE, err_out);
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

        if (g_DebugDepthTex) {
            GPU_texture_bind(g_DebugDepthTex, 15);
            int depthTexLoc = GPU_shader_get_uniform(m_shader, "DepthTexture");
            if (depthTexLoc != -1)
                GPU_shader_uniform_texture(m_shader, depthTexLoc, g_DebugDepthTex);
        }

        auto bindTexUniform = [&](GPUTexture* tex, int unit, const char* name) {
            GPU_texture_bind(tex, unit);
            int loc = GPU_shader_get_uniform(m_shader, name);
            if (loc != -1) {
                GPU_shader_uniform_texture(m_shader, loc, tex);
                CM_Debug(std::string(name) + " vinculado ao uniform.");
            }
        };

        bindTexUniform(m_custom_texture, 16, "CustomTexture");
        bindTexUniform(m_custom_texture_in, 17, "CustomTextureIn");
        bindTexUniform(m_custom_texture_out, 18, "CustomTextureOut");
    }

    // === PARTE DO COMPUTE SHADER ===
    if (has_comp) {
        comp = GetParsedProgram(COMPUTE_PROGRAM);
        GLuint texID1 = GPU_texture_opengl_bindcode(m_custom_texture);
        GLuint texID2 = GPU_texture_opengl_bindcode(m_custom_texture_in);
        GLuint texID3 = GPU_texture_opengl_bindcode(m_custom_texture_out);
        bool useFluidSim = g_apply2_value;

        std::vector<ComputeUniform> convertedUniforms;
        for (const auto& pair : g_ShaderUniform1fBindings)
            convertedUniforms.push_back({ pair.first, pair.second });

        CM_Debug("---- Compute Shader: Chamando runComputeShader ----");
        if (runComputeShader(comp.c_str(), texID1, texID2, texID3, useFluidSim, convertedUniforms) != GHOST_kSuccess) {
            CM_Error("Failed to compile/execute compute shader.");
            m_error = true;
            return false;
        }
    }

    // === PARTE DO TESSELLATION SHADER ===
    if (has_tess_ctrl && has_tess_eval) {
        tessCtrl = GetParsedProgram(TESS_CONTROL_PROGRAM);
        tessEval = GetParsedProgram(TESS_EVALUATION_PROGRAM);

        CM_Debug("---- Tessellation Shader: Chamando runTessellationShader ----");
		RunTessellationShader(tessCtrl.c_str(), tessEval.c_str());
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

void RAS_Shader::SetSamplerBindless(int loc, GLuint64 handle)
{
	glProgramUniformHandleui64ARB(m_shader->program, loc, handle);
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

        check_and_create_texture(m_custom_texture, "CustomTexture");
        check_and_create_texture(m_custom_texture_in, "CustomTextureIn");
        check_and_create_texture(m_custom_texture_out, "CustomTextureOut");

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


        if (m_shader) {
            GPU_shader_bind(m_shader);

            if (g_DebugDepthTex) {
                GPU_texture_bind(g_DebugDepthTex, 15);
                int depthTexLoc = GPU_shader_get_uniform(m_shader, "DepthTexture");
                if (depthTexLoc != -1) {
                    GPU_shader_uniform_texture(m_shader, depthTexLoc, g_DebugDepthTex);
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

    if (!Ok() || m_preDef.empty()) {
        return;
    }

    const unsigned int currentFrame = rasty->GetFrameCount();
    const int currentEye = (int)rasty->GetEye();
    const mt::mat4 &view = rasty->GetViewMatrix();

    const bool frameChanged = (m_lastFrameCount != currentFrame);
    const bool eyeChanged = (m_lastEye != currentEye);
    const bool viewChanged = (frameChanged || eyeChanged || memcmp(&m_lastViewMatrix, &view, sizeof(mt::mat4)) != 0);
    const bool modelChanged = (memcmp(&m_lastModelMatrix, &model, sizeof(mt::mat4)) != 0);

    if (viewChanged) {
        m_viewInverseValid = false;
        m_viewTransposeValid = false;
        m_viewInverseTransposeValid = false;
    }

    mt::mat4 modelView;
    bool modelViewValid = false;
    mt::mat4 modelInverse;
    bool modelInverseValid = false;

    for (RAS_DefUniform *uni : m_preDef) {
        if (uni->m_loc == -1) {
            continue;
        }

        switch (uni->m_type) {
            case MODELMATRIX:
                if (modelChanged) SetUniform(uni->m_loc, model);
                break;
            case MODELMATRIX_TRANSPOSE:
                if (modelChanged) SetUniform(uni->m_loc, model, true);
                break;
            case MODELMATRIX_INVERSE:
                if (modelChanged) {
                    if (!modelInverseValid) {
                        modelInverse = model.Inverse();
                        modelInverseValid = true;
                    }
                    SetUniform(uni->m_loc, modelInverse);
                }
                break;
            case MODELMATRIX_INVERSETRANSPOSE:
                if (modelChanged) {
                    if (!modelInverseValid) {
                        modelInverse = model.Inverse();
                        modelInverseValid = true;
                    }
                    SetUniform(uni->m_loc, modelInverse, true);
                }
                break;
            case MODELVIEWMATRIX:
                if (modelChanged || viewChanged) {
                    if (!modelViewValid) {
                        modelView = view * model;
                        modelViewValid = true;
                    }
                    SetUniform(uni->m_loc, modelView);
                }
                break;
            case MODELVIEWMATRIX_TRANSPOSE: {
                if (modelChanged || viewChanged) {
                    if (!modelViewValid) {
                        modelView = view * model;
                        modelViewValid = true;
                    }
                    SetUniform(uni->m_loc, modelView, true);
                }
                break;
            }
            case MODELVIEWMATRIX_INVERSE: {
                if (modelChanged || viewChanged) {
                    if (!modelViewValid) {
                        modelView = view * model;
                        modelViewValid = true;
                    }
                    SetUniform(uni->m_loc, modelView.Inverse());
                }
                break;
            }
            case MODELVIEWMATRIX_INVERSETRANSPOSE: {
                if (modelChanged || viewChanged) {
                    if (!modelViewValid) {
                        modelView = view * model;
                        modelViewValid = true;
                    }
                    SetUniform(uni->m_loc, modelView.Inverse(), true);
                }
                break;
            }
            case CAM_POS: {
                if (viewChanged) {
                    mt::vec3 pos(rasty->GetCameraPosition());
                    SetUniform(uni->m_loc, pos);
                }
                break;
            }
            case VIEWMATRIX:
                if (viewChanged) SetUniform(uni->m_loc, view);
                break;
            case VIEWMATRIX_TRANSPOSE:
                if (viewChanged) {
                    if (!m_viewTransposeValid) {
                        m_cachedViewTranspose = view; // SetUniform handles transpose if true is passed
                        m_viewTransposeValid = true;
                    }
                    SetUniform(uni->m_loc, view, true);
                }
                break;
            case VIEWMATRIX_INVERSE:
                if (viewChanged) {
                    if (!m_viewInverseValid) {
                        m_cachedViewInverse = view.Inverse();
                        m_viewInverseValid = true;
                    }
                    SetUniform(uni->m_loc, m_cachedViewInverse);
                }
                break;
            case VIEWMATRIX_INVERSETRANSPOSE:
                if (viewChanged) {
                    if (!m_viewInverseTransposeValid) {
                        if (!m_viewInverseValid) {
                            m_cachedViewInverse = view.Inverse();
                            m_viewInverseValid = true;
                        }
                        m_cachedViewInverseTranspose = m_cachedViewInverse;
                        m_viewInverseTransposeValid = true;
                    }
                    SetUniform(uni->m_loc, m_cachedViewInverse, true);
                }
                break;
            case CONSTANT_TIMER:
                if (frameChanged) SetUniform(uni->m_loc, (float)rasty->GetTime());
                break;
            case EYE:
                if (eyeChanged) SetUniform(uni->m_loc, (currentEye == RAS_Rasterizer::RAS_STEREO_LEFTEYE) ? 0.0f : 0.5f);
                break;
            default:
                break;
        }
    }

    m_lastFrameCount = currentFrame;
    m_lastEye = currentEye;
    memcpy(&m_lastModelMatrix, &model, sizeof(mt::mat4));
    memcpy(&m_lastViewMatrix, &view, sizeof(mt::mat4));
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