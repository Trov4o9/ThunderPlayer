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

/** \file gameengine/Rasterizer/RAS_OpenGLRasterizer/RAS_OpenGLRasterizer.cpp
 *  \ingroup bgerastogl
 */

#include "RAS_OpenGLRasterizer.h"
#include "RAS_IMaterial.h"

#include "GPU_glew.h"
#include <sstream>
#include <string>
#include "RAS_MeshUser.h"

#include "GPU_draw.h"
#include "GPU_extensions.h"
#include "GPU_material.h"
#include "GPU_shader.h"
#include "GPU_vertex_array.h"

extern "C" {
#  include "BLF_api.h"
}

#include "MEM_guardedalloc.h"

#include "CM_Message.h"

#include <cstring> // For memcpy.

// WARNING: Always respect the order from RAS_Rasterizer::EnableBit.
static const int openGLEnableBitEnums[] = {
	GL_DEPTH_TEST,         // RAS_DEPTH_TEST
	GL_NONE,               // RAS_ALPHA_TEST  (FFP removed — use discard in shader)
	GL_SCISSOR_TEST,       // RAS_SCISSOR_TEST
	GL_TEXTURE_2D,         // RAS_TEXTURE_2D
	GL_TEXTURE_CUBE_MAP_ARB, // RAS_TEXTURE_CUBE_MAP
	GL_BLEND,              // RAS_BLEND
	GL_NONE,               // RAS_COLOR_MATERIAL (FFP removed)
	GL_CULL_FACE,          // RAS_CULL_FACE
	GL_NONE,               // RAS_LIGHTING (FFP removed — lighting done in shaders)
	GL_MULTISAMPLE_ARB,    // RAS_MULTISAMPLE
	GL_POLYGON_STIPPLE,    // RAS_POLYGON_STIPPLE
	GL_POLYGON_OFFSET_FILL, // RAS_POLYGON_OFFSET_FILL
	GL_POLYGON_OFFSET_LINE, // RAS_POLYGON_OFFSET_LINE
	GL_NONE,               // RAS_TEXTURE_GEN_S (FFP removed)
	GL_NONE,               // RAS_TEXTURE_GEN_T (FFP removed)
	GL_NONE,               // RAS_TEXTURE_GEN_R (FFP removed)
	GL_NONE                // RAS_TEXTURE_GEN_Q (FFP removed)
};

// WARNING: Always respect the order from RAS_Rasterizer::DepthFunc.
static const int openGLDepthFuncEnums[] = {
	GL_NEVER, // RAS_NEVER
	GL_LEQUAL, // RAS_LEQUAL
	GL_LESS, // RAS_LESS
	GL_ALWAYS, // RAS_ALWAYS
	GL_GEQUAL, // RAS_GEQUAL
	GL_GREATER, // RAS_GREATER
	GL_NOTEQUAL, // RAS_NOTEQUAL
	GL_EQUAL // RAS_EQUAL
};

// WARNING: Always respect the order from RAS_Rasterizer::MatrixMode.
static const int openGLMatrixModeEnums[] = {
	GL_PROJECTION, // RAS_PROJECTION
	GL_MODELVIEW, // RAS_MODELVIEW
	GL_TEXTURE // RAS_TEXTURE
};

// WARNING: Always respect the order from RAS_Rasterizer::BlendFunc.
static const int openGLBlendFuncEnums[] = {
	GL_ZERO, // RAS_ZERO,
	GL_ONE, // RAS_ONE,
	GL_SRC_COLOR, // RAS_SRC_COLOR,
	GL_ONE_MINUS_SRC_COLOR, // RAS_ONE_MINUS_SRC_COLOR,
	GL_DST_COLOR, // RAS_DST_COLOR,
	GL_ONE_MINUS_DST_COLOR, // RAS_ONE_MINUS_DST_COLOR,
	GL_SRC_ALPHA, // RAS_SRC_ALPHA,
	GL_ONE_MINUS_SRC_ALPHA, // RAS_ONE_MINUS_SRC_ALPHA,
	GL_DST_ALPHA, // RAS_DST_ALPHA,
	GL_ONE_MINUS_DST_ALPHA, // RAS_ONE_MINUS_DST_ALPHA,
	GL_SRC_ALPHA_SATURATE // RAS_SRC_ALPHA_SATURATE
};

RAS_OpenGLRasterizer::ScreenPlane::ScreenPlane()
{
	// Generate the VBO and IBO for screen overlay plane.
	glGenBuffers(1, &m_vbo);
	glGenBuffers(1, &m_ibo);
	GPU_create_vertex_arrays(1, &m_vao);

	static const float vertices[] = {
		-1.0f, -1.0f, 1.0f, 0.0f, 0.0f,
		-1.0f,  1.0f, 1.0f, 0.0f, 1.0f,
		 1.0f,  1.0f, 1.0f, 1.0f, 1.0f,
		 1.0f, -1.0f, 1.0f, 1.0f, 0.0f
	};

	static const GLubyte indices[] = {3, 2, 1, 0};

	GPU_bind_vertex_array(m_vao);

	// --- IBO ---
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibo);
	glBufferStorage(
		GL_ELEMENT_ARRAY_BUFFER,
		sizeof(indices),
		indices,
		0
	);

	// --- VBO ---
	glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
	glBufferStorage(
		GL_ARRAY_BUFFER,
		sizeof(vertices),
		vertices,
		0 
	);

	// Enable old-school pointers
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);

	glVertexPointer(3, GL_FLOAT, sizeof(float) * 5, 0);
	glTexCoordPointer(2, GL_FLOAT, sizeof(float) * 5, ((char*)nullptr) + sizeof(float) * 3);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	GPU_unbind_vertex_array();
}

RAS_OpenGLRasterizer::ScreenPlane::~ScreenPlane()
{
	GPU_delete_vertex_arrays(1, &m_vao);
	glDeleteBuffers(1, &m_vbo);
	glDeleteBuffers(1, &m_ibo);
}


inline void RAS_OpenGLRasterizer::ScreenPlane::Render()
{
    GPU_bind_vertex_array(m_vao);
    
    // Draw in triangle fan mode to reduce IBO size.
    glDrawElements(GL_TRIANGLE_FAN, 4, GL_UNSIGNED_BYTE, 0);

    GPU_unbind_vertex_array();
}

RAS_OpenGLRasterizer::RAS_OpenGLRasterizer(RAS_Rasterizer *rasterizer)
	:m_rasterizer(rasterizer)
{
}

RAS_OpenGLRasterizer::~RAS_OpenGLRasterizer()
{
}

unsigned short RAS_OpenGLRasterizer::GetNumLights() const
{
	// FFP lighting removed — fixed-function GL_LIGHT slots no longer used.
	return 0;
}

void RAS_OpenGLRasterizer::Enable(RAS_Rasterizer::EnableBit bit)
{
	glEnable(openGLEnableBitEnums[bit]);
}

void RAS_OpenGLRasterizer::Disable(RAS_Rasterizer::EnableBit bit)
{
	glDisable(openGLEnableBitEnums[bit]);
}

void RAS_OpenGLRasterizer::EnableLight(unsigned short /*count*/)
{
	// FFP GL_LIGHT slots removed — lighting is handled in shaders.
}

void RAS_OpenGLRasterizer::DisableLight(unsigned short /*count*/)
{
	// FFP GL_LIGHT slots removed.
}

void RAS_OpenGLRasterizer::SetDepthFunc(RAS_Rasterizer::DepthFunc func)
{
	glDepthFunc(openGLDepthFuncEnums[func]);
}

void RAS_OpenGLRasterizer::SetBlendFunc(RAS_Rasterizer::BlendFunc src, RAS_Rasterizer::BlendFunc dst)
{
	glBlendFunc(openGLBlendFuncEnums[src], openGLBlendFuncEnums[dst]);
}

void RAS_OpenGLRasterizer::Init()
{
	// FFP glShadeModel removed — shading model is defined in vertex/fragment shaders.
}

void RAS_OpenGLRasterizer::SetAmbient(const mt::vec3& /*amb*/, float /*factor*/)
{
	// FFP glLightModelfv removed — ambient is passed via shader uniform.
}

void RAS_OpenGLRasterizer::SetFog(short /*type*/, float /*start*/, float /*dist*/, float /*intensity*/, const mt::vec3& /*color*/)
{
	// FFP fog removed — fog is implemented in fragment shaders.
}

void RAS_OpenGLRasterizer::Exit()
{
	// FFP glLightModeli removed.
}

void RAS_OpenGLRasterizer::BeginFrame()
{
	// FFP glShadeModel removed.
}

void RAS_OpenGLRasterizer::SetDepthMask(RAS_Rasterizer::DepthMask depthmask)
{
	glDepthMask(depthmask == RAS_Rasterizer::RAS_DEPTHMASK_DISABLED ? GL_FALSE : GL_TRUE);
}

unsigned int *RAS_OpenGLRasterizer::MakeScreenshot(int x, int y, int width, int height)
{
	unsigned int *pixeldata = nullptr;

	if (width && height) {
		pixeldata = (unsigned int *)malloc(sizeof(unsigned int) * width * height);
		glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixeldata);
	}

	return pixeldata;
}

void RAS_OpenGLRasterizer::Clear(int clearbit)
{
	GLbitfield glclearbit = 0;

	if (clearbit & RAS_Rasterizer::RAS_COLOR_BUFFER_BIT) {
		glclearbit |= GL_COLOR_BUFFER_BIT;
	}
	if (clearbit & RAS_Rasterizer::RAS_DEPTH_BUFFER_BIT) {
		glclearbit |= GL_DEPTH_BUFFER_BIT;
	}
	if (clearbit & RAS_Rasterizer::RAS_STENCIL_BUFFER_BIT) {
		glclearbit |= GL_STENCIL_BUFFER_BIT;
	}

	glClear(glclearbit);
}

void RAS_OpenGLRasterizer::SetClearColor(float r, float g, float b, float a)
{
	glClearColor(r, g, b, a);
}

void RAS_OpenGLRasterizer::SetClearDepth(float d)
{
	glClearDepth(d);
}

void RAS_OpenGLRasterizer::SetColorMask(bool r, bool g, bool b, bool a)
{
	glColorMask(r ? GL_TRUE : GL_FALSE, g ? GL_TRUE : GL_FALSE, b ? GL_TRUE : GL_FALSE, a ? GL_TRUE : GL_FALSE);
}

void RAS_OpenGLRasterizer::DrawOverlayPlane()
{
	m_screenPlane.Render();
}

void RAS_OpenGLRasterizer::SetViewport(int x, int y, int width, int height)
{
	glViewport(x, y, width, height);
}

void RAS_OpenGLRasterizer::GetViewport(int *rect)
{
	glGetIntegerv(GL_VIEWPORT, rect);
}

void RAS_OpenGLRasterizer::SetScissor(int x, int y, int width, int height)
{
	glScissor(x, y, width, height);
}

void RAS_OpenGLRasterizer::SetLines(bool enable)
{
	if (enable) {
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
	}
	else {
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	}
}

void RAS_OpenGLRasterizer::SetSpecularity(float /*specX*/, float /*specY*/, float /*specZ*/, float /*specval*/)
{
	// FFP glMaterialfv removed — specularity is passed via shader uniform.
}

void RAS_OpenGLRasterizer::SetShinyness(float /*shiny*/)
{
	// FFP glMaterialfv removed — shininess is passed via shader uniform.
}

void RAS_OpenGLRasterizer::SetDiffuse(float /*difX*/, float /*difY*/, float /*difZ*/, float /*diffuse*/)
{
	// FFP glMaterialfv removed — diffuse is passed via shader uniform.
}

void RAS_OpenGLRasterizer::SetEmissive(float /*eX*/, float /*eY*/, float /*eZ*/, float /*e*/)
{
	// FFP glMaterialfv removed — emissive is passed via shader uniform.
}

void RAS_OpenGLRasterizer::SetPolygonOffset(float mult, float add)
{
	glPolygonOffset(mult, add);
}

void RAS_OpenGLRasterizer::EnableClipPlane(unsigned short index, const mt::vec4& plane)
{
	double planev[4] = {plane.x, plane.y, plane.z, plane.w};
	glClipPlane(GL_CLIP_PLANE0 + index, planev);
	glEnable(GL_CLIP_PLANE0 + index);
}

void RAS_OpenGLRasterizer::DisableClipPlane(unsigned short index)
{
	glDisable(GL_CLIP_PLANE0 + index);
}

void RAS_OpenGLRasterizer::SetFrontFace(bool ccw)
{
	if (m_cache.m_frontFaceCCW != ccw) {
		if (ccw) {
			glFrontFace(GL_CCW);
		}
		else {
			glFrontFace(GL_CW);
		}
		m_cache.m_frontFaceCCW = ccw;
	}
}

void RAS_OpenGLRasterizer::EnableLights()
{
	// FFP glColorMaterial / glLightModeli removed — lighting state managed in shaders.
}

void RAS_OpenGLRasterizer::DisableForText()
{
	for (int i = 0; i < RAS_Texture::MaxUnits; i++) {
		if (m_cache.m_enableBits[RAS_Rasterizer::RAS_TEXTURE_CUBE_MAP] ||
		    m_cache.m_enableBits[RAS_Rasterizer::RAS_TEXTURE_2D])
		{
			glActiveTextureARB(GL_TEXTURE0_ARB + i);

			if (GLEW_ARB_texture_cube_map && m_cache.m_enableBits[RAS_Rasterizer::RAS_TEXTURE_CUBE_MAP]) {
				glDisable(GL_TEXTURE_CUBE_MAP_ARB);
				m_cache.m_enableBits[RAS_Rasterizer::RAS_TEXTURE_CUBE_MAP] = false;
			}
			if (m_cache.m_enableBits[RAS_Rasterizer::RAS_TEXTURE_2D]) {
				glDisable(GL_TEXTURE_2D);
				m_cache.m_enableBits[RAS_Rasterizer::RAS_TEXTURE_2D] = false;
			}
		}
	}

	glActiveTextureARB(GL_TEXTURE0_ARB);
}

void RAS_OpenGLRasterizer::RenderText3D(
    int fontid, const std::string& text, int size, int dpi,
    const float color[4], const float mat[16], float aspect)
{
    /* Preparação GL */
    m_rasterizer->DisableForText();
    SetFrontFace(true);

    // BLF state is stored per font id. Caching this with static globals leaks
    // size/matrix/aspect from one font to another and breaks font switching.
    BLF_size(fontid, size, dpi);
    BLF_enable(fontid, BLF_MATRIX | BLF_ASPECT);
    BLF_matrix(fontid, mat);
    BLF_aspect(fontid, aspect, aspect, aspect);

    /* Posição */
    BLF_position(fontid, 0, 0, 0);

    /* Desenhar */
    BLF_draw(fontid, text.c_str(), text.size());

    /* MUITO IMPORTANTE: restaurar estado */
    BLF_disable(fontid, BLF_MATRIX | BLF_ASPECT);

    m_rasterizer->SetAlphaBlend(GPU_BLEND_SOLID);
}


void RAS_OpenGLRasterizer::PushMatrix()
{
	glPushMatrix();
}

void RAS_OpenGLRasterizer::PopMatrix()
{
	glPopMatrix();
}

void RAS_OpenGLRasterizer::SetMatrixMode(RAS_Rasterizer::MatrixMode mode)
{
	if (m_cache.m_matrixMode != mode) {
		glMatrixMode(openGLMatrixModeEnums[mode]);
		m_cache.m_matrixMode = mode;
	}
}

void RAS_OpenGLRasterizer::MultMatrix(const float mat[16])
{
	glMultMatrixf(mat);
}

void RAS_OpenGLRasterizer::LoadMatrix(const float mat[16])
{
	glLoadMatrixf(mat);
}

void RAS_OpenGLRasterizer::LoadIdentity()
{
	glLoadIdentity();
}

void RAS_OpenGLRasterizer::MotionBlur(unsigned short state, float value)
{
	if (state) {
		if (state == 1) {
			// bugfix:load color buffer into accum buffer for the first time(state=1)
			glAccum(GL_LOAD, 1.0f);
			m_rasterizer->SetMotionBlur(2);
		}
		else if (value >= 0.0f && value <= 1.0f) {
			glAccum(GL_MULT, value);
			glAccum(GL_ACCUM, 1.0f - value);
			glAccum(GL_RETURN, 1.0f);
			//glFlush();
		}
	}
}

void RAS_OpenGLRasterizer::PrintVRAMUsage()
{
    CM_Message("=== VRAM USAGE ===");

    // NVIDIA
    if (GLEW_NVX_gpu_memory_info) {

        GLint total_kb = -1;
        GLint avail_kb = -1;

        glGetIntegerv(GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX, &total_kb);
        glGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &avail_kb);

        if (total_kb == 0 && avail_kb == 0) {
            CM_Message("Vendor: NVIDIA (NVX_gpu_memory_info)");
            CM_Message("  ERRO: O driver retornou 0 KB para todas as leituras.");
            CM_Message("  Isso acontece em contexto OpenGL antigo.");
            CM_Message("  O valor real de VRAM não pode ser obtido via NVX no contexto atual.");
            return;
        }

        if (total_kb > 0 && avail_kb >= 0) {
            int used_kb = total_kb - avail_kb;

            CM_Message("Vendor: NVIDIA (NVX_gpu_memory_info)");
            CM_Message("  VRAM Total: " + std::to_string(total_kb / 1024) + " MB");
            CM_Message("  VRAM Usada: " + std::to_string(used_kb / 1024) + " MB");
            CM_Message("  VRAM Disponível: " + std::to_string(avail_kb / 1024) + " MB");
            return;
        }
    }

    // AMD / Intel
    if (GLEW_ATI_meminfo) {
        GLint mem[4] = {0};
        glGetIntegerv(GL_TEXTURE_FREE_MEMORY_ATI, mem);

        CM_Message("Vendor: AMD/Intel (ATI_meminfo)");
        CM_Message("  VRAM Livre Aproximada: " + std::to_string(mem[0] / 1024) + " MB");
        return;
    }

    CM_Message("Nenhuma extensão para VRAM suportada pelo driver.");
}


void RAS_OpenGLRasterizer::PrintHardwareInfo()
{
    auto CM_PrintLine = [](const std::string &msg) { CM_Message(msg); };
    auto printSupport = [](const char* name, bool supported) {
        CM_Message(std::string("  ") + name + (supported ? " supported? yes." : " supported? no."));
    };

    // Info básica
    CM_PrintLine("=== GPU Info ===");
    CM_PrintLine("GL_VENDOR: " + std::string((const char*)glGetString(GL_VENDOR)));
    CM_PrintLine("GL_RENDERER: " + std::string((const char*)glGetString(GL_RENDERER)));
    CM_PrintLine("GL_VERSION: " + std::string((const char*)glGetString(GL_VERSION)));
    CM_PrintLine("GL_SHADING_LANGUAGE_VERSION: " + std::string((const char*)glGetString(GL_SHADING_LANGUAGE_VERSION)));

    // Extensões
    CM_PrintLine("GL_EXTENSIONS:");
    const char* extensions = (const char*)glGetString(GL_EXTENSIONS);
    std::stringstream ss(extensions);
    std::string ext;
    while (ss >> ext) {
        CM_PrintLine("  " + ext);
    }

    // Suporte a extensões principais
    CM_PrintLine("=== Supported Extensions ===");
    printSupport("GL_ARB_shader_objects", GLEW_ARB_shader_objects);
    printSupport("GL_ARB_geometry_shader4", GLEW_ARB_geometry_shader4);
    printSupport("GL_ARB_vertex_shader", GLEW_ARB_vertex_shader);
    printSupport("GL_ARB_fragment_shader", GLEW_ARB_fragment_shader);
    printSupport("GL_ARB_texture_cube_map", GLEW_ARB_texture_cube_map);
    printSupport("GL_ARB_multitexture", GLEW_ARB_multitexture);
    printSupport("GL_ARB_texture_env_combine", GLEW_ARB_texture_env_combine);
    printSupport("GL_ARB_texture_non_power_of_two", GPU_full_non_power_of_two_support());
    printSupport("GL_ARB_draw_instanced", GLEW_ARB_draw_instanced);

    // Detalhes do Vertex Shader
    if (GLEW_ARB_vertex_shader) {
        CM_PrintLine("=== Vertex Shader Details ===");
        GLint max = 0;
        glGetIntegerv(GL_MAX_VERTEX_UNIFORM_COMPONENTS_ARB, &max);
        CM_PrintLine("  Max uniform components: " + std::to_string(max));
        glGetIntegerv(GL_MAX_VARYING_FLOATS_ARB, &max);
        CM_PrintLine("  Max varying floats: " + std::to_string(max));
        glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS_ARB, &max);
        CM_PrintLine("  Max vertex texture units: " + std::to_string(max));
        glGetIntegerv(GL_MAX_VERTEX_ATTRIBS_ARB, &max);
        CM_PrintLine("  Max vertex attribs: " + std::to_string(max));
        glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS_ARB, &max);
        CM_PrintLine("  Max combined texture units: " + std::to_string(max));
        CM_PrintLine("");
    }

    // Detalhes do Fragment Shader
    if (GLEW_ARB_fragment_shader) {
        CM_PrintLine("=== Fragment Shader Details ===");
        GLint max = 0;
        glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_COMPONENTS_ARB, &max);
        CM_PrintLine("  Max uniform components: " + std::to_string(max));
        CM_PrintLine("");
    }

    // Cube map
    if (GLEW_ARB_texture_cube_map) {
        CM_PrintLine("=== Cube Map Details ===");
        GLint size = 0;
        glGetIntegerv(GL_MAX_CUBE_MAP_TEXTURE_SIZE_ARB, &size);
        CM_PrintLine("  Max cubemap size: " + std::to_string(size));
        CM_PrintLine("");
    }

    // Multitexture
    if (GLEW_ARB_multitexture) {
        CM_PrintLine("=== Multitexture Details ===");
        GLint units = 0;
        glGetIntegerv(GL_MAX_TEXTURE_UNITS_ARB, &units);
        CM_PrintLine("  Max texture units available: " + std::to_string(units));
        CM_PrintLine("");
    }

    // Limites gerais de textura e buffers
    GLint maxTexSize = 0, max3DtexSize = 0, maxArrayTexSize = 0, maxDrawBuffers = 0, maxViewportDims[2] = {0};
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexSize);
    glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &max3DtexSize);
    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &maxArrayTexSize);
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxDrawBuffers);
    glGetIntegerv(GL_MAX_VIEWPORT_DIMS, maxViewportDims);

    CM_PrintLine("=== Texture & Viewport Limits ===");
    CM_PrintLine("  Max 2D texture size: " + std::to_string(maxTexSize));
    CM_PrintLine("  Max 3D texture size: " + std::to_string(max3DtexSize));
    CM_PrintLine("  Max array texture layers: " + std::to_string(maxArrayTexSize));
    CM_PrintLine("  Max draw buffers: " + std::to_string(maxDrawBuffers));
    CM_PrintLine("  Max viewport dims: " + std::to_string(maxViewportDims[0]) + " x " + std::to_string(maxViewportDims[1]));

    // GPU memory info
    if (GLEW_NVX_gpu_memory_info) {
        GLint total_mem_kb = 0;
        glGetIntegerv(GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX, &total_mem_kb);
        CM_PrintLine("  GPU total memory: " + std::to_string(total_mem_kb / 1024) + " MB");
    }

    // MSAA
    GLint samples = 0;
    glGetIntegerv(GL_SAMPLES, &samples);
    CM_PrintLine("  Max MSAA samples: " + std::to_string(samples));

    // Uniform blocks & texture units
    GLint maxTextureImageUnits = 0, maxVertexUniformBlocks = 0, maxFragmentUniformBlocks = 0;
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &maxTextureImageUnits);
    glGetIntegerv(GL_MAX_VERTEX_UNIFORM_BLOCKS, &maxVertexUniformBlocks);
    glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_BLOCKS, &maxFragmentUniformBlocks);

    CM_PrintLine("  Max texture image units (fragment): " + std::to_string(maxTextureImageUnits));
    CM_PrintLine("  Max vertex uniform blocks: " + std::to_string(maxVertexUniformBlocks));
    CM_PrintLine("  Max fragment uniform blocks: " + std::to_string(maxFragmentUniformBlocks));
}
