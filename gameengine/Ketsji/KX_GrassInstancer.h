#ifndef __KX_GRASS_INSTANCER_H__
#define __KX_GRASS_INSTANCER_H__

/**
 * KX_GrassInstancer — standalone GPU-instanced grass pass.
 * Terrain nunca é modificado. Draw call independente após o render do terreno.
 */

#ifdef WITH_PYTHON

#include "EXP_Python.h"
#include <vector>
#include <cstdint>

class KX_GameObject;
struct GPUShader;
class RAS_Texture;

// Per-instance data — 20 bytes (compactado)
// worldX, worldY, worldZ: float32 para precisão de posição
// random, paint: uint8 normalizado [0,255] → [0,1] no shader
// sinA, cosA: int8 normalizado [-127,127] → [-1,1] no shader
// nx, ny: int8 normal do terreno compactada; nz reconstruído no shader via sqrt(1-nx²-ny²)
struct GrassInstance {
    float    worldX, worldY, worldZ;  // 12 bytes — posição XYZ do terreno
    uint8_t  random;           // 1 byte — [0,255] → [0,1]
    uint8_t  paint;            // 1 byte — [0,255] → [0,1]
    int8_t   sinA;             // 1 byte — [-127,127] → [-1,1]
    int8_t   cosA;             // 1 byte — [-127,127] → [-1,1]
    int8_t   nx;               // 1 byte — normal X do terreno compactada
    int8_t   ny;               // 1 byte — normal Y do terreno compactada
    int8_t   _pad[2];          // 2 bytes — alinhamento para 20 bytes
};
static_assert(sizeof(GrassInstance) == 20, "GrassInstance must be 20 bytes");

// Uniforms de luz cacheados por slot — query feita uma vez no Init()
// Elimina GPU_shader_get_uniform() + snprintf() por frame por luz
struct GrassLightUniforms {
    int type        = -1;
    int dist        = -1;
    int invDist     = -1;
    int spotCutoff  = -1;
    int spotExponent= -1;
    int diffuse     = -1;
    int position    = -1;
    int spotDir     = -1;
};

class KX_GrassInstancer {
public:
    struct Params {
        float scale       = 1.0f;
        float lengthMax   = 1.0f;
        float lengthMin   = 0.5f;
        float heightScale = 1.0f;
        float distance    = 25.0f;
        float strength    = 1.0f;
        float speed       = 0.01f;
        float range       = 0.01f;
        float flatness    = 0.6f;  // 0 = totalmente inclinado, 1 = totalmente reto
        int   density     = 1;
    };

    KX_GrassInstancer(KX_GameObject *terrain, const Params &p);
    ~KX_GrassInstancer();

    bool Init();
    void Draw();

    static PyObject *PyDrawTrampoline(PyObject *self, PyObject *null);

private:
    void BuildInstances();
    void UploadGPUBuffers();
    void CacheLightUniforms();   // query todos os uniforms de luz uma vez
    void RegisterSceneCallback();

    KX_GameObject *m_terrain;
    Params         m_params;

    GPUShader    *m_prog        = nullptr;
    unsigned int  m_vao         = 0;
    unsigned int  m_geomVBO     = 0;
    unsigned int  m_instanceVBO = 0;
    int           m_instanceCount = 0;

    RAS_Texture  *m_textures[5] = {};

    // Uniforms estáticos (cacheados no Init)
    int m_uTimer      = -1;
    int m_uScale      = -1;
    int m_uLenMax     = -1;
    int m_uLenMin     = -1;
    int m_uHeightScale= -1;
    int m_uDist       = -1;
    int m_uStrength   = -1;
    int m_uRange      = -1;
    int m_uLightCount = -1;
    int m_uGrassNorm  = -1;  // normal global fallback (não mais usada como única fonte)
    int m_uFlatness   = -1;
    int m_uTexNoise   = -1;
    int m_uTexGrass0  = -1;
    int m_uTexGrass1  = -1;
    int m_uTexGrass2  = -1;
    // texGround removido — grama é objeto separado

    // Uniforms de luz cacheados — sem snprintf/hash lookup por frame
    GrassLightUniforms m_lightUniforms[4];  // max 4 luzes

    std::vector<GrassInstance> m_instances;
};

#endif // WITH_PYTHON
#endif // __KX_GRASS_INSTANCER_H__
