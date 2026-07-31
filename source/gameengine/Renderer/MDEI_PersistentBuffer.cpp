/*
 * MDEI_PersistentBuffer.cpp
 */

#include "MDEI_PersistentBuffer.h"

#include <cstring>
#include <cstdio>

MDEI_PersistentBuffer::MDEI_PersistentBuffer()
	: m_ssbo(0), m_commandBuffer(0), m_mapped(nullptr)
{
	for (int i = 0; i < MDEI_RING_SEGMENTS; i++) {
		m_fences[i] = nullptr;
	}
}

MDEI_PersistentBuffer::~MDEI_PersistentBuffer()
{
	Shutdown();
}

void MDEI_PersistentBuffer::Init()
{
	const size_t ssboSize = MDEI_RING_SEGMENTS * MDEI_MAX_INSTANCES * sizeof(MDEI_Instance);
	const GLbitfield mapFlags = GL_MAP_WRITE_BIT |
	                            GL_MAP_PERSISTENT_BIT |
	                            GL_MAP_COHERENT_BIT;

	/* SSBO — persistently mapped instance data */
	glGenBuffers(1, &m_ssbo);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_ssbo);
	glBufferStorage(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)ssboSize, nullptr, mapFlags);
	m_mapped = (MDEI_Instance *)glMapBufferRange(
		GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)ssboSize, mapFlags);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

	if (!m_mapped) {
		fprintf(stderr, "[MDEI] glMapBufferRange failed — SSBO not created\n");
	}

	/* Draw-indirect command buffer — small, so plain BufferData each frame is OK */
	const size_t cmdSize = 4096 * sizeof(DrawElementsIndirectCommand);
	glGenBuffers(1, &m_commandBuffer);
	glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_commandBuffer);
	glBufferData(GL_DRAW_INDIRECT_BUFFER, (GLsizeiptr)cmdSize, nullptr, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
}

void MDEI_PersistentBuffer::Shutdown()
{
	for (int i = 0; i < MDEI_RING_SEGMENTS; i++) {
		if (m_fences[i]) {
			glDeleteSync(m_fences[i]);
			m_fences[i] = nullptr;
		}
	}

	if (m_ssbo) {
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_ssbo);
		glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
		glDeleteBuffers(1, &m_ssbo);
		m_ssbo = 0;
		m_mapped = nullptr;
	}

	if (m_commandBuffer) {
		glDeleteBuffers(1, &m_commandBuffer);
		m_commandBuffer = 0;
	}
}

MDEI_Instance *MDEI_PersistentBuffer::BeginFrame(int frameIndex)
{
	const int seg = frameIndex % MDEI_RING_SEGMENTS;

	if (m_fences[seg]) {
		/* Wait at most 5 seconds for the GPU to finish with this segment */
		GLenum result = glClientWaitSync(m_fences[seg], GL_SYNC_FLUSH_COMMANDS_BIT, 5000000000ULL);
		if (result == GL_TIMEOUT_EXPIRED || result == GL_WAIT_FAILED) {
			fprintf(stderr, "[MDEI] GPU sync timeout on segment %d\n", seg);
		}
		glDeleteSync(m_fences[seg]);
		m_fences[seg] = nullptr;
	}

	/* Return pointer to the writable region for this segment */
	return m_mapped + (seg * MDEI_MAX_INSTANCES);
}

void MDEI_PersistentBuffer::EndFrame(int frameIndex)
{
	const int seg = frameIndex % MDEI_RING_SEGMENTS;
	m_fences[seg] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

void MDEI_PersistentBuffer::UploadCommands(
	const std::vector<DrawElementsIndirectCommand>& cmds)
{
	if (cmds.empty()) return;
	glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_commandBuffer);
	glBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0,
	                (GLsizeiptr)(cmds.size() * sizeof(DrawElementsIndirectCommand)),
	                cmds.data());
	glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
}

void MDEI_PersistentBuffer::Bind(int frameIndex)
{
	const int seg = frameIndex % MDEI_RING_SEGMENTS;
	const size_t offset = (size_t)seg * MDEI_MAX_INSTANCES * sizeof(MDEI_Instance);
	/* Bind the segment's slice of the SSBO at binding point 0 */
	glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
	                  m_ssbo,
	                  (GLintptr)offset,
	                  (GLsizeiptr)(MDEI_MAX_INSTANCES * sizeof(MDEI_Instance)));
	/* Bind the command buffer */
	glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_commandBuffer);
}

void MDEI_PersistentBuffer::Unbind()
{
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
	glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
}
