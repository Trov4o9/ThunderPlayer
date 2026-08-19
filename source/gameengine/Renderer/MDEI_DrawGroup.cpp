/*
 * MDEI_DrawGroup.cpp
 */

#include "MDEI_DrawGroup.h"
#include "MDEI_Mesh.h"

#include <cstring>

MDEI_DrawGroup::MDEI_DrawGroup(MDEI_Mesh *mesh, MDEI_Shader *shader)
	: m_mesh(mesh), m_shader(shader)
{
}

void MDEI_DrawGroup::AddInstance(const float matrix[16], const float color[4])
{
	RawInstance inst;
	memcpy(inst.matrix, matrix, sizeof(inst.matrix));
	memcpy(inst.color,  color,  sizeof(inst.color));
	m_pendingInstances.push_back(inst);
}

void MDEI_DrawGroup::WriteInstancesAndCommand(
	MDEI_Instance *dest,
	unsigned int   baseInst,
	std::vector<DrawElementsIndirectCommand>& cmds)
{
	const unsigned int count = (unsigned int)m_pendingInstances.size();
	if (count == 0) return;

	/* Write per-instance data into the ring buffer */
	for (unsigned int i = 0; i < count; i++) {
		memcpy(dest[i].matrix, m_pendingInstances[i].matrix, 64);
		memcpy(dest[i].color,  m_pendingInstances[i].color,  16);
	}

	/* Pula grupo se o mesh não tem geometria (upload pendente ou após Release) */
	if (m_mesh->GetIndexCount() == 0 || m_mesh->GetVAO() == 0) return;

	/* One draw command for all instances in this group */
	DrawElementsIndirectCommand cmd;
	cmd.count         = (GLuint)m_mesh->GetIndexCount();
	cmd.instanceCount = count;
	cmd.firstIndex    = 0;
	cmd.baseVertex    = 0;
	cmd.baseInstance  = baseInst;
	cmds.push_back(cmd);
}
