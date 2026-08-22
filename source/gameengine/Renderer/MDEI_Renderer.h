/*
 * MDEI_Renderer.h — Entry point for the MDEI fast-path rendering system.
 *
 * Responsibilities:
 *  - Store all MDEI_DrawGroups (one per unique shader — pooled, or one per
 *    (mesh, shader) pair for skinned/private objects)
 *  - Maintain its own list of registered KX_GameObjects (m_registeredObjects)
 *  - Insert each registered object into the Bullet DBVT culling tree
 *    (m_dbvtControllers) so frustum culling runs alongside RAS objects.
 *  - Drive RenderSolid() and RenderShadow() using the DBVT-culled list
 *    provided by KX_Scene (no fallback — if DBVT is off, nothing is drawn).
 *  - Drive MDEI_SkinDeformer updates for objects with armature parents.
 *
 * Design contract:
 *   MDEI objects are NEVER registered in any RAS class.  Their DBVT presence
 *   is managed exclusively here, via CcdGraphicController objects owned by
 *   this renderer.  The separation of RAS vs MDEI results happens in
 *   CcdPhysicsEnvironment::CullingTestKX (HasFastRenderFlag test).
 */

#ifndef __MDEI_RENDERER_H__
#define __MDEI_RENDERER_H__

#include "MDEI_PersistentBuffer.h"
#include "MDEI_GeometryPool.h"   /* MDEI_MeshSlot */

#include <vector>
#include <unordered_map>
#include <map>
#include <utility>

class MDEI_Mesh;
class MDEI_Shader;
class MDEI_DrawGroup;
class MDEI_ObjectProxy;
class MDEI_SkinDeformer;
class CcdGraphicController;
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

	/** Release the GPU geometry (VAO/VBO/EBO) for the MDEI_Mesh owned by
	 *  this object's proxy, then allocate a fresh empty MDEI_Mesh in its
	 *  place so the next mdei_update_mesh() can re-upload new geometry.
	 *  The object remains registered (still in m_registeredObjects and
	 *  still has a valid proxy + shader); only the vertex/index data is
	 *  discarded.  Safe to call from Python on the main thread at any time. */
	void ResetMesh(KX_GameObject *gameobj);

	/** If this object's proxy->mesh still points to a shared (cached) mesh,
	 *  create a private MDEI_Mesh + dedicated MDEI_DrawGroup for it so that
	 *  subsequent Upload() calls (mdei_update_mesh) do not corrupt other
	 *  objects sharing the same Blender mesh.  No-op if already private. */
	void EnsurePrivateMesh(KX_GameObject *gameobj);

	/** Update the skin deformer for a specific object (called from within
	 *  update_anim_thread_func while armature obmat is still current). */
	void UpdateDeformerForObject(KX_GameObject *gameobj);

	/** Update ALL active skin deformers (fallback, not used in main path). */
	void UpdateDeformers();

	/** Solid pass — draws only objects in `visibleObjects` (from DBVT culling).
	 *  If visibleObjects is empty, nothing is drawn (no fallback). */
	void RenderSolid(const std::vector<KX_GameObject *>& visibleObjects,
	                 RAS_Rasterizer *rasty);

	/** Shadow pass — draws only objects in `visibleObjects` (from DBVT culling).
	 *  If visibleObjects is empty, nothing is drawn (no fallback). */
	void RenderShadow(const std::vector<KX_GameObject *>& visibleObjects,
	                  RAS_Rasterizer *rasty);

	/** Retorna true se há pelo menos um objeto registrado neste renderer.
	 *  Usado pelos call sites em KX_Scene para evitar o custo de ExecuteDraw
	 *  (reset de groups, loop de instâncias, uploads GL) em cenas sem MDEI. */
	bool HasObjects() const { return !m_registeredObjects.empty(); }

	/** Retorna a lista de objetos registrados (para debug e UpdateDbvt). */
	const std::vector<KX_GameObject *>& GetRegisteredObjects() const
	    { return m_registeredObjects; }

	/** Retorna o mapa de controladores DBVT (usado por KX_Scene::UpdateDbvt). */
	const std::unordered_map<KX_GameObject *, CcdGraphicController *>& GetDbvtControllers() const
	    { return m_dbvtControllers; }

	/** Aplica o novo nível de filtragem anisotrópica em todos os sampler
	 *  arrays MDEI já criados, recriando-os com o novo parâmetro.
	 *  Espelha o que GPU_set_anisotropic + GPU_free_images faz para o RAS:
	 *  as texturas Blender são liberadas e recriadas com o novo filtro;
	 *  aqui os GL_TEXTURE_2D_ARRAY MDEI são recriados com o mesmo nível. */
	void SetAnisotropicFiltering(short level);

	/** Recria todos os sampler arrays MDEI com o novo estado de mipmap.
	 *  enabled: liga/desliga mipmapping.
	 *  glFilterType: constante GL_* para GL_TEXTURE_MIN_FILTER (0 = usar
	 *  GPU_get_mipmap_filter() global, como faz o RAS). */
	void SetMipmapping(bool enabled, int glFilterType);

private:
	/** Returns (or creates) the single pooled DrawGroup for a shader. */
	MDEI_DrawGroup *GetOrCreatePooledGroup(MDEI_Shader *shader);

	/** Returns (or creates) a private (non-pooled) DrawGroup for (mesh, shader).
	 *  Used for skinned objects and EnsurePrivateMesh. */
	MDEI_DrawGroup *GetOrCreatePrivateGroup(MDEI_Mesh *mesh, MDEI_Shader *shader);

	/** Legacy alias — redirects to GetOrCreatePrivateGroup. */
	MDEI_DrawGroup *GetOrCreateGroup(MDEI_Mesh *mesh, MDEI_Shader *shader);

	/** Core draw: iterates visibleObjects (result of DBVT culling — no fallback). */
	void ExecuteDraw(bool shadowPass, RAS_Rasterizer *rasty,
	                 const std::vector<KX_GameObject *>& visibleObjects);

	KX_Scene              *m_scene;
	MDEI_PersistentBuffer  m_buffer;

	/** Separate ring-buffer indices for solid and shadow passes.
	 *  They run concurrently each game frame so must not share slots. */
	int m_solidFrame;
	int m_shadowFrame;

	/** Total number of ExecuteDraw calls — used only for debug output. */
	int m_debugCallCount;

	/** Conta total de vezes que EnsurePrivateMesh criou um mesh privado. */
	int m_ensurePrivateCount;

	/** All objects that were registered with this renderer.
	 *  Used for HasObjects(), lifecycle management, and deformer updates.
	 *  At draw time, only objects in the visibleObjects list (from DBVT culling)
	 *  are rendered — not this list directly. */
	std::vector<KX_GameObject *> m_registeredObjects;

	/** One CcdGraphicController per registered MDEI object, inserted in the
	 *  physics environment's DBVT culling tree.  Owned by this renderer. */
	std::unordered_map<KX_GameObject *, CcdGraphicController *> m_dbvtControllers;

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
		bool          cullFace; /* true = GL_CULL_FACE enabled para este material */
	};
	std::unordered_map<void *, ShaderEntry> m_shaderCache;

	std::vector<MDEI_DrawGroup *> m_groups;

	/** Maps (pooledGroup*, MDEI_Mesh*) → slot index in that group's pool.
	 *  Used to detect when a mesh is already uploaded and reuse its slot. */
	std::map<std::pair<MDEI_DrawGroup *, MDEI_Mesh *>, MDEI_MeshSlot> m_meshToSlot;

	/** One skin deformer per MDEI object that has an armature parent.
	 *  Key = KX_GameObject*. Owned by this renderer. */
	std::unordered_map<KX_GameObject *, MDEI_SkinDeformer *> m_skinDeformers;
};

#endif /* __MDEI_RENDERER_H__ */
