/*
 * RAS_LightManager.cpp - Global Light UBO Manager Implementation
 */

#include "RAS_LightManager.h"
#include "KX_Scene.h"
#include "KX_LightObject.h"
#include "RAS_ILightObject.h"
#include "DNA_scene_types.h"

#include "GPU_glew.h"
#include <cstring>
#include <cmath>

RAS_LightManager *RAS_LightManager::s_instance = nullptr;

RAS_LightManager *RAS_LightManager::GetInstance()
{
	if (!s_instance) {
		s_instance = new RAS_LightManager();
	}
	return s_instance;
}

void RAS_LightManager::Initialize()
{
	GetInstance();
}

void RAS_LightManager::Cleanup()
{
	if (s_instance) {
		delete s_instance;
		s_instance = nullptr;
	}
}

RAS_LightManager::RAS_LightManager()
	: m_lightUBO(0),
	  m_needsUpdate(true),
	  m_lastFrameUpdated(-1),
	  m_initialized(false)
{
	memset(&m_lightBlockCPU, 0, sizeof(GPUSceneLightBlock));
	
	// Criar UBO OpenGL
	glGenBuffers(1, &m_lightUBO);
	glBindBuffer(GL_UNIFORM_BUFFER, m_lightUBO);
	glBufferData(GL_UNIFORM_BUFFER, sizeof(GPUSceneLightBlock), nullptr, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
	
	m_initialized = true;
	
	printf("[RAS_LightManager] Initialized - UBO handle: %u, size: %zu bytes\n", 
	       m_lightUBO, sizeof(GPUSceneLightBlock));
}

RAS_LightManager::~RAS_LightManager()
{
	if (m_lightUBO) {
		glDeleteBuffers(1, &m_lightUBO);
		m_lightUBO = 0;
	}
	printf("[RAS_LightManager] Cleanup complete\n");
}

void RAS_LightManager::UpdateLights(KX_Scene *scene, int currentFrame)
{
	if (!scene) return;
	
	// Se já atualizou neste frame e não está dirty, pula
	if (!m_needsUpdate && m_lastFrameUpdated == currentFrame)
		return;
	
	// Obter lista de luzes da cena
	EXP_ListValue<KX_LightObject> *lights = scene->GetLightList();
	if (!lights) {
		m_lightBlockCPU.sceneLightCount = 0.0f;
		glBindBuffer(GL_UNIFORM_BUFFER, m_lightUBO);
		glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(GPUSceneLightBlock), &m_lightBlockCPU);
		glBindBuffer(GL_UNIFORM_BUFFER, 0);
		return;
	}
	
	// Obter layers ativas
	int activeLayers = ~0;
	if (Scene *blenderScene = scene->GetBlenderScene()) {
		activeLayers = blenderScene->lay;
	}
	
	int uploadCount = 0;
	const int listCount = lights->GetCount();
	
	for (int i = 0; i < listCount && uploadCount < MAX_SCENE_LIGHTS; ++i) {
		KX_LightObject *kxL = lights->GetValue(i);
		if (!kxL) continue;
		
		// Filtrar por layer
		if ((kxL->GetLayer() & activeLayers) == 0) continue;
		
		RAS_ILightObject *ld = kxL->GetLightData();
		if (!ld) continue;
		
		// Obter dados da luz
		const mt::vec3 vpos = kxL->NodeGetWorldPosition();
		const mt::vec3 ldir = -kxL->NodeGetWorldOrientation().GetColumn(2);
		const mt::vec3 spotDir = ldir.Normalized();
		
		float energy = ld->m_energy;
		mt::vec3 color = ld->m_color;
		
		const float dist = ld->m_distance;
		const int u = uploadCount;
		
		GPUSceneLightData &light = m_lightBlockCPU.lights[u];
		
		// Tipo: 0=spot, 1=sun, 2=point
		light.type = (float)ld->m_type;
		light.pad[0] = light.pad[1] = light.pad[2] = 0.0f;
		
		// Cor difusa * energia
		light.diffuse[0] = color.x * energy;
		light.diffuse[1] = color.y * energy;
		light.diffuse[2] = color.z * energy;
		light.diffuse[3] = 0.0f;
		
		// Posição
		light.position[0] = vpos.x;
		light.position[1] = vpos.y;
		light.position[2] = vpos.z;
		light.position[3] = 0.0f;
		
		// Direção do spotlight
		light.spotDir[0] = spotDir.x;
		light.spotDir[1] = spotDir.y;
		light.spotDir[2] = spotDir.z;
		light.spotDir[3] = 0.0f;
		
		// Parâmetros
		light.params[0] = dist > 0.0f ? 1.0f / dist : 0.0f;  // 1/distance para atenuação
		light.params[1] = std::cos(ld->m_spotsize * 0.5f);   // cos(spotsize/2)
		light.params[2] = ld->m_spotblend / 5.0f;            // blend factor
		light.params[3] = dist > 0.0f ? dist * dist : 0.0f;  // distance²
		
		++uploadCount;
	}
	
	m_lightBlockCPU.sceneLightCount = (float)uploadCount;
	
	// Upload para GPU
	glBindBuffer(GL_UNIFORM_BUFFER, m_lightUBO);
	glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(GPUSceneLightBlock), &m_lightBlockCPU);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
	
	m_needsUpdate = false;
	m_lastFrameUpdated = currentFrame;
	
	// Debug output (apenas quando muda)
	static int lastCount = -1;
	if (uploadCount != lastCount) {
		printf("[RAS_LightManager] Updated %d lights for frame %d\n", uploadCount, currentFrame);
		lastCount = uploadCount;
	}
}

void RAS_LightManager::BindUBO()
{
	if (m_lightUBO) {
		glBindBufferBase(GL_UNIFORM_BUFFER, 1, m_lightUBO);
	}
}

void RAS_LightManager::MarkDirty()
{
	m_needsUpdate = true;
}
