/*
 * RAS_LightManager.h - Global Light UBO Manager
 * Manages a shared Uniform Buffer Object for scene lighting
 * that can be used by all shaders in the game engine.
 */

#ifndef __RAS_LIGHTMANAGER_H__
#define __RAS_LIGHTMANAGER_H__

#include "GPU_material.h"

class KX_Scene;
class KX_LightObject;

class RAS_LightManager {
private:
	static RAS_LightManager *s_instance;
	
	unsigned int m_lightUBO;              // Handle do UBO OpenGL
	GPUSceneLightBlock m_lightBlockCPU;   // Cache CPU-side
	bool m_needsUpdate;
	int m_lastFrameUpdated;
	bool m_initialized;
	
public:
	static RAS_LightManager *GetInstance();
	static void Initialize();
	static void Cleanup();
	
	RAS_LightManager();
	~RAS_LightManager();
	
	void UpdateLights(KX_Scene *scene, int currentFrame);
	void BindUBO();  // Bind ao binding point 1
	void MarkDirty();
	
	unsigned int GetUBOHandle() const { return m_lightUBO; }
	const GPUSceneLightBlock &GetLightData() const { return m_lightBlockCPU; }
};

#endif  // __RAS_LIGHTMANAGER_H__
