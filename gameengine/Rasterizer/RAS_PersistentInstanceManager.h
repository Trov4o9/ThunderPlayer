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

#ifndef __RAS_PERSISTENT_INSTANCE_MANAGER_H__
#define __RAS_PERSISTENT_INSTANCE_MANAGER_H__

#include "glew-mx.h"
#include <vector>
#include <unordered_map>
#include "mathfu.h"

struct DrawElementsIndirectCommand {
    unsigned int count;
    unsigned int instanceCount;
    unsigned int firstIndex;
    unsigned int baseVertex;
    unsigned int baseInstance;
};

struct InstanceData {
    float modelMatrix[16];      // mat4 = 16 floats
    float normalMatrix[16];     // mat4 = 16 floats
    float color[4];             // vec4 = 4 floats
    unsigned int info[4];       // uvec4 = 4 uints
};

class RAS_PersistentInstanceManager {
private:
    static RAS_PersistentInstanceManager *s_instance;
    
    // SSBO Persistente
    GLuint m_instanceSSBO;
    InstanceData *m_instancePtr;
    size_t m_instanceCapacity;
    
    // Ring buffer
    static const int RING_SIZE = 3;
    int m_currentRing;
    GLsync m_instanceFences[RING_SIZE];
    size_t m_instanceSegmentSize;
    size_t m_instanceCurrentOffset;
    
    // Gerenciamento de slots
    struct InstanceSlot {
        int meshUserID;
        bool dirty;
        int ringIndex;
        unsigned long long lastUpdateFrame;
    };
    std::vector<InstanceSlot> m_instanceSlots;
    std::vector<int> m_freeInstanceSlots;
    
    // Cache
    struct InstanceCache {
        mt::mat4 modelMatrix;
        mt::mat4 normalMatrix;
        mt::vec4 color;
        unsigned int layer;
        bool valid;
    };
    std::unordered_map<int, InstanceCache> m_instanceCache;
    
    // Indirect Command Buffer
    GLuint m_indirectBuffer;
    DrawElementsIndirectCommand *m_commandPtr;
    size_t m_commandCapacity;
    
    // Comandos
    struct CommandSlot {
        int displayArrayID;
        int materialID;
        bool dirty;
        bool active;
        std::vector<int> instanceSlots;
    };
    std::vector<CommandSlot> m_commandSlots;
    std::vector<int> m_freeCommandSlots;
    std::unordered_map<unsigned long long, int> m_commandLookup;
    
    bool m_initialized;
    unsigned long long m_frameCounter;
    
    RAS_PersistentInstanceManager();
    ~RAS_PersistentInstanceManager();
    
    unsigned long long MakeKey(int displayArrayID, int materialID) const;

public:
    static RAS_PersistentInstanceManager *GetInstance();
    static void Initialize();
    static void Cleanup();
    
    // Instance management
    int AllocateInstanceSlot(int meshUserID);
    void FreeInstanceSlot(int slotIndex);
    bool IsInstanceSlotValid(int slotIndex) const;
    void UpdateInstance(int slotIndex, const mt::mat4& modelMatrix,
                       const mt::mat4& normalMatrix, const mt::vec4& color,
                       unsigned int layer, unsigned int passIndex,
                       unsigned int matPassIndex);
    
    // Command management
    int AllocateCommand(int displayArrayID, int materialID);
    void FreeCommand(int commandIndex);
    int FindOrCreateCommand(int displayArrayID, int materialID);
    void UpdateCommand(int commandIndex, unsigned int count, 
                      unsigned int firstIndex, unsigned int baseVertex,
                      const std::vector<int>& instanceSlots);
    void MarkCommandDirty(int commandIndex);
    
    // Frame management
    void BeginFrame();
    void EndFrame();
    unsigned long long GetFrameCounter() const { return m_frameCounter; }
    
    // Binding
    void BindResources();
    void UnbindResources();
    GLuint GetSSBO() const { return m_instanceSSBO; }
    GLuint GetIndirectBuffer() const { return m_indirectBuffer; }
    
    // Draw execution
    void ExecuteIndirectDraw(int commandIndex);
    bool IsInitialized() const { return m_initialized; }
};

#endif  // __RAS_PERSISTENT_INSTANCE_MANAGER_H__
