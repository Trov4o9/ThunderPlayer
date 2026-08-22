/*
 * MDEI_GeometryPool.h — Shared VBO + EBO pool for one MDEI_Shader.
 *
 * All static meshes that share the same shader upload their geometry into a
 * single VBO and a single EBO.  This allows all draw commands for a shader
 * to share one VAO, enabling a TRUE single glMultiDrawElementsIndirect call
 * per shader regardless of how many different meshes are used.
 *
 * Layout
 * ------
 * VBO:  [ mesh0_verts | mesh1_verts | mesh2_verts | ... ]
 * EBO:  [ mesh0_inds  | mesh1_inds  | mesh2_inds  | ... ]
 *
 * Each slot stores:
 *   firstIndex  = byte offset in EBO / sizeof(uint)
 *   baseVertex  = first vertex index in VBO for this mesh
 *   indexCount  = number of indices
 *
 * Skinned meshes are NOT pooled (they have their own ring VBOs and unique
 * poses); they keep individual MDEI_Mesh objects and are drawn separately.
 *
 * Memory management
 * -----------------
 * On Allocate() the pool grows the CPU-side staging buffers and marks itself
 * dirty.  On UploadIfDirty() the entire concatenated buffer is re-uploaded to
 * the GPU with glBufferData (GL_DYNAMIC_DRAW).  This is the same upload path
 * as the current ring-buffer: small enough that a full re-upload is cheap.
 *
 * Free() marks a slot as unused (ref-count reaches zero).  The slot's memory
 * is compacted on the next UploadIfDirty() call.  Between Free() and the next
 * upload the slot still occupies GPU memory but will never be drawn (no
 * commands reference it).
 */

#ifndef __MDEI_GEOMETRY_POOL_H__
#define __MDEI_GEOMETRY_POOL_H__

#include "GPU_glew.h"
#include "MDEI_Mesh.h"   /* MDEI_Vertex, MDEI_MAX_UV */

#include <vector>
#include <string>

/* Forward declaration */
class MDEI_Shader;

/** Handle returned by Allocate(); used to reference a mesh slice. */
using MDEI_MeshSlot = int; /* index into m_slots, -1 = invalid */
static const MDEI_MeshSlot MDEI_SLOT_INVALID = -1;

/** Geometry info for one mesh allocated in the pool. */
struct MDEI_PoolMeshInfo {
    GLuint  firstIndex;  /* first index in the shared EBO (element count, not bytes) */
    GLint   baseVertex;  /* first vertex in the shared VBO for this mesh             */
    GLuint  indexCount;  /* number of indices                                        */
    int     refCount;    /* number of objects using this slot                        */
    bool    active;      /* false = freed, will be compacted                         */

    /* AABB (local space) — forwarded from the source MDEI_Mesh */
    float aabbMin[3];
    float aabbMax[3];

    /* UV metadata — needed so BindUVAttribs works for the shared VAO */
    int         uvCount;
    std::string uvNames[MDEI_MAX_UV];
    int         activeUv;
    GLsizei     stride;
    GLintptr    uvOffset[MDEI_MAX_UV];
};

class MDEI_GeometryPool {
public:
    MDEI_GeometryPool();
    ~MDEI_GeometryPool();

    /** Allocate a slot for a static (non-skinned) MDEI_Mesh.
     *  Copies vertex/index data into the CPU staging buffers and marks dirty.
     *  Returns MDEI_SLOT_INVALID if mesh is invalid or has no geometry. */
    MDEI_MeshSlot Allocate(const MDEI_Mesh *mesh);

    /** Decrement ref-count; if it reaches zero the slot is freed.
     *  The VAO/VBO/EBO are NOT shrunk immediately — compaction happens on
     *  the next UploadIfDirty() call after a free. */
    void Free(MDEI_MeshSlot slot);

    /** Increment ref-count (called when a second object shares the same slot). */
    void AddRef(MDEI_MeshSlot slot);

    /** Re-upload the concatenated VBO + EBO if the pool was modified since the
     *  last upload.  Rebuilds the VAO.  Should be called once per frame before
     *  draw (or lazily when a new object is registered). */
    void UploadIfDirty();

    /** Returns info for a slot (asserts slot is valid). */
    const MDEI_PoolMeshInfo& GetInfo(MDEI_MeshSlot slot) const;

    /** The single VAO that covers the entire shared VBO + EBO. */
    GLuint GetVAO() const { return m_vao; }

    /** Bind UV generic attributes for the current shader's GPUVertexAttribs
     *  for a specific slot.  Must be called with VAO already bound. */
    void BindUVAttribs(MDEI_MeshSlot slot, const GPUVertexAttribs& attribs) const;

    /** Release all GL objects. */
    void Shutdown();

    /** True if the pool has at least one active (non-freed) slot. */
    bool HasGeometry() const;

private:
    void Compact();   /* removes freed slots and rebuilds staging buffers */
    void Rebuild();   /* re-uploads VBO+EBO and rebuilds VAO              */

    struct Slot {
        MDEI_PoolMeshInfo info;
        /* CPU copy of vertex floats for this mesh (packed: pos+nor+uvs) */
        std::vector<float>        verts;   /* packed floats               */
        std::vector<unsigned int> inds;    /* original indices (pre-base) */
    };

    std::vector<Slot>  m_slots;
    bool               m_dirty;       /* needs Rebuild()                 */

    /* Concatenated CPU staging buffers (rebuilt by Compact) */
    std::vector<float>        m_allVerts;
    std::vector<unsigned int> m_allInds;

    /* Shared stride — all meshes in the pool must have the same stride
     * (same number of UV layers).  If a new mesh has a different stride
     * we force a full repack. */
    GLsizei  m_stride;   /* 0 = pool is empty */

    GLuint   m_vbo;
    GLuint   m_ebo;
    GLuint   m_vao;
};

#endif /* __MDEI_GEOMETRY_POOL_H__ */
