/*
 * MDEI_Mesh.cpp
 *
 * Static meshes:  single VBO + single VAO, GL_STATIC_DRAW, never updated.
 * Skinned meshes: one large VBO with MDEI_RING_SEGMENTS contiguous segments,
 *                 allocated with glBufferStorage + persistent-map flags.
 *                 One VAO per segment, each pointing into its slice of the VBO.
 *                 CPU writes directly through m_persistentPtr (zero stalls).
 *                 GLsync fences guard each segment so the GPU never reads a
 *                 segment while the CPU is writing to it.
 */

#include "MDEI_Mesh.h"

#include <cstring>
#include <cstdio>

/* CD_ type codes needed for BindUVAttribs */
#include "DNA_customdata_types.h"   /* CD_MTFACE, CD_ORCO, CD_NORMAL */
#include "mathfu.h"                 /* mt::vec3_packed (for BindUVAttribs context) */

/* ── ctor / dtor ─────────────────────────────────────────────────────────── */

MDEI_Mesh::MDEI_Mesh()
	: m_vao(0), m_vbo(0), m_ebo(0)
	, m_indexCount(0), m_vertCount(0), m_stride(0)
	, m_aabbMin(mt::zero3), m_aabbMax(mt::zero3)
	, m_uvCount(0), m_activeUv(0)
	, m_isSkinnedVBO(false)
	, m_persistentPtr(nullptr)
	, m_writeSegment(0)
{
	memset(m_uvOffset, 0, sizeof(m_uvOffset));
	for (int i = 0; i < MDEI_RING_SEGMENTS; i++) {
		m_vaoRing[i] = 0;
		m_fences[i]  = nullptr;
	}
}

MDEI_Mesh::~MDEI_Mesh()
{
	Release();
}

/* ── BuildVAO ────────────────────────────────────────────────────────────── */
/* Set up one VAO whose VBO pointer points at segmentByteOffset inside vbo. */
void MDEI_Mesh::BuildVAO(GLuint vao, GLuint vbo, GLintptr segmentByteOffset)
{
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);

	/* Fixed-pipeline pos + normal (FFP built-ins gl_Vertex / gl_Normal) */
	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(3, GL_FLOAT, m_stride,
	                (const void *)(segmentByteOffset + 0));

	glEnableClientState(GL_NORMAL_ARRAY);
	glNormalPointer(GL_FLOAT, m_stride,
	                (const void *)(segmentByteOffset + 12));

	/* EBO is shared across all segments */
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);

	glBindVertexArray(0);
	/* Unbind EBO *after* VAO to keep the binding inside the VAO */
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

/* ── Upload ──────────────────────────────────────────────────────────────── */

void MDEI_Mesh::Upload(const std::vector<MDEI_Vertex>& verts,
                       const std::vector<unsigned int>& indices,
                       int uvCount,
                       const std::string uvNames[MDEI_MAX_UV],
                       int activeUv,
                       const std::vector<int>& origIndexMap)
{
	m_indexCount   = (GLsizei)indices.size();
	m_vertCount    = (GLsizei)verts.size();
	m_uvCount      = uvCount;
	m_activeUv     = activeUv;
	m_origIndexMap = origIndexMap;
	m_isSkinnedVBO = !origIndexMap.empty();

	for (int i = 0; i < MDEI_MAX_UV; i++)
		m_uvNames[i] = uvNames[i];

	/* Compute stride and UV offsets:
	 *   pos(3f) + normal(3f) + uv[0..uvSlots-1](2f each)
	 *   pos    → byte  0
	 *   normal → byte 12
	 *   uv[i]  → byte 24 + i*8
	 */
	const int uvSlots = (uvCount > 0) ? uvCount : 1;
	m_stride          = (GLsizei)(24 + uvSlots * 8);
	for (int i = 0; i < MDEI_MAX_UV; i++)
		m_uvOffset[i] = (GLintptr)(24 + i * 8);

	/* Pack vertex data */
	std::vector<float> packed;
	packed.reserve(verts.size() * (size_t)(m_stride / 4));
	for (const MDEI_Vertex& v : verts) {
		packed.push_back(v.px); packed.push_back(v.py); packed.push_back(v.pz);
		packed.push_back(v.nx); packed.push_back(v.ny); packed.push_back(v.nz);
		for (int i = 0; i < uvSlots; i++) {
			packed.push_back(v.uv[i][0]);
			packed.push_back(v.uv[i][1]);
		}
	}

	const GLsizeiptr vertBytes = (GLsizeiptr)(packed.size() * sizeof(float));

	/* EBO — shared, never changes */
	glGenBuffers(1, &m_ebo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER,
	             (GLsizeiptr)(indices.size() * sizeof(unsigned int)),
	             indices.data(), GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	if (!m_isSkinnedVBO) {
		/* ── Static path: one VBO + one VAO ──────────────────────────── */
		glGenBuffers(1, &m_vbo);
		glGenVertexArrays(1, &m_vao);

		glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
		glBufferData(GL_ARRAY_BUFFER, vertBytes, packed.data(), GL_STATIC_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, 0);

		BuildVAO(m_vao, m_vbo, 0);
	}
	else {
		/* ── Skinned path: persistent ring VBO + N VAOs ───────────────
		 *
		 * Total VBO size = MDEI_RING_SEGMENTS * bytes_per_segment.
		 * Each segment holds one full copy of the vertex data.
		 * Flags:
		 *   GL_MAP_WRITE_BIT      — CPU will write
		 *   GL_MAP_PERSISTENT_BIT — mapping stays valid while GPU uses the buffer
		 *   GL_MAP_COHERENT_BIT   — no explicit flush needed (coherent writes)
		 * ------------------------------------------------------------------ */
		const GLsizeiptr segBytes = vertBytes;
		const GLsizeiptr totalBytes = (GLsizeiptr)MDEI_RING_SEGMENTS * segBytes;
		const GLbitfield storageFlags = GL_MAP_WRITE_BIT |
		                                GL_MAP_PERSISTENT_BIT |
		                                GL_MAP_COHERENT_BIT;

		glGenBuffers(1, &m_vbo);
		glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
		glBufferStorage(GL_ARRAY_BUFFER, totalBytes, nullptr, storageFlags);

		/* Persistently map the entire buffer once — stays mapped until Release() */
		m_persistentPtr = (float *)glMapBufferRange(
		    GL_ARRAY_BUFFER, 0, totalBytes, storageFlags);

		if (!m_persistentPtr) {
			fprintf(stderr, "[MDEI] MDEI_Mesh: persistent map failed for skinned VBO\n");
		}
		glBindBuffer(GL_ARRAY_BUFFER, 0);

		/* Pre-fill every segment with the rest-pose vertex data so the
		 * mesh renders correctly even before the first deformation frame. */
		if (m_persistentPtr) {
			for (int seg = 0; seg < MDEI_RING_SEGMENTS; seg++) {
				float *dst = m_persistentPtr +
				             (size_t)seg * (vertBytes / sizeof(float));
				memcpy(dst, packed.data(), (size_t)vertBytes);
			}
		}

		/* Create one VAO per segment */
		glGenVertexArrays(MDEI_RING_SEGMENTS, m_vaoRing);
		for (int seg = 0; seg < MDEI_RING_SEGMENTS; seg++) {
			BuildVAO(m_vaoRing[seg], m_vbo,
			         (GLintptr)((size_t)seg * (size_t)vertBytes));
		}

		m_writeSegment = 0;
	}

	/* Compute rest-pose AABB */
	m_aabbMin = mt::vec3( 1e30f,  1e30f,  1e30f);
	m_aabbMax = mt::vec3(-1e30f, -1e30f, -1e30f);
	for (const MDEI_Vertex& v : verts) {
		if (v.px < m_aabbMin[0]) m_aabbMin[0] = v.px;
		if (v.py < m_aabbMin[1]) m_aabbMin[1] = v.py;
		if (v.pz < m_aabbMin[2]) m_aabbMin[2] = v.pz;
		if (v.px > m_aabbMax[0]) m_aabbMax[0] = v.px;
		if (v.py > m_aabbMax[1]) m_aabbMax[1] = v.py;
		if (v.pz > m_aabbMax[2]) m_aabbMax[2] = v.pz;
	}
}

/* ── BeginSkinFrame / EndSkinFrame ───────────────────────────────────────── */

float *MDEI_Mesh::BeginSkinFrame()
{
	if (!m_isSkinnedVBO || !m_persistentPtr)
		return nullptr;

	/* Advance to the next segment (round-robin) */
	m_writeSegment = (m_writeSegment + 1) % MDEI_RING_SEGMENTS;

	/* Wait for the GPU to finish any draw that was reading this segment */
	if (m_fences[m_writeSegment]) {
		GLenum result = glClientWaitSync(m_fences[m_writeSegment],
		                                 GL_SYNC_FLUSH_COMMANDS_BIT,
		                                 5000000000ULL /* 5 s */);
		if (result == GL_TIMEOUT_EXPIRED || result == GL_WAIT_FAILED) {
			fprintf(stderr, "[MDEI] MDEI_Mesh: GPU sync timeout on skin segment %d\n",
			        m_writeSegment);
		}
		glDeleteSync(m_fences[m_writeSegment]);
		m_fences[m_writeSegment] = nullptr;
	}

	/* Return the base pointer for this segment */
	const int floatsPerVert  = m_stride / (int)sizeof(float);
	const size_t segFloats   = (size_t)m_vertCount * (size_t)floatsPerVert;
	return m_persistentPtr + (size_t)m_writeSegment * segFloats;
}

void MDEI_Mesh::EndSkinFrame()
{
	if (!m_isSkinnedVBO) return;
	/* The fence is placed *after* the draw call that uses this segment
	 * so the GPU signals it when the draw finishes.  We place it here
	 * so it covers any draw that follows this write in the same frame. */
	m_fences[m_writeSegment] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

/* ── GetCurrentVAO ───────────────────────────────────────────────────────── */

GLuint MDEI_Mesh::GetCurrentVAO() const
{
	if (!m_isSkinnedVBO)
		return m_vao;
	return m_vaoRing[m_writeSegment];
}

/* ── BindUVAttribs ───────────────────────────────────────────────────────── */

void MDEI_Mesh::BindUVAttribs(const GPUVertexAttribs& attribs) const
{
	/* Bind the correct VBO slice for the active segment (skinned) or the
	 * static VBO.  glVertexAttribPointer offsets are absolute byte addresses
	 * relative to the currently bound GL_ARRAY_BUFFER. */
	glBindBuffer(GL_ARRAY_BUFFER, m_vbo);

	/* For skinned meshes, calculate the byte offset of the current segment */
	const int floatsPerVert = m_stride / (int)sizeof(float);
	const GLintptr segOffset = m_isSkinnedVBO
	    ? (GLintptr)((size_t)m_writeSegment *
	                 (size_t)m_vertCount * (size_t)floatsPerVert * sizeof(float))
	    : (GLintptr)0;

	for (int i = 0; i < attribs.totlayer; i++) {
		const int type    = attribs.layer[i].type;
		const int glindex = attribs.layer[i].glindex;
		if (glindex < 0) continue;

		if (type == CD_MTFACE) {
			const char *layerName = attribs.layer[i].name;
			int uvSlot = -1;

			if (!layerName[0]) {
				uvSlot = (m_activeUv < m_uvCount) ? m_activeUv : 0;
			}
			else {
				for (int j = 0; j < m_uvCount; j++) {
					if (m_uvNames[j] == layerName) {
						uvSlot = j;
						break;
					}
				}
			}
			if (uvSlot < 0 || uvSlot >= m_uvCount)
				uvSlot = (m_uvCount > 0) ? 0 : -1;

			if (uvSlot >= 0) {
				glEnableVertexAttribArray((GLuint)glindex);
				glVertexAttribPointer(
				    (GLuint)glindex, 2, GL_FLOAT, GL_FALSE,
				    m_stride,
				    (const void *)(segOffset + m_uvOffset[uvSlot]));
			}
		}
		else if (type == CD_ORCO) {
			glEnableVertexAttribArray((GLuint)glindex);
			glVertexAttribPointer(
			    (GLuint)glindex, 3, GL_FLOAT, GL_FALSE,
			    m_stride, (const void *)(segOffset + 0));
		}
		else if (type == CD_NORMAL) {
			glEnableVertexAttribArray((GLuint)glindex);
			glVertexAttribPointer(
			    (GLuint)glindex, 3, GL_FLOAT, GL_FALSE,
			    m_stride, (const void *)(segOffset + 12));
		}
	}

	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

/* ── Release ─────────────────────────────────────────────────────────────── */

void MDEI_Mesh::Release()
{
	/* Fences */
	for (int i = 0; i < MDEI_RING_SEGMENTS; i++) {
		if (m_fences[i]) {
			glDeleteSync(m_fences[i]);
			m_fences[i] = nullptr;
		}
	}

	/* Ring VAOs (skinned) */
	for (int i = 0; i < MDEI_RING_SEGMENTS; i++) {
		if (m_vaoRing[i]) {
			glDeleteVertexArrays(1, &m_vaoRing[i]);
			m_vaoRing[i] = 0;
		}
	}

	/* Static VAO */
	if (m_vao) { glDeleteVertexArrays(1, &m_vao); m_vao = 0; }

	/* VBO — unmap persistent before deleting */
	if (m_vbo) {
		if (m_isSkinnedVBO && m_persistentPtr) {
			glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
			glUnmapBuffer(GL_ARRAY_BUFFER);
			glBindBuffer(GL_ARRAY_BUFFER, 0);
			m_persistentPtr = nullptr;
		}
		glDeleteBuffers(1, &m_vbo);
		m_vbo = 0;
	}

	/* EBO */
	if (m_ebo) { glDeleteBuffers(1, &m_ebo); m_ebo = 0; }

	m_origIndexMap.clear();
	m_isSkinnedVBO = false;
	m_vertCount    = 0;
}
