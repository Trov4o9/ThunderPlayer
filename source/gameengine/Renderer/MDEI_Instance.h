/*
 * MDEI_Instance.h — Per-instance data written into the persistent ring buffer (SSBO).
 * Layout must match the GLSL struct in mdei_shader_vert.glsl exactly.
 */

#ifndef __MDEI_INSTANCE_H__
#define __MDEI_INSTANCE_H__

#include "mathfu.h"

/** Per-instance data packed into the SSBO.
 *  Total: 80 bytes — mat4 (64) + vec4 color (16). */
struct MDEI_Instance {
	float matrix[16]; /* model matrix, column-major */
	float color[4];   /* object color (RGBA) */
};

static_assert(sizeof(MDEI_Instance) == 80, "MDEI_Instance must be 80 bytes");

#endif /* __MDEI_INSTANCE_H__ */
