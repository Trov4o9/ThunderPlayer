/*
 * MDEI_DrawGroup.h — One draw group per unique MDEI_Mesh + MDEI_Shader pair.
 * Accumulates visible instances per frame and generates draw commands.
 */

#ifndef __MDEI_DRAW_GROUP_H__
#define __MDEI_DRAW_GROUP_H__

#include "MDEI_PersistentBuffer.h"

#include <vector>

class MDEI_Mesh;
class MDEI_Shader;
class KX_GameObject;

class MDEI_DrawGroup {
public:
	MDEI_DrawGroup(MDEI_Mesh *mesh, MDEI_Shader *shader);

	MDEI_Mesh   *GetMesh()   const { return m_mesh; }
	MDEI_Shader *GetShader() const { return m_shader; }

	/** Called once per frame before rendering begins: clear instance list. */
	void BeginFrame() { m_pendingInstances.clear(); }

	/** Add a visible instance with its world matrix + color. */
	void AddInstance(const float matrix[16], const float color[4]);

	/** Write all pending instances into the ring buffer slice starting at
	 *  \p dest, starting at absolute instance offset \p baseInst.
	 *  Fills out the draw command and appends it to \p cmds. */
	void WriteInstancesAndCommand(
		MDEI_Instance *dest,
		unsigned int   baseInst,
		std::vector<DrawElementsIndirectCommand>& cmds);

	unsigned int PendingCount() const { return (unsigned int)m_pendingInstances.size(); }

private:
	MDEI_Mesh   *m_mesh;
	MDEI_Shader *m_shader;

	struct RawInstance {
		float matrix[16];
		float color[4];
	};
	std::vector<RawInstance> m_pendingInstances;
};

#endif /* __MDEI_DRAW_GROUP_H__ */
