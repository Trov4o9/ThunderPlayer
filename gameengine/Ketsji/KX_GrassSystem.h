#ifndef __KX_GRASS_SYSTEM_H__
#define __KX_GRASS_SYSTEM_H__

/**
 * KX_GrassSystem — sistema global de grama GPU-instanced.
 *
 * Design:
 *   - Singleton por cena (owned by KX_Scene).
 *   - RegisterTerrain() gera instâncias/chunks do terreno e faz upload
 *     incremental para o VBO global.
 *   - Shader, VAO e VBO são criados uma única vez e o VBO cresce conforme necessário.
 *   - Draw() itera terrenos e seus chunks visíveis.
 *   - setGrassParams() muda parâmetros globais (uniforms re-enviados no
 *     próximo Draw).
 */

#include <vector>
#include <string>
#include <cstdint>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <ankerl/unordered_dense.h>
#include "mathfu.h"

#if defined(_WIN32) || defined(_MSC_VER)
#  include <malloc.h>   // _aligned_malloc / _aligned_free
#endif

// ============================================================================
// KX_GRASS_DENSITY_SHADER_MERGE
//   Quando ativo, até 3 gramas são fundidas em uma única instância GPU.
//   O vertex shader usa gl_VertexID para selecionar a grama dentro da instância
//   (vértices 0-5 = grama1, 6-11 = grama2, 12-17 = grama3).
//   Cada instância emite 18 vértices (3 gramas × 2 triângulos × 3 vértices).
//   O total de gramas desenhadas permanece idêntico; apenas o número de
//   instâncias e o overhead de front-end da GPU são reduzidos.
// ============================================================================
#define KX_GRASS_DENSITY_SHADER_MERGE 1

class KX_GameObject;
class KX_Scene;
class KX_Camera;
struct GPUShader;
class RAS_Texture;
class RAS_Rasterizer;

// ============================================================================
// KX_GRASS_DENSITY_SHADER_MERGE
//   Quando ativo, até 3 gramas são fundidas em uma única instância GPU.
//   O vertex shader usa gl_VertexID para selecionar a grama dentro da instância
//   (vértices 0-5 = grama1, 6-11 = grama2, 12-17 = grama3).
//   Cada instância emite 18 vértices (3 gramas × 2 triângulos × 3 vértices).
//   O total de gramas desenhadas permanece idêntico; apenas o número de
//   instâncias e o overhead de front-end da GPU são reduzidos.
// ============================================================================
#define KX_GRASS_DENSITY_SHADER_MERGE 1

// ============================================================================
// Per-instance data
// ============================================================================
#if KX_GRASS_DENSITY_SHADER_MERGE
// 64 bytes: posição de até 4 gramas fundidas + dados compartilhados da instância
struct GrassInstance {
    float   worldX,  worldY,  worldZ;  // grama 1 — posição world  (12 bytes)
    float   worldX2, worldY2, worldZ2; // grama 2 — posição world  (12 bytes)
    float   worldX3, worldY3, worldZ3; // grama 3 — posição world  (12 bytes)
    float   worldX4, worldY4, worldZ4; // grama 4 — posição world  (12 bytes)
    float   bladeLen;                  //  4 bytes (blen pré-calculado na CPU)
    uint8_t random;                    //  1 byte  [0,255]→[0,1]
    int8_t  sinA;                      //  1 byte  [-127,127]→[-1,1]
    int8_t  cosA;                      //  1 byte  [-127,127]→[-1,1]
    int8_t  tiltX;                     //  1 byte  tilt X compactada
    int8_t  tiltY;                     //  1 byte  tilt Y compactada
    int8_t  tiltZ;                     //  1 byte  tilt Z compactada
    int8_t  _pad[2];                   //  2 bytes alinhamento
    uint32_t selectionKey;
};
static_assert(sizeof(GrassInstance) == 64, "GrassInstance must be 64 bytes (merge mode)");
#else
// 28 bytes: modo original, uma grama por instância
struct GrassInstance {
    float   worldX, worldY, worldZ; // 12 bytes
    float   bladeLen;               //  4 bytes
    uint8_t random;                 //  1 byte
    int8_t  sinA;                   //  1 byte
    int8_t  cosA;                   //  1 byte
    int8_t  tiltX;                  //  1 byte
    int8_t  tiltY;                  //  1 byte
    int8_t  tiltZ;                  //  1 byte
    int8_t  _pad[2];                //  2 bytes
    uint32_t selectionKey;
};
static_assert(sizeof(GrassInstance) == 28, "GrassInstance must be 28 bytes");
#endif

struct GrassPackedNormal {
    int8_t x;
    int8_t y;
    int8_t z;
};

// ============================================================================
// SubChunk — subdivisão de chunk para culling fino (1/4 do tamanho)
// ============================================================================
struct GrassSubChunk {
    // AABB em world space
    float minX, minY, minZ;
    float maxX, maxY, maxZ;
    // Faixa no VBO global (relativo ao chunk pai)
    int   baseInstance;
    int   instanceCount;
    // Cache para culling
    mutable unsigned int drawInstanceCount = 0;
    mutable unsigned int drawBaseInstance = 0;
};

// ============================================================================
// Chunk virtual — grupo espacial de instâncias para culling
// ============================================================================
struct GrassChunk {
    // AABB em world space
    float minX, minY, minZ;
    float maxX, maxY, maxZ;
    float baseMaxZ;
    int32_t ccx;
    int32_t ccy;
    // Faixa no VBO global (baseInstance é relativo ao TerrainGrassData::vboOffset)
    int   baseInstance;
    int   instanceCount;
    unsigned int drawBaseInstance = 0;
    uint32_t drawOffset = 0;
    // Cache para Histerese de LOD (evita oscilação de densidade)
    mutable unsigned int lastLODCount;
    mutable unsigned int drawInstanceCount = 0;
    mutable uint32_t bladeLenVersion = 0;
    
    // Sub-chunks para culling fino (4 subdivisões: 2x2 em XY)
    int firstSubChunk = -1;  // Índice do primeiro sub-chunk no array de sub-chunks
    int subChunkCount = 0;   // Número de sub-chunks (geralmente 4, mas pode variar)
};

// ============================================================================
// MegaChunk — Culling hierárquico
// ============================================================================
struct GrassMegaChunk {
    float minX, minY, minZ;
    float maxX, maxY, maxZ;
    int firstChunk; // Índice do primeiro chunk contíguo em TerrainGrassData::chunks
    int chunkCount; // Número de chunks neste MegaChunk
};

struct GrassChunkLodSoA {
    int chunkCount = 0;

    std::vector<float> minX, minY, minZ;
    std::vector<float> maxX, maxY, maxZ;
    std::vector<float> centerX, centerY;

    std::vector<uint32_t> instanceCount;
    std::vector<uint32_t> lastLODCount;
    std::vector<uint32_t> drawInstanceCount;
    std::vector<uint32_t> drawBaseInstance;

    void Clear();
    void Reserve(size_t logicalChunkCount);
    void PushChunk(float inMinX, float inMinY, float inMinZ,
                   float inMaxX, float inMaxY, float inMaxZ,
                   float inCenterX, float inCenterY,
                   uint32_t inInstanceCount, uint32_t inLastLODCount,
                   uint32_t inDrawInstanceCount, uint32_t inDrawBaseInstance);
    void Finalize(int logicalChunkCount);
};

// ============================================================================
// Estruturas para carregamento incremental
// ============================================================================
struct GrassChunkBuilder {
    std::vector<GrassInstance> instances;
    float minZ =  1e9f;
    float maxZ = -1e9f;
};

// Estrutura para glMultiDrawArraysIndirect
struct DrawArraysIndirectCommand {
    unsigned int count;         // Número de vértices (3 para a blade)
    unsigned int instanceCount; // Número de instâncias do chunk
    unsigned int first;         // Offset do primeiro vértice (sempre 0)
    unsigned int baseInstance;  // Offset global no VBO de instâncias
};

struct TerrainGrassData {
    KX_GameObject* terrain;
    uint64_t terrainToken = 0;
    uint64_t buildEpoch = 0;
    std::vector<GrassChunk> chunks;
    GrassChunkLodSoA lodSoA;
    std::vector<GrassMegaChunk> megaChunks; // Culling hierárquico
    std::vector<GrassSubChunk> subChunks;   // Sub-chunks para culling fino
    std::vector<GrassPackedNormal> baseNormals;
    unsigned int normalSSBO = 0;
    int vboOffset;      // Índice de início no VBO global de instâncias
    int instanceCount;  // Total de instâncias deste terreno
    uint32_t tiltVersion = 0;
};

// Gerenciamento de memória GPU
struct GrassFreeBlock {
    int offset;
    int size;
};

// ============================================================================
// Parâmetros globais do sistema
// ============================================================================
struct GrassParams {
    float scale       = 1.0f;
    float lengthMax   = 1.3f;
    float lengthMin   = 0.8f;
    float heightScale = 2.0f;
    float distance    = 250000.0f;
    float strength    = 1.0f;
    float speed       = 1.0f;   // dividido por 100 internamente
    float range       = 1.0f;   // dividido por 100 internamente
    float flatness    = 0.65f;
    int   density     = 10;
    // LOD independente para vento e luz; -1 = usar cálculo automático baseado em distance
    float windLod     = -1.0f;
    float lightLod    = -1.0f;
    // lodStart: distância de início do fade de densidade; -1 = automático (50% de distance)
    float lodStart    = -1.0f;
};

// ============================================================================
// Uniforms de luz cacheados (usados apenas para fallback sem UBO)
// ============================================================================
struct GrassLightUniforms {
    int type        = -1;
    int diffuse     = -1;
    int position    = -1;
    int spotDir     = -1;
    int params      = -1; // x: invDist, y: cos(cutoff), z: exponent/5.0
};

// ============================================================================
// Layout do UBO de luz+timer+uniforms+handles bindless (std140)
// Binding point 0 — atualizado 1x/frame via glNamedBufferSubData
//
// GLSL não permite struct definida dentro de uniform block (erro C1321),
// então cada campo da luz vira um array de vec4 separado.
//
// Handles bindless são uvec2 (64 bits) empacotados em vec4 pares para
// respeitar o alinhamento std140 (16 bytes por array element).
// ============================================================================
struct alignas(16) GrassLightBlock {
    float timer;       // offset  0
    float camPosX;     // offset  4
    float camPosY;     // offset  8
    float lightCount;  // offset 12
    // Escalares de vento/LOD (movidos do glProgramUniform para o UBO)
    float strength;        // offset 16
    float range;           // offset 20
    float windDisableDist; // offset 24
    float onlySunDist;     // offset 28
    // Handles bindless das texturas: texNoise (slot 0) e texGrass0 (slot 1)
    // uvec2 em std140 alinha a 8 bytes; dois uvec2 = 16 bytes (offsets 32..47)
    uint64_t hTexNoise;    // offset 32  (uvec2 no GLSL)
    uint64_t hTexGrass0;   // offset 40  (uvec2 no GLSL)
    // Arrays de vec4 (std140) exigem base alinhada a 16.
    // offset após hTexGrass0 = 48 — já é múltiplo de 16, sem pad necessário.
    // Cada campo da luz como array de 4 × vec4 (std140 alinha arrays a 16 bytes)
    float lType    [4][4]; // offset 48  — [i][0] = type, [i][1..3] = pad
    float lDiffuse [4][4]; // offset 112
    float lPosition[4][4]; // offset 176
    float lSpotDir [4][4]; // offset 240
    float lParams  [4][4]; // offset 304
};

struct GrassLightOverride {
    float multiplier = 1.0f;
    mt::vec3 color = mt::vec3(-1.0f, -1.0f, -1.0f);
};

// ============================================================================
// SPSC threading — snapshot + resultado do worker
// ============================================================================

// Input imutável capturado pela main thread antes de enfileirar.
// NÃO contém ponteiros para KX_Mesh ou KX_GameObject.
// Arrays de dados brutos copiados diretamente dos DisplayArrays —
// a transformação world-space fica para o worker.
struct GrassJobInput {
    KX_GameObject              *terrain   = nullptr; // só para identificação na main thread
    uint64_t                    terrainToken = 0;
    uint64_t                    buildEpoch = 0;
    GrassParams                 params;
    mt::mat3x4                  worldMat;            // copiada como POD
    bool                        hasColors = false;   // se false, todos os vértices são surface

    // Arrays brutos copiados contiguamente de todos os DisplayArrays da mesh.
    // Cada entrada em triIndices é um índice global em positions[].
    std::vector<mt::vec3_packed> positions;          // posições locais (sem transformar)
    std::vector<uint32_t>        triIndices;         // triplas: [i0, i1, i2, ...]
    std::vector<uint8_t>         surfaceAlpha;       // alpha por vértice (0 = não-surface)

    // AABB pré-calculada em world space para chunk grid
    float                       invChunkSize = 0.0f;
    int                         minChunkX = 0, maxChunkX = 0;
    int                         minChunkY = 0, maxChunkY = 0;
    uint32_t                    bladeLenVersion = 0; // snapshot de m_bladeLenVersion
};

// Output preenchido pelo worker, consumido pela main thread.
struct GrassJobResult {
    KX_GameObject              *terrain      = nullptr;
    uint64_t                    terrainToken = 0;
    uint64_t                    buildEpoch = 0;
    std::vector<GrassInstance>  instances;
    std::vector<GrassPackedNormal> baseNormals;
    std::vector<GrassChunk>     chunks;
    GrassChunkLodSoA            lodSoA;
    std::vector<GrassMegaChunk> megaChunks;
    std::vector<GrassSubChunk>  subChunks;   // Sub-chunks gerados
    int                         instanceCount = 0;
    std::atomic<bool>           ready         {false};

    GrassJobResult() = default;
    // Não copiável por causa do atomic
    GrassJobResult(const GrassJobResult&) = delete;
    GrassJobResult& operator=(const GrassJobResult&) = delete;
    GrassJobResult(GrassJobResult&&) = delete;
    GrassJobResult& operator=(GrassJobResult&&) = delete;
};

// ============================================================================
// ThreadArena — arena PMR com buffer alinhado no heap, reset O(1).
//
// _aligned_malloc/_aligned_free (Windows/MSVC) ou aligned_alloc/free (GCC/Clang)
// garantem alinhamento de cache-line (64 bytes) sem overhead de std::vector.
// Não colocar o buffer no stack — WorkerLoop roda em std::thread com
// stack default de 1MB no Windows; 4MB no stack causa stack overflow.
// ============================================================================
struct ThreadArena {
    std::byte                           *buffer  = nullptr;
    size_t                               size    = 0;
    std::pmr::monotonic_buffer_resource  arena;

    static constexpr size_t kAlign = 64;

    static size_t AlignUp(size_t v) { return (v + kAlign - 1) & ~(kAlign - 1); }

    // Aloca com alinhamento garantido e lança bad_alloc se falhar —
    // evita construir arena(nullptr, ...) que é comportamento indefinido.
    static std::byte *Allocate(size_t sz)
    {
#if defined(_WIN32) || defined(_MSC_VER)
        auto *p = static_cast<std::byte *>(_aligned_malloc(sz, kAlign));
#else
        auto *p = static_cast<std::byte *>(std::aligned_alloc(kAlign, sz));
#endif
        if (!p) throw std::bad_alloc();
        return p;
    }

    // Ordem da lista de inicialização segue a ordem dos membros na struct:
    // buffer → size → arena. AlignUp garante múltiplo de kAlign (exigência POSIX).
    explicit ThreadArena(size_t sz,
                         std::pmr::memory_resource *fallback = std::pmr::null_memory_resource())
        : buffer(Allocate(AlignUp(sz)))
        , size(AlignUp(sz))
        , arena(buffer, size, fallback)
    {}

    ~ThreadArena()
    {
#if defined(_WIN32) || defined(_MSC_VER)
        _aligned_free(buffer);
#else
        std::free(buffer);
#endif
    }

    void reset() { arena.release(); }

    std::pmr::memory_resource *get() { return &arena; }

    ThreadArena(const ThreadArena &)            = delete;
    ThreadArena &operator=(const ThreadArena &) = delete;
    ThreadArena(ThreadArena &&)                 = delete;
    ThreadArena &operator=(ThreadArena &&)      = delete;
};

// ============================================================================
// KX_GrassSystem
// ============================================================================
class KX_GrassSystem {
public:
    static constexpr float CHUNK_SIZE = 42.0f;

    explicit KX_GrassSystem(KX_Scene *scene);
    ~KX_GrassSystem();

    // Registra um terrain — pode ser chamado antes de Init()
    void RegisterTerrain(KX_GameObject *terrain);
    void UnregisterTerrain(KX_GameObject *terrain);

    // Retorna true se a cena dada tem pelo menos um terrain registrado
    // em qualquer instância do GrassSystem. Usado pelo engine para evitar
    // Draw() desnecessário em cenas sem grama.
    static bool SceneHasTerrain(KX_Scene *scene);

    // Constrói instâncias, divide em chunks, faz upload GPU
    // Deve ser chamado após todos os terrenos registrarem
    bool Init();

    // Shutdown explícito — sinaliza o worker para parar e espera o join.
    // Chamado pelo destrutor; pode ser chamado antecipadamente pela engine
    // para garantir shutdown limpo antes da destruição da cena.
    void Shutdown();

    // Muda parâmetros globais — uniforms re-enviados no próximo Draw
    void SetParams(const GrassParams &p);
    const GrassParams &GetParams() const { return m_params; }

    // Chamado todo frame por KX_Scene::DrawGrassSystem()
    // Init() é chamado automaticamente no primeiro Draw() se necessário
    void Draw(RAS_Rasterizer *rasty = nullptr);

    // Texturas (slots 0-3: noise, grass0, grass1, grass2)
    void SetTexture(int slot, RAS_Texture *tex);
    void SetTextureObject(KX_GameObject *obj);
    // Multiplicador de força de lâmpadas específicas
    void ClearLightMultipliers();
    void AddLightMultiplier(const std::string& name, float multiplier, const mt::vec3& color = mt::vec3(-1.0f, -1.0f, -1.0f));

private:
    struct GrassChunkDrawRef {
        KX_GameObject *terrainKey = nullptr;  // chave estável no map (ponteiro não move)
        uint32_t chunkIndex = 0;
        uint32_t drawBaseInstance = 0;
    };

    // Métodos para sistema incremental
    TerrainGrassData* BuildTerrainData(KX_GameObject *terrain);
    bool AllocateVBO(int count, int &outOffset);
    void DeallocateVBO(int offset, int count);
    void AppendToGPUBuffer(const std::vector<GrassInstance> &instances, TerrainGrassData &data);
    void ResizeIndirectBuffer(int newCapacity);
    void EnsureGPUResources(); // Cria Shader, VAO e VBO inicial se necessário

    void CacheLightUniforms();  // não-op — mantida para compatibilidade de chamadas antigas
    void UploadStaticUniforms();
    bool AcquireBindlessHandles(); // cria/valida handles bindless para as texturas ativas
    void UpdateBoundsIfNeeded();
    void UpdateTiltIfNeeded();
    void RebuildCommandLayoutIfNeeded();

    // SPSC threading
    void CaptureTerrainSnapshot(KX_GameObject *terrain, GrassJobInput &out);
    void EnqueueBuildJob(KX_GameObject *terrain);
    void ProcessFinishedJobs();
    void WorkerLoop();

    KX_Scene                    *m_scene;
    GrassParams                  m_params;
    bool                         m_paramsChanged = false;
    bool                         m_initialized   = false;

    KX_GameObject               *m_textureObject = nullptr;

    // Dados organizados por terreno
    ankerl::unordered_dense::map<KX_GameObject*, TerrainGrassData> m_terrainData;
    std::vector<KX_GameObject*> m_terrains; // Mantém a ordem de registro se necessário

    // Gestão de memória GPU
    std::vector<GrassFreeBlock> m_freeBlocks;

    static constexpr int kIndirectRingSize = 8;

    // Cache de renderização persistente
    std::vector<GrassChunkDrawRef> m_chunkDrawOrder;
    std::vector<DrawArraysIndirectCommand> m_visibleDrawCommands;
    size_t m_visibleDrawCount = 0;
    int m_visibleIndirectIndex = 0;
    uint64_t m_visibleCommandsVersion = 1;
    bool m_visibleDrawCommandsDirty = true;
    bool m_commandLayoutDirty = true;
    uint64_t m_lastCullingFrame = 0;
    double   m_lastCullingTime  = 0.0;

    // UBO luz+timer (binding 0)
    unsigned int m_lightUBO      = 0;
    GrassLightBlock m_lightBlockCPU = {}; // cópia CPU — só sobe se dirty

    GPUShader   *m_prog          = nullptr;
    unsigned int m_computeProg   = 0;
    int          m_uComputeBaseInstance = -1;
    int          m_uComputeCount = -1;
    int          m_uComputeFlatness = -1;
    unsigned int m_vao           = 0;
    unsigned int m_instanceVBO   = 0;
    void        *m_instancePtr   = nullptr;
    unsigned int m_indirectVBO[kIndirectRingSize] = {}; // Ring de buffers para comandos indiretos
    DrawArraysIndirectCommand* m_indirectPtr[kIndirectRingSize] = {}; // Ponteiros mapeados persistentes
    void        *m_indirectFence[kIndirectRingSize] = {}; // Fence por slot do ring buffer indireto
    int          m_indirectWriteIndex = 0; // Próximo slot do ring a ser escrito
    int          m_indirectCapacity = 0; // Capacidade atual do m_indirectVBO (em comandos)
    uint64_t     m_indirectSlotVersion[kIndirectRingSize] = {};
    int          m_vboCapacity   = 0; // Capacidade atual do m_instanceVBO (em instâncias)
    int          m_totalInstances = 0; // Total de instâncias ocupadas no VBO (soma dos terrenos)
    int          m_highestUsedOffset = 0; // O maior offset já alocado (para otimizar cópia no grow)

    bool         m_hasBaseInstance = true;
    bool         m_hasIndirect     = true; // Suporte a glMultiDrawArraysIndirect
    bool         m_hasPersistentInstance = false;

    ankerl::unordered_dense::map<std::string, GrassLightOverride> m_lightMultipliers;

    RAS_Texture *m_textures[4]   = {};

    // Uniforms estáticos que FICAM como glProgramUniform (não entram no UBO)
    // REMOVIDOS — todos migraram para GrassLightBlock UBO (binding 0)
    // Mantemos apenas locations do compute shader (não têm UBO próprio)
    int m_uTexNoise    = -1; // reservado — não usado (bindless via UBO)
    int m_uTexGrass0   = -1; // reservado — não usado (bindless via UBO)
    int m_uTexGrass1   = -1;
    int m_uTexGrass2   = -1;

    // Handles bindless das texturas (residentes na GPU)
    uint64_t m_hTexNoise  = 0;
    uint64_t m_hTexGrass0 = 0;
    bool     m_texturesDirty = true; // true até ambos os handles estarem válidos

    // timer, camPosXY, lightCount e lights[] agora residem no UBO (m_lightUBO)
    // Mantemos apenas o location do binding block para verificação
    int m_uTimer       = -1; // reservado / não usado se UBO ativo

    // Attrib locations cacheadas — zero glGetAttribLocation por frame
    // Usando int em vez de GLint para evitar dependência de GL no header
    int m_locXYZ   = -1;
    int m_locSC    = -1;
    int m_locTilt  = -1;
    int m_locBlen  = -1;
#if KX_GRASS_DENSITY_SHADER_MERGE
    int m_locXYZ2  = -1; // worldXYZ2 — grama 2 fundida
    int m_locXYZ3  = -1; // worldXYZ3 — grama 3 fundida
    int m_locXYZ4  = -1; // worldXYZ4 — grama 4 fundida
#endif

    // m_lightUniforms removido — luzes agora vão via UBO (GrassLightBlock, binding 0)

    bool m_boundsDirty = false;
    bool m_forceCullingUpdate = false;
    uint32_t m_bladeLenVersion = 1;
    uint32_t m_tiltVersion = 1;

    float m_cachedBoundsScale = -1.0f;
    float m_cachedBoundsLenMax = -1.0f;
    float m_cachedBoundsHeightScale = -1.0f;
    float m_cachedBoundsStrength = -1.0f;

    // Contador global de terrains por cena.
    // Register: ++g_sceneTerrainCount[scene]
    // Unregister: decrementa, remove a entrada quando chega a 0.
    // Consulta HasScene() é O(1) — sem varredura de listas.
    static ankerl::unordered_dense::map<KX_Scene *, int> s_sceneTerrainCount;

    // =========================================================================
    // SPSC worker thread
    // =========================================================================
    static constexpr int kJobRingSize = 64;

    // Slot do ring buffer — input + output lado a lado para cache locality
    struct JobSlot {
        GrassJobInput  input;
        GrassJobResult result;
    };

    // Armazenamento dos slots — array fixo, sem alocação dinâmica no hot path
    JobSlot m_jobSlots[kJobRingSize];

    // Producer (main thread): escreve em m_jobSlots[writeIdx % kJobRingSize],
    // depois incrementa writeIdx.
    std::atomic<int> m_writeIdx {0};

    // Consumer (worker thread): lê de m_jobSlots[readIdx % kJobRingSize],
    // depois incrementa readIdx.
    std::atomic<int> m_readIdx  {0};

    // Main thread consome resultados prontos a partir deste índice.
    int m_doneIdx = 0;

    std::thread      m_workerThread;
    std::atomic<bool> m_workerRunning {false};
    std::mutex       m_workerCvMutex;
    std::condition_variable m_workerCv;
    std::atomic<uint32_t> m_workerWakeups {0};
    uint64_t m_nextTerrainToken = 1;
};

#endif // __KX_GRASS_SYSTEM_H__
