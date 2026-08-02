/*
 * MDEI_Renderer.h — Entry point for the MDEI fast-path rendering system.
 *
 * Responsibilities:
 *  - Store all MDEI_DrawGroups (one per unique mesh+shader pair)
 *  - Maintain its own list of registered KX_GameObjects (m_registeredObjects)
 *  - Drive RenderSolid() and RenderShadow() using that list directly,
 *    bypassing the RAS culling pipeline entirely.
 *  - Drive MDEI_SkinDeformer updates for objects with armature parents.
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
class MDEI_SkinDeformer;
class KX_GameObject;
class KX_Scene;
class RAS_Rasterizer;
class BL_ArmatureObject;
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

	/** Register an armature-driven skin deformer for a previously registered
	 *  MDEI object.  Must be called AFTER RegisterObject().
	 *  @param blenderObjNew  Object used as deformation reference (provides obmat). */
	void RegisterArmature(KX_GameObject     *gameobj,
	                      Object            *blenderObjNew,
	                      BL_ArmatureObject *arma);

	/** Called when a replica is created (AddObject). */
	void RegisterReplica(KX_GameObject *replica, KX_GameObject *original);

	/** Called when an object is removed (EndObject / NewRemoveObject).
	 *  Removes from m_registeredObjects and frees the proxy + deformer. */
	void UnregisterObject(KX_GameObject *gameobj);

	/** Update the skin deformer for a specific object (called from within
	 *  update_anim_thread_func while armature obmat is still current). */
	void UpdateDeformerForObject(KX_GameObject *gameobj);

	/** Update ALL active skin deformers (fallback, not used in main path). */
	void UpdateDeformers();

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

	/* ── Mesh cache ───────────────────────────────────────────────────
	 * Static meshes: keyed by blenderMesh* (shared across instances).
	 * Skinned meshes: keyed by blenderObject* (one VBO per object because
	 * each object has its own pose).
	 * Both maps point to MDEI_Mesh objects owned by this renderer.
	 * ------------------------------------------------------------------ */
	std::unordered_map<void *, MDEI_Mesh *> m_meshCache;
	std::unordered_map<void *, MDEI_Mesh *> m_skinnedMeshCache; /* key = Object* */

	struct ShaderEntry {
		MDEI_Shader  *shader;
		GPUMaterial  *gpuMat;
	};
	std::unordered_map<void *, ShaderEntry> m_shaderCache;

	std::vector<MDEI_DrawGroup *> m_groups;

	/** One skin deformer per MDEI object that has an armature parent.
	 *  Key = KX_GameObject*. Owned by this renderer. */
	std::unordered_map<KX_GameObject *, MDEI_SkinDeformer *> m_skinDeformers;
};

#endif /* __MDEI_RENDERER_H__ */
