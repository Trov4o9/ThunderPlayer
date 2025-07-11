/*
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
 * The Original Code is Copyright (C) 2005 Blender Foundation.
 * All rights reserved.
 */

/** \file GPU_shader.h
 *  \ingroup gpu
 */

#ifndef __GPU_SHADER_H__
#define __GPU_SHADER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>  // Para usar o tipo bool em C

typedef struct GPUShader GPUShader;
struct GPUTexture;

/* GPU Shader flags */
enum {
	GPU_SHADER_FLAGS_NONE = 0,
	GPU_SHADER_FLAGS_SPECIAL_OPENSUBDIV = (1 << 0),
	GPU_SHADER_FLAGS_NEW_SHADING        = (1 << 1),
	GPU_SHADER_FLAGS_SPECIAL_INSTANCING = (1 << 2),
};

/* Shader creation */
GPUShader *GPU_shader_create(
        const char *vertexcode,
        const char *fragcode,
        const char *geocode,
        const char *libcode,
        const char *defines,
        int input, int output, int number);

GPUShader *GPU_shader_create_ex(
        const char *vertexcode,
        const char *fragcode,
        const char *geocode,
        const char *libcode,
        const char *defines,
        int input, int output, int number,
        const int flags);

/* Compute Shader creation */
GPUShader *GPU_shader_create_compute(
        const char *computecode,
        const char *libcode,
        const char *defines,
        int number);

GPUShader *GPU_shader_create_compute_ex(
        const char *computecode,
        const char *libcode,
        const char *defines,
        int number,
        const int flags);

char *GPU_shader_validate(GPUShader *shader);
void GPU_shader_free(GPUShader *shader);

void GPU_shader_bind(GPUShader *shader);
void GPU_shader_unbind(void);

int GPU_shader_program(GPUShader *shader);

/* Uniforms */
typedef struct GPUUniformInfo {
	unsigned int size;
	unsigned int type;
	char name[255];
} GPUUniformInfo;

int GPU_shader_get_uniform_infos(GPUShader *shader, GPUUniformInfo **infos);
void *GPU_shader_get_interface(GPUShader *shader);
void GPU_shader_set_interface(GPUShader *shader, void *interface);
int GPU_shader_get_uniform(GPUShader *shader, const char *name);

void GPU_shader_uniform_vector(GPUShader *shader, int location, int length,
	int arraysize, const float *value);
void GPU_shader_uniform_vector_int(GPUShader *shader, int location, int length,
	int arraysize, const int *value);

void GPU_shader_uniform_texture(GPUShader *shader, int location, struct GPUTexture *tex);
void GPU_shader_uniform_int(GPUShader *shader, int location, int value);
void GPU_shader_uniform_float(GPUShader *shader, int location, float value);

/* Geometry shader config */
void GPU_shader_geometry_stage_primitive_io(GPUShader *shader, int input, int output, int number);

/* Attributes */
int GPU_shader_get_attribute(GPUShader *shader, const char *name);
void GPU_shader_bind_attribute(GPUShader *shader, int location, const char *name);
void GPU_shader_bind_instancing_attrib(GPUShader *shader, void *matrixoffset, void *positionoffset);

/* Builtin/Non-generated shaders */
typedef enum GPUBuiltinShader {
	GPU_SHADER_VSM_STORE            = 0,
	GPU_SHADER_VSM_STORE_INSTANCING = 1,
	GPU_SHADER_SEP_GAUSSIAN_BLUR    = 2,
	GPU_SHADER_SMOKE                = 3,
	GPU_SHADER_SMOKE_FIRE           = 4,
	GPU_SHADER_SMOKE_COBA           = 5,
	GPU_SHADER_BLACK                = 6,
	GPU_SHADER_BLACK_INSTANCING     = 7,
	GPU_SHADER_DRAW_FRAME_BUFFER	= 8,
	GPU_SHADER_STEREO_STIPPLE       = 9,
	GPU_SHADER_STEREO_ANAGLYPH      = 10,
	GPU_SHADER_FRUSTUM_LINE         = 11,
	GPU_SHADER_FRUSTUM_SOLID        = 12,
	GPU_SHADER_FLAT_COLOR           = 13,
	GPU_SHADER_2D_BOX               = 14,
} GPUBuiltinShader;

GPUShader *GPU_shader_get_builtin_shader(GPUBuiltinShader shader);
GPUShader *GPU_shader_get_builtin_fx_shader(int effects, bool persp);

void GPU_shader_free_builtin_shaders(void);

/* Vertex attributes */
#define GPU_MAX_ATTRIB 32

typedef struct GPUVertexAttribs {
	struct {
		int type;
		int glindex;
		int glinfoindoex;
		int gltexco;
		int attribid;
		char name[64];	/* MAX_CUSTOMDATA_LAYER_NAME */
	} layer[GPU_MAX_ATTRIB];

	int totlayer;
} GPUVertexAttribs;

#ifdef __cplusplus
}
#endif

#endif  /* __GPU_SHADER_H__ */
