/*
 * MDEI_ObjectProxy.h — Lightweight per-object link between KX_GameObject
 * and the MDEI draw group. Stored in KX_GameObject::m_mdeiProxy.
 * No dependency on RAS_* whatsoever.
 */

#ifndef __MDEI_OBJECT_PROXY_H__
#define __MDEI_OBJECT_PROXY_H__

#include "MDEI_GeometryPool.h"  /* MDEI_MeshSlot, MDEI_SLOT_INVALID */

class MDEI_Mesh;
class MDEI_DrawGroup;

/** Replaces RAS_MeshUser for objects with OB_FAST_RENDER.
 *  Owned by MDEI_Renderer — never by KX_GameObject (pointer only). */
struct MDEI_ObjectProxy {
	/** MDEI_Mesh pointer:
	 *  - Static (pooled) objects: pointer to the source MDEI_Mesh still in
	 *    m_meshCache, used for AABB and for looking up the pool slot.
	 *  - Skinned / EnsurePrivate objects: the per-object MDEI_Mesh. */
	MDEI_Mesh      *mesh;

	/** DrawGroup this object belongs to.
	 *  Pooled objects: the shared-per-shader DrawGroup.
	 *  Skinned / EnsurePrivate: the dedicated (mesh, shader) DrawGroup. */
	MDEI_DrawGroup *group;

	/** Slot in group->GetPool() for static/pooled objects.
	 *  MDEI_SLOT_INVALID for skinned/private objects. */
	MDEI_MeshSlot   meshSlot;

	/** Slot index inside the group's command list (legacy, kept for compat). */
	int             index;

	/** Full constructor for pooled objects. */
	MDEI_ObjectProxy(MDEI_Mesh *m, MDEI_DrawGroup *g, MDEI_MeshSlot slot)
	    : mesh(m), group(g), meshSlot(slot), index(-1) {}

	/** Legacy constructor for skinned / EnsurePrivate objects (no slot). */
	MDEI_ObjectProxy(MDEI_Mesh *m, MDEI_DrawGroup *g)
	    : mesh(m), group(g), meshSlot(MDEI_SLOT_INVALID), index(-1) {}
};

#endif /* __MDEI_OBJECT_PROXY_H__ */
