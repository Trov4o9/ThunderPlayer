/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#include "RAS_PersistentInstanceManager.h"
#include <cstdio>
#include <cstring>

extern "C" int USE_PERSISTENT_SSBO_RENDERING;

RAS_PersistentInstanceManager *RAS_PersistentInstanceManager::s_instance = nullptr;

RAS_PersistentInstanceManager::RAS_PersistentInstanceManager()
    : m_instanceSSBO(0),
      m_instancePtr(nullptr),
      m_instanceCapacity(0),
      m_currentRing(0),
      m_instanceSegmentSize(0),
      m_instanceCurrentOffset(0),
      m_indirectBuffer(0),
      m_commandPtr(nullptr),
      m_commandCapacity(0),
      m_initialized(false),
      m_frameCounter(0)
{
    for (int i = 0; i < RING_SIZE; ++i) {
        m_instanceFences[i] = nullptr;
    }
}

RAS_PersistentInstanceManager::~RAS_PersistentInstanceManager()
{
    if (!m_initialized) return;
    
    for (int i = 0; i < RING_SIZE; ++i) {
        if (m_instanceFences[i]) {
            glDeleteSync(m_instanceFences[i]);
        }
    }
    
    if (m_instancePtr) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_instanceSSBO);
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }
    
    if (m_commandPtr) {
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_indirectBuffer);
        glUnmapBuffer(GL_DRAW_INDIRECT_BUFFER);
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    }
    
    if (m_instanceSSBO) glDeleteBuffers(1, &m_instanceSSBO);
    if (m_indirectBuffer) glDeleteBuffers(1, &m_indirectBuffer);
}

RAS_PersistentInstanceManager *RAS_PersistentInstanceManager::GetInstance()
{
    if (!s_instance) {
        s_instance = new RAS_PersistentInstanceManager();
    }
    return s_instance;
}

void RAS_PersistentInstanceManager::Initialize()
{
    if (!USE_PERSISTENT_SSBO_RENDERING) return;
    
    RAS_PersistentInstanceManager *mgr = GetInstance();
    if (mgr->m_initialized) return;
    
    // Create SSBO
    mgr->m_instanceCapacity = 128 * 1024;
    mgr->m_instanceSegmentSize = mgr->m_instanceCapacity * sizeof(InstanceData);
    size_t totalInstanceSize = mgr->m_instanceSegmentSize * RING_SIZE;
    
    glGenBuffers(1, &mgr->m_instanceSSBO);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, mgr->m_instanceSSBO);
    
    GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    glBufferStorage(GL_SHADER_STORAGE_BUFFER, totalInstanceSize, nullptr, flags);
    
    mgr->m_instancePtr = (InstanceData*)glMapBufferRange(
        GL_SHADER_STORAGE_BUFFER, 0, totalInstanceSize, flags);
    
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    
    // Create Indirect Buffer
    mgr->m_commandCapacity = 10 * 1024;
    size_t totalCommandSize = mgr->m_commandCapacity * sizeof(DrawElementsIndirectCommand);
    
    glGenBuffers(1, &mgr->m_indirectBuffer);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, mgr->m_indirectBuffer);
    glBufferStorage(GL_DRAW_INDIRECT_BUFFER, totalCommandSize, nullptr, flags);
    
    mgr->m_commandPtr = (DrawElementsIndirectCommand*)glMapBufferRange(
        GL_DRAW_INDIRECT_BUFFER, 0, totalCommandSize, flags);
    
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    
    mgr->m_initialized = true;
    
    printf("[RAS_PersistentInstanceManager] Initialized:\n");
    printf("  Instance SSBO: %zu MB (%zu instances × %d rings)\n",
           totalInstanceSize / (1024*1024), mgr->m_instanceCapacity, RING_SIZE);
    printf("  Command Buffer: %zu KB (%zu commands)\n",
           totalCommandSize / 1024, mgr->m_commandCapacity);
}

void RAS_PersistentInstanceManager::Cleanup()
{
    if (s_instance) {
        delete s_instance;
        s_instance = nullptr;
    }
}

unsigned long long RAS_PersistentInstanceManager::MakeKey(int displayArrayID, int materialID) const
{
    return ((unsigned long long)displayArrayID << 32) | (unsigned int)materialID;
}

int RAS_PersistentInstanceManager::AllocateInstanceSlot(int meshUserID)
{
    int slotIndex;
    
    if (!m_freeInstanceSlots.empty()) {
        slotIndex = m_freeInstanceSlots.back();
        m_freeInstanceSlots.pop_back();
    } else {
        slotIndex = m_instanceSlots.size();
        m_instanceSlots.push_back(InstanceSlot());
    }
    
    InstanceSlot& slot = m_instanceSlots[slotIndex];
    slot.meshUserID = meshUserID;
    slot.dirty = true;
    slot.ringIndex = m_currentRing;
    slot.lastUpdateFrame = m_frameCounter;
    
    return slotIndex;
}

void RAS_PersistentInstanceManager::FreeInstanceSlot(int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= (int)m_instanceSlots.size()) return;
    
    m_instanceSlots[slotIndex].meshUserID = -1;
    m_instanceSlots[slotIndex].dirty = false;
    m_freeInstanceSlots.push_back(slotIndex);
    
    m_instanceCache.erase(slotIndex);
}

bool RAS_PersistentInstanceManager::IsInstanceSlotValid(int slotIndex) const
{
    return slotIndex >= 0 && slotIndex < (int)m_instanceSlots.size() &&
           m_instanceSlots[slotIndex].meshUserID != -1;
}

void RAS_PersistentInstanceManager::UpdateInstance(
    int slotIndex, const mt::mat4& modelMatrix, const mt::mat4& normalMatrix,
    const mt::vec4& color, unsigned int layer, unsigned int passIndex,
    unsigned int matPassIndex)
{
    if (!IsInstanceSlotValid(slotIndex)) return;
    
    InstanceSlot& slot = m_instanceSlots[slotIndex];
    
    // Check cache
    auto it = m_instanceCache.find(slotIndex);
    if (it != m_instanceCache.end()) {
        const InstanceCache& cached = it->second;
        // Compare using memcmp since mat4 may not have operator==
        if (cached.valid && 
            std::memcmp(&cached.modelMatrix, &modelMatrix, sizeof(mt::mat4)) == 0 &&
            std::memcmp(&cached.normalMatrix, &normalMatrix, sizeof(mt::mat4)) == 0 &&
            std::memcmp(&cached.color, &color, sizeof(mt::vec4)) == 0 &&
            cached.layer == layer) {
            return; // No update needed
        }
    }
    
    // Calculate offset in ring buffer
    size_t offset = (m_instanceSegmentSize / sizeof(InstanceData)) * m_currentRing + slotIndex;
    
    if (offset >= m_instanceCapacity * RING_SIZE) {
        printf("[RAS_PersistentInstanceManager] ERROR: Offset out of bounds!\n");
        return;
    }
    
    InstanceData& data = m_instancePtr[offset];
    
    // Copy modelMatrix (mt::mat4 to float[16])
    std::memcpy(data.modelMatrix, modelMatrix.Data(), sizeof(float) * 16);
    
    // Copy normalMatrix (mt::mat4 to float[16])
    std::memcpy(data.normalMatrix, normalMatrix.Data(), sizeof(float) * 16);
    
    // Copy color (mt::vec4 to float[4])
    std::memcpy(data.color, color.Data(), sizeof(float) * 4);
    
    data.info[0] = layer;
    data.info[1] = passIndex;
    data.info[2] = matPassIndex;
    data.info[3] = 0;
    
    // Update cache
    InstanceCache& cache = m_instanceCache[slotIndex];
    cache.modelMatrix = modelMatrix;
    cache.normalMatrix = normalMatrix;
    cache.color = color;
    cache.layer = layer;
    cache.valid = true;
    
    slot.dirty = false;
    slot.lastUpdateFrame = m_frameCounter;
}

int RAS_PersistentInstanceManager::AllocateCommand(int displayArrayID, int materialID)
{
    int commandIndex;
    
    if (!m_freeCommandSlots.empty()) {
        commandIndex = m_freeCommandSlots.back();
        m_freeCommandSlots.pop_back();
    } else {
        commandIndex = m_commandSlots.size();
        m_commandSlots.push_back(CommandSlot());
    }
    
    CommandSlot& slot = m_commandSlots[commandIndex];
    slot.displayArrayID = displayArrayID;
    slot.materialID = materialID;
    slot.dirty = true;
    slot.active = true;
    slot.instanceSlots.clear();
    
    unsigned long long key = MakeKey(displayArrayID, materialID);
    m_commandLookup[key] = commandIndex;
    
    return commandIndex;
}

void RAS_PersistentInstanceManager::FreeCommand(int commandIndex)
{
    if (commandIndex < 0 || commandIndex >= (int)m_commandSlots.size()) return;
    
    CommandSlot& slot = m_commandSlots[commandIndex];
    unsigned long long key = MakeKey(slot.displayArrayID, slot.materialID);
    m_commandLookup.erase(key);
    
    slot.displayArrayID = -1;
    slot.materialID = -1;
    slot.active = false;
    slot.instanceSlots.clear();
    m_freeCommandSlots.push_back(commandIndex);
}

int RAS_PersistentInstanceManager::FindOrCreateCommand(int displayArrayID, int materialID)
{
    unsigned long long key = MakeKey(displayArrayID, materialID);
    auto it = m_commandLookup.find(key);
    
    if (it != m_commandLookup.end()) {
        return it->second;
    }
    
    return AllocateCommand(displayArrayID, materialID);
}

void RAS_PersistentInstanceManager::UpdateCommand(
    int commandIndex, unsigned int count, unsigned int firstIndex,
    unsigned int baseVertex, const std::vector<int>& instanceSlots)
{
    if (commandIndex < 0 || commandIndex >= (int)m_commandSlots.size()) return;
    
    CommandSlot& slot = m_commandSlots[commandIndex];
    
    // Check if changed
    bool changed = (slot.instanceSlots.size() != instanceSlots.size());
    if (!changed) {
        for (size_t i = 0; i < instanceSlots.size(); ++i) {
            if (slot.instanceSlots[i] != instanceSlots[i]) {
                changed = true;
                break;
            }
        }
    }
    
    if (!changed && !slot.dirty) return;
    
    slot.instanceSlots = instanceSlots;
    
    unsigned int baseInstance = instanceSlots.empty() ? 0 : instanceSlots[0];
    unsigned int instanceCount = instanceSlots.size();
    
    DrawElementsIndirectCommand& cmd = m_commandPtr[commandIndex];
    cmd.count = count;
    cmd.instanceCount = instanceCount;
    cmd.firstIndex = firstIndex;
    cmd.baseVertex = baseVertex;
    cmd.baseInstance = baseInstance;
    
    slot.dirty = false;
}

void RAS_PersistentInstanceManager::MarkCommandDirty(int commandIndex)
{
    if (commandIndex >= 0 && commandIndex < (int)m_commandSlots.size()) {
        m_commandSlots[commandIndex].dirty = true;
    }
}

void RAS_PersistentInstanceManager::BeginFrame()
{
    if (!USE_PERSISTENT_SSBO_RENDERING) return;
    
    m_frameCounter++;
    m_currentRing = m_frameCounter % RING_SIZE;
    
    GLsync& fence = m_instanceFences[m_currentRing];
    if (fence) {
        GLenum result = glClientWaitSync(fence, 0, 0);
        while (result == GL_TIMEOUT_EXPIRED) {
            result = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000);
        }
        glDeleteSync(fence);
        fence = nullptr;
    }
    
    m_instanceCurrentOffset = m_instanceSegmentSize * m_currentRing;
}

void RAS_PersistentInstanceManager::EndFrame()
{
    if (!USE_PERSISTENT_SSBO_RENDERING) return;
    
    GLsync& fence = m_instanceFences[m_currentRing];
    if (fence) {
        glDeleteSync(fence);
    }
    fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

void RAS_PersistentInstanceManager::BindResources()
{
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_instanceSSBO);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_indirectBuffer);
}

void RAS_PersistentInstanceManager::UnbindResources()
{
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, 0);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
}

void RAS_PersistentInstanceManager::ExecuteIndirectDraw(int commandIndex)
{
    if (commandIndex < 0 || commandIndex >= (int)m_commandSlots.size()) return;
    
    const CommandSlot& slot = m_commandSlots[commandIndex];
    if (!slot.active || slot.instanceSlots.empty()) return;
    
    size_t offset = commandIndex * sizeof(DrawElementsIndirectCommand);
    glDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, (const void*)offset);
}
