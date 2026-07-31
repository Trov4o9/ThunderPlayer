/*
 * MDEI_Renderer.h — Entry point for the MDEI fast-path rendering system.
 *
 * Responsibilities:
 *  - Store all MDEI_DrawGroups (one per unique mesh+shader pair)
 *  - Maintain its own list of registered KX_GameObjects (m_registeredObjects)
 *  - Drive RenderSolid() and RenderShadow() using that list directly,
 *    bypassing the RAS culling pipeline entirely.
 *
 * WHY a separate list:
 *   Objects with OB_FAST_RENDER have no RAS_MeshUser, no physics body visible
 *   to the DBVT, and no HasShadowCasterMaterial(). The engine's
 *   CalculateVisibleMeshes / shadow filter therefore never includes them.
 *   The MDEI_Renderer manages its own visibility independently.
 */

#ifndef __MDEI_RENDERER_H__
#define __MDEI_RENDERER_H__

#include "MDEI_PersistentBuffer.h"

#include <vector>
#include <unordered_map>

class MDEI_Mesh;
class MDEI_Shader;
class MDEI_DrawGroup;
class MDEI_ObjectProxy;
class KX_GameObject;
class KX_Scene;
class RAS_Rasterizer;
struct Material;
struct Scene;
struct Object;
struct GPUMaterial;

class MDEI_Renderer {
public:
	explicit MDEI_Renderer(KX_Scene *scene);
	~MDEI_Renderer();

	/** Called during scene conversion: build mesh + shader, create proxy,
	 *  and add gameobj to m_registeredObjects. */
	void RegisterObject(KX_GameObject *gameobj,
	                    Object *blenderObj,
	                    Material *blenderMat,
	                    Scene *blenderScene);

	/** Called when a replica is created (AddObject). */
	void RegisterReplica(KX_GameObject *replica, KX_GameObject *original);

	/** Called when an object is removed (EndObject / NewRemoveObject).
	 *  Removes from m_registeredObjects and frees the proxy. */
	void UnregisterObject(KX_GameObject *gameobj);

	/** Solid pass — ignores `objects`, uses m_registeredObjects instead. */
	void RenderSolid(const std::vector<KX_GameObject *>& objects,
	                 RAS_Rasterizer *rasty);

	/** Shadow pass — ignores `objects`, uses m_registeredObjects instead. */
	void RenderShadow(const std::vector<KX_GameObject *>& objects,
	                  RAS_Rasterizer *rasty);

private:
	MDEI_DrawGroup *GetOrCreateGroup(MDEI_Mesh *mesh, MDEI_Shader *shader);

	/** Core draw: iterates m_registeredObjects (not the RAS list). */
	void ExecuteDraw(bool shadowPass, RAS_Rasterizer *rasty);

	KX_Scene              *m_scene;
	MDEI_PersistentBuffer  m_buffer;

	/** Separate ring-buffer indices for solid and shadow passes.
	 *  They run concurrently each game frame so must not share slots. */
	int m_solidFrame;
	int m_shadowFrame;

	/** Total number of ExecuteDraw calls — used only for debug output. */
	int m_debugCallCount;

	/** All objects that were registered with this renderer.
	 *  This is the authoritative list used at draw time. */
	std::vector<KX_GameObject *> m_registeredObjects;

	std::unordered_map<void *, MDEI_Mesh *> m_meshCache;

	struct ShaderEntry {
		MDEI_Shader  *shader;
		GPUMaterial  *gpuMat;
	};
	std::unordered_map<void *, ShaderEntry> m_shaderCache;

	std::vector<MDEI_DrawGroup *> m_groups;
};

#endif /* __MDEI_RENDERER_H__ */
