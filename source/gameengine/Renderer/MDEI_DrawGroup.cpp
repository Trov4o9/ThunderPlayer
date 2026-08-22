/*
 * MDEI_DrawGroup.cpp
 *
 * Pooled group (IsPooled() == true):
 *   Instâncias são agrupadas por slot (mesh). Para cada slot único, todas as
 *   instâncias são escritas de forma contígua no SSBO e um único
 *   DrawElementsIndirectCommand é emitido com instanceCount = N.
 *   Resultado: 1 comando por (shader × mesh) distinto, independente do
 *   número de instâncias — mantém o buffer de comandos pequeno mesmo com
 *   milhares de addObject().
 *
 * Private group (IsPooled() == false):
 *   Comportamento legado: um único comando para todas as instâncias do grupo.
 *   Usado para objetos com armature e EnsurePrivateMesh (procedural).
 */

#include "MDEI_DrawGroup.h"
#include "MDEI_Mesh.h"
#include "MDEI_GeometryPool.h"

#include <cstring>
#include <vector>

/* ── Constructors / Destructor ────────────────────────────────────────────── */

MDEI_DrawGroup::MDEI_DrawGroup(MDEI_Shader *shader)
    : m_shader(shader)
    , m_pool(new MDEI_GeometryPool())
    , m_privateMesh(nullptr)
{
}

MDEI_DrawGroup::MDEI_DrawGroup(MDEI_Mesh *mesh, MDEI_Shader *shader)
    : m_shader(shader)
    , m_pool(nullptr)
    , m_privateMesh(mesh)
{
}

MDEI_DrawGroup::~MDEI_DrawGroup()
{
    delete m_pool; /* nullptr-safe */
    /* m_privateMesh is NOT owned by the group — caller manages lifetime */
}

/* ── GetCurrentVAO ────────────────────────────────────────────────────────── */

GLuint MDEI_DrawGroup::GetCurrentVAO() const
{
    if (m_pool)         return m_pool->GetVAO();
    if (m_privateMesh)  return m_privateMesh->GetCurrentVAO();
    return 0;
}

/* ── GetPrivateIndexCount ─────────────────────────────────────────────────── */

GLuint MDEI_DrawGroup::GetPrivateIndexCount() const
{
    if (m_privateMesh) return (GLuint)m_privateMesh->GetIndexCount();
    return 0;
}

/* ── AddInstance ──────────────────────────────────────────────────────────── */

void MDEI_DrawGroup::AddInstance(const float matrix[16], const float color[4],
                                  MDEI_MeshSlot slot)
{
    RawInstance inst;
    memcpy(inst.matrix, matrix, sizeof(inst.matrix));
    memcpy(inst.color,  color,  sizeof(inst.color));
    inst.slot = slot;
    m_pendingInstances.push_back(inst);
}

/* ── WriteInstancesAndCommand ─────────────────────────────────────────────── */

void MDEI_DrawGroup::WriteInstancesAndCommand(
    MDEI_Instance *dest,
    unsigned int   baseInst,
    std::vector<DrawElementsIndirectCommand>& cmds)
{
    const unsigned int count = (unsigned int)m_pendingInstances.size();
    if (count == 0) return;

    if (m_pool) {
        /* ── Pooled path ──────────────────────────────────────────────── *
         *                                                                 *
         * Instâncias são agrupadas por slot. Para cada slot único,       *
         * escrevemos TODAS as suas instâncias de forma contígua no SSBO  *
         * e geramos UM único DrawElementsIndirectCommand com             *
         * instanceCount = N (número de instâncias daquele slot).         *
         *                                                                 *
         * Algoritmo: dois passos                                          *
         *   1. Varrer m_pendingInstances e contar instâncias por slot    *
         *   2. Escrever no SSBO e gerar comandos por slot                *
         *                                                                 *
         * Para não alocar memória dinâmica no hot path, usamos dois      *
         * laços lineares simples. O número de slots distintos é pequeno  *
         * na prática (geralmente 1–4 por grupo pooled).                  *
         * ─────────────────────────────────────────────────────────────── */

        /* Passo 1: reordenar logicamente — acumular instâncias por slot  *
         * Usamos a ordem em que os slots aparecem pela primeira vez para  *
         * preservar o comportamento determinístico entre frames.          */

        /* Lista de slots únicos na ordem de aparição */
        std::vector<MDEI_MeshSlot> slotOrder;
        slotOrder.reserve(8);

        for (unsigned int i = 0; i < count; i++) {
            const MDEI_MeshSlot s = m_pendingInstances[i].slot;
            if (s == MDEI_SLOT_INVALID) continue;
            bool found = false;
            for (MDEI_MeshSlot us : slotOrder) {
                if (us == s) { found = true; break; }
            }
            if (!found) slotOrder.push_back(s);
        }

        /* Passo 2: para cada slot único, escrever instâncias e emitir cmd */
        unsigned int ssboOffset = 0; /* offset dentro do bloco deste grupo */

        for (MDEI_MeshSlot targetSlot : slotOrder) {
            const MDEI_PoolMeshInfo &info = m_pool->GetInfo(targetSlot);
            if (info.indexCount == 0) continue;

            /* Contar e escrever instâncias deste slot de forma contígua */
            unsigned int slotCount = 0;
            for (unsigned int i = 0; i < count; i++) {
                if (m_pendingInstances[i].slot != targetSlot) continue;
                MDEI_Instance &dst = dest[ssboOffset + slotCount];
                memcpy(dst.matrix, m_pendingInstances[i].matrix, 64);
                memcpy(dst.color,  m_pendingInstances[i].color,  16);
                slotCount++;
            }

            if (slotCount == 0) continue;

            /* Um único comando cobre todas as instâncias deste slot */
            DrawElementsIndirectCommand cmd;
            cmd.count         = info.indexCount;
            cmd.instanceCount = slotCount;
            cmd.firstIndex    = info.firstIndex;
            cmd.baseVertex    = info.baseVertex;
            cmd.baseInstance  = baseInst + ssboOffset;
            cmds.push_back(cmd);

            ssboOffset += slotCount;
        }
    }
    else {
        /* ── Private (skinned / EnsurePrivate) path ──────────────────── *
         * Um único comando para todas as instâncias — comportamento       *
         * legado inalterado.                                              */
        if (!m_privateMesh ||
            m_privateMesh->GetIndexCount() == 0 ||
            m_privateMesh->GetCurrentVAO() == 0)
            return;

        /* Escrever instâncias no SSBO */
        for (unsigned int i = 0; i < count; i++) {
            memcpy(dest[i].matrix, m_pendingInstances[i].matrix, 64);
            memcpy(dest[i].color,  m_pendingInstances[i].color,  16);
        }

        DrawElementsIndirectCommand cmd;
        cmd.count         = (GLuint)m_privateMesh->GetIndexCount();
        cmd.instanceCount = count;
        cmd.firstIndex    = 0;
        cmd.baseVertex    = 0;
        cmd.baseInstance  = baseInst;
        cmds.push_back(cmd);
    }
}
