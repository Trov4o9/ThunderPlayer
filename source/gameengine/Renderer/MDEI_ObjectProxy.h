/*
 * MDEI_ObjectProxy.h — Lightweight per-object link between KX_GameObject
 * and the MDEI draw group. Stored in KX_GameObject::m_mdeiProxy.
 * No dependency on RAS_* whatsoever.
 */

#ifndef __MDEI_OBJECT_PROXY_H__
#define __MDEI_OBJECT_PROXY_H__

class MDEI_Mesh;
class MDEI_DrawGroup;

/** Replaces RAS_MeshUser for objects with OB_FAST_RENDER.
 *  Owned by MDEI_Renderer — never by KX_GameObject (pointer only). */
struct MDEI_ObjectProxy {
	MDEI_Mesh      *mesh;    /* shared geometry (may be the same as the template) */
	MDEI_DrawGroup *group;   /* draw group this instance belongs to               */
	int             index;   /* slot index inside the group (-1 = not yet active) */

	MDEI_ObjectProxy(MDEI_Mesh *m, MDEI_DrawGroup *g)
		: mesh(m), group(g), index(-1) {}
};

#endif /* __MDEI_OBJECT_PROXY_H__ */
