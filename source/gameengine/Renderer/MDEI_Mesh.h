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
 * At draw time, BindUVAttribs(attribs) walks the GPUVertexAttribs and
 * calls glVertexAttribPointer(glindex, ...) for each UV layer matched
 * by name (or the active layer when the name is empty).
 */

#ifndef __MDEI_MESH_H__
#define __MDEI_MESH_H__

#include "GPU_glew.h"
#include "GPU_shader.h"   /* GPUVertexAttribs */
#include "mathfu.h"

#include <vector>
#include <string>

/* Maximum UV layers stored in a single MDEI mesh.
 * Matches RAS_Texture::MaxUnits (8) for parity.  */
#define MDEI_MAX_UV 8

/** Per-vertex data packed into the VBO.
 *  Position and normal are always present.
 *  UV layers 0..uvCount-1 follow immediately after.
 *  Total size = 24 + uvCount*8 bytes.
 *  The struct itself only holds the fixed head; UV data is stored
 *  in a parallel vector<float[2]> during build, then interleaved. */
struct MDEI_Vertex {
	float px, py, pz;              /* position (24 bytes head) */
	float nx, ny, nz;
	float uv[MDEI_MAX_UV][2];      /* up to 8 UV layers        */
};

/** A static mesh ready for glMultiDrawElementsIndirect.
 *  Created once per unique Blender Mesh* during scene conversion. */
class MDEI_Mesh {
public:
	MDEI_Mesh();
	~MDEI_Mesh();

	/** Upload vertices and indices to the GPU.
	 *  @param uvCount  How many UV layers are filled in MDEI_Vertex::uv[].
	 *  @param uvNames  Layer names parallel to uv[0..uvCount-1].
	 *  @param activeUv Index of the active UV layer (used when name is ""). */
	void Upload(const std::vector<MDEI_Vertex>& verts,
	            const std::vector<unsigned int>& indices,
	            int uvCount,
	            const std::string uvNames[MDEI_MAX_UV],
	            int activeUv);

	/** Bind UV generic attributes using locations from GPUVertexAttribs.
	 *  Must be called while the VAO is bound (glBindVertexArray active). */
	void BindUVAttribs(const GPUVertexAttribs& attribs) const;

	/** Release GL objects. */
	void Release();

	GLuint  GetVAO()        const { return m_vao; }
	GLuint  GetVBO()        const { return m_vbo; }
	GLuint  GetEBO()        const { return m_ebo; }
	GLsizei GetIndexCount() const { return m_indexCount; }

	/** AABB for frustum culling (local-space extents). */
	mt::vec3 m_aabbMin;
	mt::vec3 m_aabbMax;

	/* UV metadata — kept for BindUVAttribs matching */
	int         m_uvCount;
	std::string m_uvNames[MDEI_MAX_UV];
	int         m_activeUv;

private:
	GLuint  m_vao;
	GLuint  m_vbo;
	GLuint  m_ebo;
	GLsizei m_indexCount;

	/* Byte stride and per-UV offsets — computed in Upload(), used in BindUVAttribs() */
	GLsizei m_stride;
	GLintptr m_uvOffset[MDEI_MAX_UV];   /* byte offset of uv[i] inside a vertex */
};

#endif /* __MDEI_MESH_H__ */
