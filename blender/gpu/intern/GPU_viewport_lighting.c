/**
 * GPU_viewport_lighting.c
 * 
 * Viewport/Editor lighting UBO system implementation
 * Shares same UBO structure with RAS_LightManager (binding point 1)
 */

#include "GPU_viewport_lighting.h"
#include "GPU_material.h"

#include "DNA_lamp_types.h"
#include "DNA_scene_types.h"
#include "DNA_object_types.h"

#include "BLI_math.h"
#include "BLI_utildefines.h"

#include "MEM_guardedalloc.h"

#include "GPU_extensions.h"
#include "GPU_glew.h"

#include <string.h>
#include <stdio.h>

/* UBO data structure - must match GPU_material.h and RAS_LightManager */
typedef struct ViewportLightUBO {
	GLuint ubo_id;
	GPUSceneLightBlock data;
	bool is_dirty;
	int last_frame;
} ViewportLightUBO;

static ViewportLightUBO *g_viewport_light_ubo = NULL;

/* Initialize the viewport lighting UBO system */
void GPU_viewport_lighting_init(void)
{
	if (g_viewport_light_ubo != NULL) {
		return;  /* Already initialized */
	}
	
	g_viewport_light_ubo = MEM_callocN(sizeof(ViewportLightUBO), "ViewportLightUBO");
	
	/* Create UBO */
	glGenBuffers(1, &g_viewport_light_ubo->ubo_id);
	glBindBuffer(GL_UNIFORM_BUFFER, g_viewport_light_ubo->ubo_id);
	glBufferData(GL_UNIFORM_BUFFER, sizeof(GPUSceneLightBlock), NULL, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
	
	/* Initialize data */
	memset(&g_viewport_light_ubo->data, 0, sizeof(GPUSceneLightBlock));
	g_viewport_light_ubo->is_dirty = true;
	g_viewport_light_ubo->last_frame = -1;
	
	printf("[GPU_viewport_lighting] Initialized UBO (ID: %u, binding: 1)\n", g_viewport_light_ubo->ubo_id);
}

/* Shutdown and cleanup */
void GPU_viewport_lighting_exit(void)
{
	if (g_viewport_light_ubo == NULL) {
		return;
	}
	
	if (g_viewport_light_ubo->ubo_id != 0) {
		glDeleteBuffers(1, &g_viewport_light_ubo->ubo_id);
	}
	
	MEM_freeN(g_viewport_light_ubo);
	g_viewport_light_ubo = NULL;
	
	printf("[GPU_viewport_lighting] Shutdown complete\n");
}

/* Update viewport lights from scene */
void GPU_viewport_lighting_update(struct Scene *scene, struct SceneRenderLayer *srl)
{
	if (g_viewport_light_ubo == NULL) {
		GPU_viewport_lighting_init();
	}
	
	/* Clear light data */
	memset(&g_viewport_light_ubo->data, 0, sizeof(GPUSceneLightBlock));
	
	int light_count = 0;
	Base *base;
	
	/* Iterate through scene objects */
	for (base = scene->base.first; base && light_count < MAX_SCENE_LIGHTS; base = base->next) {
		if (!(base->flag & SELECT) && !(base->lay & scene->lay)) {
			continue;  /* Skip invisible objects */
		}
		
		Object *ob = base->object;
		if (ob->type != OBJ_LAMP) {
			continue;
		}
		
		Lamp *la = (Lamp *)ob->data;
		GPUSceneLightData *light = &g_viewport_light_ubo->data.lights[light_count];
		
		/* Light type */
		if (la->type == LA_SPOT) {
			light->type_mode[0] = 0.0f;  /* SPOT */
		}
		else if (la->type == LA_SUN) {
			light->type_mode[0] = 1.0f;  /* SUN */
		}
		else if (la->type == LA_LOCAL) {
			light->type_mode[0] = 2.0f;  /* POINT */
		}
		else {
			continue;  /* Unsupported light type */
		}
		
		/* Color and energy */
		light->color_energy[0] = la->r * la->energy;
		light->color_energy[1] = la->g * la->energy;
		light->color_energy[2] = la->b * la->energy;
		light->color_energy[3] = 1.0f;
		
		/* Position (world space) */
		light->position[0] = ob->obmat[3][0];
		light->position[1] = ob->obmat[3][1];
		light->position[2] = ob->obmat[3][2];
		light->position[3] = 1.0f;
		
		/* Direction for spot/sun lights */
		if (la->type == LA_SPOT || la->type == LA_SUN) {
			float dir[3];
			dir[0] = -ob->obmat[2][0];
			dir[1] = -ob->obmat[2][1];
			dir[2] = -ob->obmat[2][2];
			normalize_v3(dir);
			
			light->spotDirection[0] = dir[0];
			light->spotDirection[1] = dir[1];
			light->spotDirection[2] = dir[2];
			light->spotDirection[3] = la->spotsize;  /* Spot size in radians */
		}
		
		/* Attenuation parameters */
		light->attenuation[0] = la->dist;
		light->attenuation[1] = la->att1;  /* Linear attenuation */
		light->attenuation[2] = la->att2;  /* Quadratic attenuation */
		light->attenuation[3] = la->spotblend;  /* Spot blend */
		
		light_count++;
	}
	
	/* Store light count */
	g_viewport_light_ubo->data.sceneLightInfo[0] = (float)light_count;
	g_viewport_light_ubo->is_dirty = true;
	
	printf("[GPU_viewport_lighting] Updated %d lights from viewport scene\n", light_count);
}

/* Bind UBO to shader (binding point 1) */
void GPU_viewport_lighting_bind(void)
{
	if (g_viewport_light_ubo == NULL) {
		GPU_viewport_lighting_init();
	}
	
	/* Upload data if dirty */
	if (g_viewport_light_ubo->is_dirty) {
		glBindBuffer(GL_UNIFORM_BUFFER, g_viewport_light_ubo->ubo_id);
		glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(GPUSceneLightBlock), &g_viewport_light_ubo->data);
		glBindBuffer(GL_UNIFORM_BUFFER, 0);
		g_viewport_light_ubo->is_dirty = false;
	}
	
	/* Bind to binding point 1 (same as runtime) */
	glBindBufferBase(GL_UNIFORM_BUFFER, 1, g_viewport_light_ubo->ubo_id);
}

/* Unbind UBO */
void GPU_viewport_lighting_unbind(void)
{
	glBindBufferBase(GL_UNIFORM_BUFFER, 1, 0);
}

/* Mark lights as dirty (needs update) */
void GPU_viewport_lighting_mark_dirty(void)
{
	if (g_viewport_light_ubo != NULL) {
		g_viewport_light_ubo->is_dirty = true;
	}
}
