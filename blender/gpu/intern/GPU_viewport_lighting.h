/**
 * GPU_viewport_lighting.h
 * 
 * Viewport/Editor lighting UBO system
 * Mirrors the runtime RAS_LightManager but for viewport rendering
 */

#ifndef __GPU_VIEWPORT_LIGHTING_H__
#define __GPU_VIEWPORT_LIGHTING_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the viewport lighting UBO system */
void GPU_viewport_lighting_init(void);

/* Shutdown and cleanup */
void GPU_viewport_lighting_exit(void);

/* Update viewport lights from scene */
void GPU_viewport_lighting_update(struct Scene *scene, struct SceneRenderLayer *srl);

/* Bind UBO to shader (binding point 1) */
void GPU_viewport_lighting_bind(void);

/* Unbind UBO */
void GPU_viewport_lighting_unbind(void);

/* Mark lights as dirty (needs update) */
void GPU_viewport_lighting_mark_dirty(void);

#ifdef __cplusplus
}
#endif

#endif /* __GPU_VIEWPORT_LIGHTING_H__ */
