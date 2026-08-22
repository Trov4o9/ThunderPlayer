/*
 * MDEI_Mesh.h — Geometry data for the MDEI fast-path renderer.
 * One MDEI_Mesh per unique Blender Mesh (shared by all instances).
 *
 * UV layout
 * ---------
 * The Blender codegen vertex shader declares generic attributes:
 *   in vec2 att0;   (first UV layer requested by the material)
 *   in vec2 att1;   (second UV layer, if any)
 *   ...
 * The attribute *location* for each layer is returned by
 * GPU_material_vertex_attributes() as attribs.layer[i].glindex.
 *
 * We store ALL UV layers found in the mesh in a single VBO, tightly
 * packed per vertex:
 *   [pos(3f) | normal(3f) | uv0(2f) | uv1(2f) | ... ]
 *
 * Skinned VBO ring buffer
 * -----------------------
 * For meshes with armature deformation (m_isSkinnedVBO == true):
 *   • The VBO is allocated with glBufferStorage + persistent-map flags
 *     so the CPU can write directly without any stall.
 *   • MDEI_RING_SEGMENTS separate VAOs are created, each pointing to a
 *     different segment of the same VBO.  This avoids VAO rebind overhead
 *     while allowing the CPU and GPU to work on different segments.
 *   • BeginSkinFrame() waits (if needed) for the GPU to finish the
 *     oldest in-flight segment, then returns the write pointer.
 *   • EndSkinFrame() places a fence sync on the segment just written.
 *   • GetCurrentVAO() returns the VAO for the segment that was just written
 *     (same segment that will be drawn this frame).
 *
 * Static VBO (m_isSkinnedVBO == false):
 *   • Allocated with glBufferData / GL_STATIC_DRAW.
 *   • Single VAO.  Never updated at runtime.
 */

#ifndef __MDEI_MESH_H__
#define __MDEI_MESH_H__

#include "GPU_glew.h"
#include "GPU_shader.h"   /* GPUVertexAttribs */
#include "mathfu.h"

#include <vector>
#include <string>

/* Number of VBO ring segments for skinned meshes.
 * Matches MDEI_RING_SEGMENTS from MDEI_PersistentBuffer so the CPU
 * writer and GPU consumer are always in different segments. */
#ifndef MDEI_RING_SEGMENTS
#  define MDEI_RING_SEGMENTS 6
#endif

/* Maximum UV layers stored in a single MDEI mesh.
 * Matches RAS_Texture::MaxUnits (8) for parity.  */
#define MDEI_MAX_UV 8

/** Per-vertex data packed into the VBO. */
struct MDEI_Vertex {
	float px, py, pz;              /* position  */
	float nx, ny, nz;              /* normal    */
	float uv[MDEI_MAX_UV][2];      /* UV layers */
};

/** A mesh ready for glMultiDrawElementsIndirect.
 *  Created once per unique Blender Mesh* (static) or per Object* (skinned). */
class MDEI_Mesh {
public:
	MDEI_Mesh();
	~MDEI_Mesh();

	/** Upload vertices and indices to the GPU.
	 *  @param uvCount      How many UV layers are filled in MDEI_Vertex::uv[].
	 *  @param uvNames      Layer names parallel to uv[0..uvCount-1].
	 *  @param activeUv     Index of the active UV layer (used when name is "").
	 *  @param origIndexMap Maps vboIndex → Mesh.mvert[] index.  Non-empty means
	 *                      this is a skinned mesh; a persistent-mapped ring VBO
	 *                      with MDEI_RING_SEGMENTS VAOs will be allocated. */
	/** cpuColors: optional per-vertex color data, matching the RAS storage format.
	 *
	 *  Layout: flat array of (vertCount × colorLayers) uint32_t values, interleaved
	 *  per vertex — [v0_L0, v0_L1, ..., v1_L0, v1_L1, ...].
	 *  Each uint32_t is a packed RGBA value identical to RAS_DisplayArray::Color::m_flat
	 *  (bytes: r=bits[0..7], g=bits[8..15], b=bits[16..23], a=bits[24..31] — matching
	 *  the memory layout of MLoopCol { unsigned char r, g, b, a } on a little-endian
	 *  machine via union reinterpretation, same as BL_GetUvRgba in the converter).
	 *
	 *  Empty (or wrong size) → m_cpuColors is cleared; all vertices are treated as
	 *  fully opaque surface (alpha = 255) by any consumer.
	 *  colorLayers = 0 → same as empty. */
	void Upload(const std::vector<MDEI_Vertex>& verts,
	            const std::vector<unsigned int>& indices,
	            int uvCount,
	            const std::string uvNames[MDEI_MAX_UV],
	            int activeUv,
	            const std::vector<int>& origIndexMap = std::vector<int>(),
	            const std::vector<uint32_t>& cpuColors = std::vector<uint32_t>(),
	            int colorLayers = 0);

	/* ── Skinned VBO ring-buffer API ─────────────────────────────────────
	 *
	 * Usage per frame (called by MDEI_SkinDeformer):
	 *
	 *   float *ptr = mesh->BeginSkinFrame();   // wait + get write pointer
	 *   // write pos/nor into ptr for segment m_writeSegment
	 *   mesh->EndSkinFrame();                  // place fence sync
	 *
	 * Then at draw time (MDEI_Renderer::ExecuteDraw):
	 *   glBindVertexArray(mesh->GetCurrentVAO());
	 * ------------------------------------------------------------------ */

	/** Wait for the next segment to be free and return a write pointer to
	 *  the segment's vertex data (floats).  Only valid on skinned meshes. */
	float *BeginSkinFrame();

	/** Place a fence sync on the segment just written.
	 *  Must be called after BeginSkinFrame() + data write + draw submission. */
	void EndSkinFrame();

	/** Bind UV generic attributes using locations from GPUVertexAttribs. */
	void BindUVAttribs(const GPUVertexAttribs& attribs) const;

	/** Release all GL objects. */
	void Release();

	/* ── Accessors ──────────────────────────────────────────────────────── */

	/** VAO to bind for drawing.
	 *  Static mesh → single VAO.
	 *  Skinned mesh → VAO for the segment written by the last BeginSkinFrame(). */
	GLuint  GetCurrentVAO()  const;
	/** Alias para compatibilidade com código de debug que usa GetVAO(). */
	GLuint  GetVAO()         const { return GetCurrentVAO(); }
	GLuint  GetVBO()         const { return m_vbo; }
	GLuint  GetEBO()         const { return m_ebo; }
	GLsizei GetIndexCount()  const { return m_indexCount; }
	GLsizei GetVertCount()   const { return m_vertCount; }
	int     GetFloatsPerVert() const { return (int)(m_stride / sizeof(float)); }
	bool    IsSkinned()      const { return m_isSkinnedVBO; }

	/* ── Accessors needed by MDEI_GeometryPool ───────────────────────── */

	/** Full packed vertex buffer (pos+nor+uvs) — stored after Upload().
	 *  Empty for skinned meshes (they write directly to the ring VBO). */
	const std::vector<float>&        GetPackedVerts() const { return m_packedVerts; }
	const std::vector<unsigned int>& GetCpuInds()    const { return m_cpuInds; }

	GLsizei  GetStride()           const { return m_stride; }
	int      GetUVCount()          const { return m_uvCount; }
	int      GetActiveUV()         const { return m_activeUv; }
	const std::string& GetUVName(int i) const { return m_uvNames[i]; }
	GLintptr GetUVOffset(int i)    const { return m_uvOffset[i]; }

	/** AABB (local-space). */
	mt::vec3 m_aabbMin;
	mt::vec3 m_aabbMax;

	/** Cópia CPU da geometria — mantida entre uploads.
	 *
	 *  m_cpuVerts:      posições empacotadas [x0,y0,z0, x1,y1,z1, ...]
	 *  m_cpuInds:       índices dos triângulos (triplas).
	 *  m_cpuColors:     cor por vértice, packed RGBA uint32 igual ao RAS.
	 *                   Layout flat interleaved: [v0_L0, v0_L1, ..., v1_L0, ...]
	 *                   Tamanho = vertCount × m_cpuColorLayers.
	 *                   Vazio se nenhuma camada de cor foi fornecida.
	 *  m_cpuColorLayers: número de camadas de cor em m_cpuColors (0 se vazio).
	 *
	 *  Para obter o alpha do vértice i, camada 0:
	 *    uint8_t a = (m_cpuColors[i * m_cpuColorLayers + 0] >> 24) & 0xFF;
	 *
	 *  Liberada por Release() junto com os buffers GL. */
	std::vector<float>         m_cpuVerts;
	std::vector<unsigned int>  m_cpuInds;
	std::vector<uint32_t>      m_cpuColors;
	int                        m_cpuColorLayers = 0;

	/* UV metadata */
	int         m_uvCount;
	std::string m_uvNames[MDEI_MAX_UV];
	int         m_activeUv;

	/** vboIndex → Mesh.mvert[] original index (skinned only). */
	std::vector<int> m_origIndexMap;

private:
	void BuildVAO(GLuint vao, GLuint vbo, GLintptr segmentByteOffset);

	/* ── Static mesh fields ─────────────────────────────────────────── */
	GLuint  m_vao;          /* single VAO for static meshes */
	GLuint  m_vbo;          /* VBO (GL_STATIC_DRAW)         */
	GLuint  m_ebo;          /* EBO (always GL_STATIC_DRAW)  */
	GLsizei m_indexCount;
	GLsizei m_vertCount;
	GLsizei m_stride;
	GLintptr m_uvOffset[MDEI_MAX_UV];
	bool    m_isSkinnedVBO;

	/** Packed vertex data kept in RAM for MDEI_GeometryPool (static only). */
	std::vector<float> m_packedVerts;

	/* ── Skinned mesh ring-buffer fields ────────────────────────────── */
	GLuint  m_vaoRing[MDEI_RING_SEGMENTS];  /* one VAO per segment */
	float  *m_persistentPtr;                /* base of persistent map */
	GLsync  m_fences[MDEI_RING_SEGMENTS];
	int     m_writeSegment;                 /* segment written last BeginSkinFrame() */
};

#endif /* __MDEI_MESH_H__ */
