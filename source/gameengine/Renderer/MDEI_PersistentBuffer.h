/*
 * MDEI_PersistentBuffer.h — Triple ring-buffer persistently mapped (GL 4.4).
 * Manages the SSBO of per-instance data and the draw-indirect command buffer.
 */

#ifndef __MDEI_PERSISTENT_BUFFER_H__
#define __MDEI_PERSISTENT_BUFFER_H__

#include "GPU_glew.h"
#include "MDEI_Instance.h"

#include <vector>

/** Maximum instances that can be rendered per frame across all draw groups. */
#define MDEI_MAX_INSTANCES 65536

/** Number of ring-buffer segments.
 *  6 = 3 logical frames × 2 passes (solid + shadow) so they never share a slot. */
#define MDEI_RING_SEGMENTS 6

struct DrawElementsIndirectCommand {
	GLuint count;          /* number of indices           */
	GLuint instanceCount;  /* number of instances         */
	GLuint firstIndex;     /* byte offset / sizeof(uint)  */
	GLint  baseVertex;     /* added to every index        */
	GLuint baseInstance;   /* gl_BaseInstance value       */
};
static_assert(sizeof(DrawElementsIndirectCommand) == 20,
              "DrawElementsIndirectCommand must be 20 bytes");

class MDEI_PersistentBuffer {
public:
	MDEI_PersistentBuffer();
	~MDEI_PersistentBuffer();

	void Init();
	void Shutdown();

	/** Call before writing instances for frame N. Waits if the GPU is
	 *  still consuming this segment, then returns a pointer to write into. */
	MDEI_Instance *BeginFrame(int frameIndex);

	/** Call after all instances have been written. */
	void EndFrame(int frameIndex);

	/** Upload draw commands (small, so plain glBufferSubData is fine). */
	void UploadCommands(const std::vector<DrawElementsIndirectCommand>& cmds);

	/** Bind the SSBO at binding 0 and the draw-indirect buffer. */
	void Bind(int frameIndex);
	void Unbind();

	/** Returns the base byte offset for the current segment. */
	size_t SegmentOffset(int frameIndex) const
	{
		return (size_t)(frameIndex % MDEI_RING_SEGMENTS) * MDEI_MAX_INSTANCES * sizeof(MDEI_Instance);
	}

private:
	GLuint        m_ssbo;           /* GL_SHADER_STORAGE_BUFFER  */
	GLuint        m_commandBuffer;  /* GL_DRAW_INDIRECT_BUFFER   */
	MDEI_Instance *m_mapped;        /* persistent write pointer  */
	GLsync        m_fences[MDEI_RING_SEGMENTS];
};

#endif /* __MDEI_PERSISTENT_BUFFER_H__ */
