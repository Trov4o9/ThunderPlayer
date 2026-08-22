/*
 * MDEI_GeometryPool.cpp
 *
 * See MDEI_GeometryPool.h for the design overview.
 *
 * Key invariant
 * -------------
 * All slots in the pool share the same vertex stride (same UV layout).
 * If a new mesh has fewer UV layers we pad with zeros;
 * if it has MORE UV layers the pool is considered incompatible and the mesh
 * falls back to its own MDEI_Mesh (this case is extremely rare in practice).
 *
 * Compaction
 * ----------
 * When a slot is freed (ref-count → 0) we do NOT immediately patch the GPU
 * buffers.  Instead m_dirty is set and on the next UploadIfDirty() the pool
 * is fully repacked from remaining active slots.  This is O(total geometry)
 * but happens rarely (only on endObject for the LAST user of a mesh).
 */

#include "MDEI_GeometryPool.h"

#include <cstring>
#include <cstdio>
#include <cassert>

/* CD_ type codes for BindUVAttribs */
#include "DNA_customdata_types.h"

/* ── ctor / dtor ──────────────────────────────────────────────────────────── */

MDEI_GeometryPool::MDEI_GeometryPool()
    : m_dirty(false), m_stride(0)
    , m_vbo(0), m_ebo(0), m_vao(0)
{
}

MDEI_GeometryPool::~MDEI_GeometryPool()
{
    Shutdown();
}

/* ── Shutdown ─────────────────────────────────────────────────────────────── */

void MDEI_GeometryPool::Shutdown()
{
    if (m_vao) { glDeleteVertexArrays(1, &m_vao); m_vao = 0; }
    if (m_vbo) { glDeleteBuffers(1, &m_vbo);      m_vbo = 0; }
    if (m_ebo) { glDeleteBuffers(1, &m_ebo);      m_ebo = 0; }

    m_slots.clear();
    m_allVerts.clear();
    m_allInds.clear();
    m_stride = 0;
    m_dirty  = false;
}

/* ── Allocate ─────────────────────────────────────────────────────────────── */

MDEI_MeshSlot MDEI_GeometryPool::Allocate(const MDEI_Mesh *mesh)
{
    if (!mesh || mesh->GetIndexCount() == 0) return MDEI_SLOT_INVALID;

    /* ── Retrieve the packed CPU vertex data from the mesh.
     * MDEI_Mesh stores a CPU copy in m_cpuVerts (positions only).
     * We need the full packed layout (pos+nor+uvs) which is NOT stored by
     * MDEI_Mesh after Upload().  To reconstruct it we must rebuild.
     *
     * Design choice: MDEI_Mesh::GetPackedVerts() provides a const ref to
     * the packed float vector.  We add that accessor — see header change.
     * Until then we use the available public accessors to pull the data. */

    const std::vector<float>&        packedVerts = mesh->GetPackedVerts();
    const std::vector<unsigned int>& indices     = mesh->GetCpuInds();

    if (packedVerts.empty() || indices.empty()) return MDEI_SLOT_INVALID;

    const GLsizei meshStride = mesh->GetStride();

    /* If the pool is empty, adopt this mesh's stride */
    if (m_stride == 0) m_stride = meshStride;

    /* Incompatible stride: caller falls back to private group. */
    if (meshStride != m_stride)
        return MDEI_SLOT_INVALID;

    /* Find a free slot to reuse */
    int slotIdx = MDEI_SLOT_INVALID;
    for (int i = 0; i < (int)m_slots.size(); i++) {
        if (!m_slots[i].info.active) { slotIdx = i; break; }
    }

    if (slotIdx == MDEI_SLOT_INVALID) {
        m_slots.emplace_back();
        slotIdx = (int)m_slots.size() - 1;
    }

    Slot &s = m_slots[slotIdx];
    s.verts = packedVerts;
    s.inds  = indices;

    /* Fill MeshInfo — offsets will be patched during Compact/Rebuild */
    MDEI_PoolMeshInfo &info = s.info;
    info.firstIndex  = 0;
    info.baseVertex  = 0;
    info.indexCount  = (GLuint)indices.size();
    info.refCount    = 1;
    info.active      = true;

    info.aabbMin[0] = mesh->m_aabbMin[0]; info.aabbMin[1] = mesh->m_aabbMin[1]; info.aabbMin[2] = mesh->m_aabbMin[2];
    info.aabbMax[0] = mesh->m_aabbMax[0]; info.aabbMax[1] = mesh->m_aabbMax[1]; info.aabbMax[2] = mesh->m_aabbMax[2];

    info.uvCount  = mesh->GetUVCount();
    info.activeUv = mesh->GetActiveUV();
    info.stride   = meshStride;
    for (int i = 0; i < MDEI_MAX_UV; i++) {
        info.uvNames[i]  = mesh->GetUVName(i);
        info.uvOffset[i] = mesh->GetUVOffset(i);
    }

    m_dirty = true;
    return slotIdx;
}

/* ── AddRef / Free ────────────────────────────────────────────────────────── */

void MDEI_GeometryPool::AddRef(MDEI_MeshSlot slot)
{
    if (slot < 0 || slot >= (int)m_slots.size()) return;
    if (m_slots[slot].info.active)
        m_slots[slot].info.refCount++;
}

void MDEI_GeometryPool::Free(MDEI_MeshSlot slot)
{
    if (slot < 0 || slot >= (int)m_slots.size()) return;
    MDEI_PoolMeshInfo &info = m_slots[slot].info;
    if (!info.active) return;

    info.refCount--;
    if (info.refCount <= 0) {
        info.active   = false;
        info.refCount = 0;
        m_dirty       = true;
    }
}

/* ── GetInfo ──────────────────────────────────────────────────────────────── */

const MDEI_PoolMeshInfo& MDEI_GeometryPool::GetInfo(MDEI_MeshSlot slot) const
{
    assert(slot >= 0 && slot < (int)m_slots.size());
    return m_slots[slot].info;
}

/* ── HasGeometry ──────────────────────────────────────────────────────────── */

bool MDEI_GeometryPool::HasGeometry() const
{
    for (const Slot &s : m_slots)
        if (s.info.active && s.info.indexCount > 0) return true;
    return false;
}

/* ── UploadIfDirty ────────────────────────────────────────────────────────── */

void MDEI_GeometryPool::UploadIfDirty()
{
    if (!m_dirty) return;
    Compact();
    Rebuild();
    m_dirty = false;
}

/* ── Compact ──────────────────────────────────────────────────────────────── */

void MDEI_GeometryPool::Compact()
{
    m_allVerts.clear();
    m_allInds.clear();

    if (m_stride == 0) return;

    const int floatsPerVert = m_stride / (int)sizeof(float);

    GLuint vertexCursor = 0;  /* running vertex count  */
    GLuint indexCursor  = 0;  /* running index count   */

    for (Slot &s : m_slots) {
        if (!s.info.active) continue;

        MDEI_PoolMeshInfo &info = s.info;

        /* Patch offsets */
        info.baseVertex = (GLint)vertexCursor;
        info.firstIndex = indexCursor;

        /* Append packed verts */
        m_allVerts.insert(m_allVerts.end(), s.verts.begin(), s.verts.end());

        /* Append indices — they are already relative to vertex 0 of this
         * mesh; baseVertex in the draw command accounts for the shift. */
        m_allInds.insert(m_allInds.end(), s.inds.begin(), s.inds.end());

        vertexCursor += (GLuint)(s.verts.size() / (size_t)floatsPerVert);
        indexCursor  += (GLuint)s.inds.size();
    }
}

/* ── Rebuild ──────────────────────────────────────────────────────────────── */

void MDEI_GeometryPool::Rebuild()
{
    if (m_allVerts.empty() || m_stride == 0) return;

    const GLsizeiptr vertBytes = (GLsizeiptr)(m_allVerts.size() * sizeof(float));
    const GLsizeiptr indBytes  = (GLsizeiptr)(m_allInds.size()  * sizeof(unsigned int));

    /* ── VBO ── */
    if (!m_vbo) glGenBuffers(1, &m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, vertBytes, m_allVerts.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    /* ── EBO ── */
    if (!m_ebo) glGenBuffers(1, &m_ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indBytes, m_allInds.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    /* ── VAO ── */
    if (m_vao) { glDeleteVertexArrays(1, &m_vao); m_vao = 0; }
    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);

    /* Fixed-pipeline pos + normal */
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, m_stride, (const void *)(GLintptr)0);

    glEnableClientState(GL_NORMAL_ARRAY);
    glNormalPointer(GL_FLOAT, m_stride, (const void *)(GLintptr)12);

    /* EBO */
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);

    glBindVertexArray(0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

/* ── BindUVAttribs ────────────────────────────────────────────────────────── */

void MDEI_GeometryPool::BindUVAttribs(MDEI_MeshSlot slot,
                                       const GPUVertexAttribs& attribs) const
{
    if (slot < 0 || slot >= (int)m_slots.size()) return;

    const MDEI_PoolMeshInfo &info = m_slots[slot].info;

    /* segOffset = 0 (pool VBO is not a ring VBO — not skinned) */
    const GLintptr segOffset = (GLintptr)0;

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);

    for (int i = 0; i < attribs.totlayer; i++) {
        const int type    = attribs.layer[i].type;
        const int glindex = attribs.layer[i].glindex;
        if (glindex < 0) continue;

        if (type == CD_MTFACE) {
            const char *layerName = attribs.layer[i].name;
            int uvSlot = -1;

            if (!layerName[0]) {
                uvSlot = (info.activeUv < info.uvCount) ? info.activeUv : 0;
            }
            else {
                for (int j = 0; j < info.uvCount; j++) {
                    if (info.uvNames[j] == layerName) { uvSlot = j; break; }
                }
            }
            if (uvSlot < 0 || uvSlot >= info.uvCount)
                uvSlot = (info.uvCount > 0) ? 0 : -1;

            if (uvSlot >= 0) {
                glEnableVertexAttribArray((GLuint)glindex);
                glVertexAttribPointer(
                    (GLuint)glindex, 2, GL_FLOAT, GL_FALSE,
                    info.stride,
                    (const void *)(segOffset + info.uvOffset[uvSlot]));
            }
        }
        else if (type == CD_ORCO) {
            glEnableVertexAttribArray((GLuint)glindex);
            glVertexAttribPointer(
                (GLuint)glindex, 3, GL_FLOAT, GL_FALSE,
                info.stride, (const void *)(segOffset + 0));
        }
        else if (type == CD_NORMAL) {
            glEnableVertexAttribArray((GLuint)glindex);
            glVertexAttribPointer(
                (GLuint)glindex, 3, GL_FLOAT, GL_FALSE,
                info.stride, (const void *)(segOffset + 12));
        }
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
}
