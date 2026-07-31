/*
 * MDEI_Mesh.cpp
 */

#include "MDEI_Mesh.h"

#include <cstring>
#include <cstdio>

/* CD_ type codes needed for BindUVAttribs */
#include "DNA_customdata_types.h"   /* CD_MTFACE, CD_ORCO, CD_NORMAL */

MDEI_Mesh::MDEI_Mesh()
	: m_vao(0), m_vbo(0), m_ebo(0), m_indexCount(0),
	  m_aabbMin(mt::zero3), m_aabbMax(mt::zero3),
	  m_uvCount(0), m_activeUv(0), m_stride(0)
{
	memset(m_uvOffset, 0, sizeof(m_uvOffset));
}

MDEI_Mesh::~MDEI_Mesh()
{
	Release();
}

void MDEI_Mesh::Upload(const std::vector<MDEI_Vertex>& verts,
                       const std::vector<unsigned int>& indices,
                       int uvCount,
                       const std::string uvNames[MDEI_MAX_UV],
                       int activeUv)
{
	m_indexCount = (GLsizei)indices.size();
	m_uvCount    = uvCount;
	m_activeUv   = activeUv;
	for (int i = 0; i < MDEI_MAX_UV; i++)
		m_uvNames[i] = uvNames[i];

	/* ── Compute the VBO layout ──────────────────────────────────────
	 * We store only the used part of MDEI_Vertex:
	 *   pos(3f) + normal(3f) + uv[0..uvCount-1](2f each)
	 *
	 * MDEI_Vertex always has MDEI_MAX_UV UV slots, but we only upload
	 * the first uvCount of them to avoid wasting GPU memory.
	 * ------------------------------------------------------------------
	 * Offsets (bytes):
	 *   pos    → 0
	 *   normal → 12
	 *   uv[i]  → 24 + i*8
	 * ------------------------------------------------------------------ */
	const int uvSlots  = (uvCount > 0) ? uvCount : 1;   /* at least 1 slot (may be zero UV) */
	m_stride           = (GLsizei)(24 + uvSlots * 8);

	for (int i = 0; i < MDEI_MAX_UV; i++)
		m_uvOffset[i] = (GLintptr)(24 + i * 8);

	/* Pack the vertex data using only the used UV range */
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

	glGenVertexArrays(1, &m_vao);
	glGenBuffers(1, &m_vbo);
	glGenBuffers(1, &m_ebo);

	glBindVertexArray(m_vao);

	/* VBO */
	glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
	glBufferData(GL_ARRAY_BUFFER,
	             (GLsizeiptr)(packed.size() * sizeof(float)),
	             packed.data(), GL_STATIC_DRAW);

	/* EBO */
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER,
	             (GLsizeiptr)(indices.size() * sizeof(unsigned int)),
	             indices.data(), GL_STATIC_DRAW);

	/* ── Fixed-pipeline attributes (position + normal) ───────────────
	 * The vertex shader reads gl_Vertex and gl_Normal via FFP built-ins,
	 * which are driven by glVertexPointer / glNormalPointer.
	 * The VAO captures these client-state bindings for later replay.
	 * ------------------------------------------------------------------ */
	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(3, GL_FLOAT, m_stride, (const void *)0);

	glEnableClientState(GL_NORMAL_ARRAY);
	glNormalPointer(GL_FLOAT, m_stride, (const void *)12);

	/* NOTE: UV generic attributes (att0, att1, ...) are NOT bound here.
	 * They are bound per-material at draw time via BindUVAttribs()
	 * because the glindex depends on the compiled GPUMaterial. */

	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	/* Unbind EBO *after* VAO to preserve the binding inside the VAO. */
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	/* Compute AABB */
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

void MDEI_Mesh::BindUVAttribs(const GPUVertexAttribs& attribs) const
{
	/* Re-bind VBO so glVertexAttribPointer offsets resolve correctly. */
	glBindBuffer(GL_ARRAY_BUFFER, m_vbo);

	for (int i = 0; i < attribs.totlayer; i++) {
		const int type    = attribs.layer[i].type;
		const int glindex = attribs.layer[i].glindex;
		if (glindex < 0) continue;

		if (type == CD_MTFACE) {
			/* Find which UV layer slot to use */
			const char *layerName = attribs.layer[i].name;
			int uvSlot = -1;

			if (!layerName[0]) {
				/* Empty name → use active UV */
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

			if (uvSlot < 0 || uvSlot >= m_uvCount) {
				/* Fallback: first UV layer, or zero if none */
				uvSlot = (m_uvCount > 0) ? 0 : -1;
			}

			if (uvSlot >= 0) {
				glEnableVertexAttribArray((GLuint)glindex);
				glVertexAttribPointer(
					(GLuint)glindex, 2, GL_FLOAT, GL_FALSE,
					m_stride,
					(const void *)m_uvOffset[uvSlot]);
			}
		}
		else if (type == CD_ORCO) {
			/* Object-space position (used by Generated texture coordinates) */
			glEnableVertexAttribArray((GLuint)glindex);
			glVertexAttribPointer(
				(GLuint)glindex, 3, GL_FLOAT, GL_FALSE,
				m_stride, (const void *)0);
		}
		else if (type == CD_NORMAL) {
			/* Normal attribute */
			glEnableVertexAttribArray((GLuint)glindex);
			glVertexAttribPointer(
				(GLuint)glindex, 3, GL_FLOAT, GL_FALSE,
				m_stride, (const void *)12);
		}
		/* CD_TANGENT and CD_MCOL are not supported by MDEI (simple meshes). */
	}

	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void MDEI_Mesh::Release()
{
	if (m_vao) { glDeleteVertexArrays(1, &m_vao); m_vao = 0; }
	if (m_vbo) { glDeleteBuffers(1, &m_vbo); m_vbo = 0; }
	if (m_ebo) { glDeleteBuffers(1, &m_ebo); m_ebo = 0; }
}
