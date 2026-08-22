/*
 * MDEI_DrawGroup.h
 *
 * One draw group per unique MDEI_Shader.
 *
 * All objects that share the same shader (regardless of mesh) belong to the
 * same DrawGroup.  The group owns a MDEI_GeometryPool that holds all their
 * vertex/index data in a single VBO+EBO.  This allows a TRUE single
 * glMultiDrawElementsIndirect call per shader, with each command pointing to
 * a different slice of the shared VBO/EBO via firstIndex + baseVertex.
 *
 * Skinned objects are NOT pooled; they keep individual MDEI_Mesh objects and
 * belong to a separate DrawGroup whose m_pool is nullptr.
 */

#ifndef __MDEI_DRAW_GROUP_H__
#define __MDEI_DRAW_GROUP_H__

#include "MDEI_PersistentBuffer.h"
#include "MDEI_GeometryPool.h"   /* MDEI_MeshSlot */

#include <vector>

class MDEI_Mesh;
class MDEI_Shader;

class MDEI_DrawGroup {
public:
	/** Static group: uses shared geometry pool (all non-skinned meshes). */
	explicit MDEI_DrawGroup(MDEI_Shader *shader);

	/** Skinned/private group: owns a single specific mesh, no pool.
	 *  Compatible with the old (mesh, shader) constructor so that
	 *  EnsurePrivateMesh / RegisterArmature compile unchanged. */
	MDEI_DrawGroup(MDEI_Mesh *mesh, MDEI_Shader *shader);

	~MDEI_DrawGroup();

	/* ── Accessors ──────────────────────────────────────────────────────── */

	MDEI_Shader       *GetShader() const { return m_shader; }

	/** Returns the private mesh for skinned/private groups, nullptr for pooled. */
	MDEI_Mesh         *GetMesh()   const { return m_privateMesh; }

	/** Returns the shared pool (nullptr for skinned/private groups). */
	MDEI_GeometryPool *GetPool()   const { return m_pool; }

	bool IsPooled()   const { return m_pool != nullptr; }

	/** Swap the private mesh pointer (used by ResetMesh / EnsurePrivateMesh). */
	void SetMesh(MDEI_Mesh *mesh) { m_privateMesh = mesh; }

	/* ── Frame lifecycle ────────────────────────────────────────────────── */

	/** Clear pending instance list. */
	void BeginFrame() { m_pendingInstances.clear(); }

	/** Add a visible instance.
	 *  @param slot  Geometry pool slot for this instance (MDEI_SLOT_INVALID for
	 *               skinned/private groups where the mesh is known at group level). */
	void AddInstance(const float matrix[16], const float color[4],
	                 MDEI_MeshSlot slot = MDEI_SLOT_INVALID);

	/** Write all pending instances into the ring buffer slice starting at
	 *  \p dest, starting at absolute instance offset \p baseInst.
	 *  Appends one DrawElementsIndirectCommand per instance into \p cmds.
	 *  For pooled groups the pool must have been uploaded before this call. */
	void WriteInstancesAndCommand(
		MDEI_Instance *dest,
		unsigned int   baseInst,
		std::vector<DrawElementsIndirectCommand>& cmds);

	/** Number of instances accumulated this frame. */
	unsigned int PendingCount() const { return (unsigned int)m_pendingInstances.size(); }

	/** Called once after all objects have been registered so the pool uploads
	 *  its geometry to the GPU. No-op for skinned/private groups. */
	void UploadPool() { if (m_pool) m_pool->UploadIfDirty(); }

	/** GLuint of the VAO to bind before drawing.
	 *  Pooled → pool's shared VAO.
	 *  Private → m_privateMesh's current VAO (handles skinned ring). */
	GLuint GetCurrentVAO() const;

	/** Index count / firstIndex / baseVertex for a draw call that covers
	 *  the private mesh (only valid for non-pooled groups). */
	GLuint  GetPrivateIndexCount() const;
	GLuint  GetPrivateFirstIndex() const { return 0; }
	GLint   GetPrivateBaseVertex() const { return 0; }

private:
	MDEI_Shader       *m_shader;
	MDEI_GeometryPool *m_pool;        /* non-null for pooled (static) groups */
	MDEI_Mesh         *m_privateMesh; /* non-null for skinned/private groups  */

	struct RawInstance {
		float         matrix[16];
		float         color[4];
		MDEI_MeshSlot slot;   /* pool slot (-1 for private groups) */
	};
	std::vector<RawInstance> m_pendingInstances;
};

#endif /* __MDEI_DRAW_GROUP_H__ */
