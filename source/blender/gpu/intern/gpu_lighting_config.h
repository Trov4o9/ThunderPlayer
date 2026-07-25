/**
 * gpu_lighting_config.h
 * 
 * Global configuration for GPU lighting system
 * Controls whether to use UBO-based lighting or legacy lamp functions
 */

#ifndef __GPU_LIGHTING_CONFIG_H__
#define __GPU_LIGHTING_CONFIG_H__

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * UBO-BASED LIGHTING SYSTEM FLAG
 * ============================================================================
 * Set to 1 to enable the new UBO lighting system (removes old lamp functions)
 * Set to 0 to use the old lighting system (default/legacy behavior)
 * 
 * When enabled (1):
 * - Old lamp uniforms are NOT generated
 * - Old lamp GLSL functions are NOT injected
 * - New UBO shader (gpu_shader_ubo_lighting.glsl) is used instead
 * 
 * When disabled (0):
 * - Old lamp system works normally
 * - Compatible with all existing materials
 * - UBO is still updated in background (for future use)
 */
#define USE_UBO_LIGHTING_SYSTEM 0

#ifdef __cplusplus
}
#endif

#endif /* __GPU_LIGHTING_CONFIG_H__ */
