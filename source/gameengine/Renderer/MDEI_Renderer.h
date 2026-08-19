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

	/** Solid pass — ignores `objects`, uses m_registeredObjects instead. */
	void RenderSolid(const std::vector<KX_GameObject *>& objects,
	                 RAS_Rasterizer *rasty);

	/** Shadow pass — ignores `objects`, uses m_registeredObjects instead. */
	void RenderShadow(const std::vector<KX_GameObject *>& objects,
	                  RAS_Rasterizer *rasty);

	/** Retorna true se há pelo menos um objeto registrado neste renderer.
	 *  Usado pelos call sites em KX_Scene para evitar o custo de ExecuteDraw
	 *  (reset de groups, loop de instâncias, uploads GL) em cenas sem MDEI. */
	bool HasObjects() const { return !m_registeredObjects.empty(); }

	/** Retorna a lista de objetos registrados (para debug). */
	const std::vector<KX_GameObject *>& GetRegisteredObjects() const
	    { return m_registeredObjects; }

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

	/** Conta total de vezes que EnsurePrivateMesh criou um mesh privado. */
	int m_ensurePrivateCount;

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
		bool          cullFace; /* true = GL_CULL_FACE enabled para este material */
	};
	std::unordered_map<void *, ShaderEntry> m_shaderCache;

	std::vector<MDEI_DrawGroup *> m_groups;

	/** One skin deformer per MDEI object that has an armature parent.
	 *  Key = KX_GameObject*. Owned by this renderer. */
	std::unordered_map<KX_GameObject *, MDEI_SkinDeformer *> m_skinDeformers;
};

#endif /* __MDEI_RENDERER_H__ */
