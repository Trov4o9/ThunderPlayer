/**
 * KX_GrassSystem.cpp — sistema global de grama GPU-instanced.
 *
 * Pipeline:
 *   Fase 1 — BuildTerrainData(): lê mesh de cada terrain registrado,
 *             gera GrassInstance[] agrupadas em chunks espaciais.
 *   Fase 2 — AppendToGPUBuffer(): upload incremental para o VBO global.
 *   Fase 3 — Draw(): culling por chunk e draw calls instanciados.
 */

#ifdef WITH_PYTHON

#define KX_GRASS_ENABLE_COMPUTE 1

#define KX_GRASS_ENABLE_OCCLUSION   0
#define KX_GRASS_OCCLUSION_SCAN_SCENE 1

#define KX_GRASS_DISABLE_CULLING_LOD 0
#define KX_GRASS_CAMERA_RELATIVE_XY  1
#define KX_GRASS_PERSISTENT_INDIRECT 1

#include "KX_GrassSystem.h"
#include "KX_Scene.h"
#include "KX_Camera.h"
#include "KX_GameObject.h"
#include "KX_LightObject.h"
#include "KX_Globals.h"
#include "KX_BlenderMaterial.h"
#include "KX_Mesh.h"
#include "RAS_Mesh.h"
#include "RAS_DisplayArray.h"
#include "RAS_ILightObject.h"
#include "RAS_Texture.h"
#include "RAS_Rasterizer.h"
#include "SG_Frustum.h"
#include "DNA_scene_types.h"

#include "CcdPhysicsEnvironment.h"

extern "C" {
#  include "GPU_glew.h"
#  include "GPU_shader.h"
#  include "GPU_vertex_array.h"
}

#include <array>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <utility>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory_resource>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

#if !defined(__AVX2__)
#error "KX_GrassSystem SIMD requires AVX2"
#endif
#include <immintrin.h>

extern uint64_t GetCurrentFrame();


ankerl::unordered_dense::map<KX_Scene *, int> KX_GrassSystem::s_sceneTerrainCount;

bool KX_GrassSystem::SceneHasTerrain(KX_Scene *scene)
{
    return s_sceneTerrainCount.find(scene) != s_sceneTerrainCount.end();
}

static constexpr float kLodUpdateThresholdFraction = 0.49f;

namespace {
static void DeleteIndirectSlotFence(void *&fencePtr)
{
    if (!fencePtr) {
        return;
    }

    glDeleteSync((GLsync)fencePtr);
    fencePtr = nullptr;
}

static bool PollIndirectSlotFence(void *&fencePtr)
{
    if (!fencePtr) {
        return true;
    }

    GLsync fence = (GLsync)fencePtr;
    const GLenum result = glClientWaitSync(fence, 0, 0);
    if (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED || result == GL_WAIT_FAILED) {
        glDeleteSync(fence);
        fencePtr = nullptr;
        return true;
    }
    return false;
}

static void WaitIndirectSlotFence(void *&fencePtr)
{
    if (!fencePtr) {
        return;
    }

    GLsync fence = (GLsync)fencePtr;
    glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ULL);
    glDeleteSync(fence);
    fencePtr = nullptr;
}
}  // namespace

void GrassChunkLodSoA::Clear()
{
    chunkCount = 0;
    minX.clear(); minY.clear(); minZ.clear();
    maxX.clear(); maxY.clear(); maxZ.clear();
    centerX.clear(); centerY.clear();
    instanceCount.clear();
    lastLODCount.clear();
    drawInstanceCount.clear();
    drawBaseInstance.clear();
}

void GrassChunkLodSoA::Reserve(size_t logicalChunkCount)
{
    const size_t n = logicalChunkCount + 7;
    minX.reserve(n); minY.reserve(n); minZ.reserve(n);
    maxX.reserve(n); maxY.reserve(n); maxZ.reserve(n);
    centerX.reserve(n); centerY.reserve(n);
    instanceCount.reserve(n);
    lastLODCount.reserve(n);
    drawInstanceCount.reserve(n);
    drawBaseInstance.reserve(n);
}

void GrassChunkLodSoA::PushChunk(float inMinX, float inMinY, float inMinZ,
                                 float inMaxX, float inMaxY, float inMaxZ,
                                 float inCenterX, float inCenterY,
                                 uint32_t inInstanceCount, uint32_t inLastLODCount,
                                 uint32_t inDrawInstanceCount, uint32_t inDrawBaseInstance)
{
    minX.push_back(inMinX); minY.push_back(inMinY); minZ.push_back(inMinZ);
    maxX.push_back(inMaxX); maxY.push_back(inMaxY); maxZ.push_back(inMaxZ);
    centerX.push_back(inCenterX); centerY.push_back(inCenterY);
    instanceCount.push_back(inInstanceCount);
    lastLODCount.push_back(inLastLODCount);
    drawInstanceCount.push_back(inDrawInstanceCount);
    drawBaseInstance.push_back(inDrawBaseInstance);
}

void GrassChunkLodSoA::Finalize(int logicalChunkCount)
{
    chunkCount = logicalChunkCount;
    const size_t n = (size_t)logicalChunkCount + 7;
    minX.resize(n, 0.0f); minY.resize(n, 0.0f); minZ.resize(n, 0.0f);
    maxX.resize(n, 0.0f); maxY.resize(n, 0.0f); maxZ.resize(n, 0.0f);
    centerX.resize(n, 0.0f); centerY.resize(n, 0.0f);
    instanceCount.resize(n, 0u);
    lastLODCount.resize(n, 0u);
    drawInstanceCount.resize(n, 0u);
    drawBaseInstance.resize(n, 0u);
}

namespace {
struct SimdFrustumPlanes {
    float nx[6], ny[6], nz[6], d[6];
    float anx[6], any[6], anz[6];
    explicit SimdFrustumPlanes(const std::array<mt::vec4, 6> &planes)
    {
        for (int i = 0; i < 6; ++i) {
            nx[i] = planes[i].x;
            ny[i] = planes[i].y;
            nz[i] = planes[i].z;
            d[i]  = planes[i].w;
            anx[i] = std::fabs(planes[i].x);
            any[i] = std::fabs(planes[i].y);
            anz[i] = std::fabs(planes[i].z);
        }
    }
};

static inline __m256i LaneMaskI32(int laneCount)
{
    switch (laneCount) {
        case 8:  return _mm256_set1_epi32(-1);
        case 7:  return _mm256_setr_epi32(-1, -1, -1, -1, -1, -1, -1, 0);
        case 6:  return _mm256_setr_epi32(-1, -1, -1, -1, -1, -1, 0, 0);
        case 5:  return _mm256_setr_epi32(-1, -1, -1, -1, -1, 0, 0, 0);
        case 4:  return _mm256_setr_epi32(-1, -1, -1, -1, 0, 0, 0, 0);
        case 3:  return _mm256_setr_epi32(-1, -1, -1, 0, 0, 0, 0, 0);
        case 2:  return _mm256_setr_epi32(-1, -1, 0, 0, 0, 0, 0, 0);
        case 1:  return _mm256_setr_epi32(-1, 0, 0, 0, 0, 0, 0, 0);
        default: return _mm256_setzero_si256();
    }
}

static inline __m256i SelectI(__m256i mask, __m256i a, __m256i b)
{
    return _mm256_or_si256(_mm256_and_si256(mask, a), _mm256_andnot_si256(mask, b));
}

static inline __m256i AbsI32(__m256i v)
{
    return _mm256_abs_epi32(v);
}

static inline __m256i FrustumNotOutsideMask8(const SimdFrustumPlanes &fp,
                                             const GrassChunkLodSoA &soa,
                                             int baseIndex)
{
    const __m256 minX = _mm256_loadu_ps(soa.minX.data() + baseIndex);
    const __m256 minY = _mm256_loadu_ps(soa.minY.data() + baseIndex);
    const __m256 minZ = _mm256_loadu_ps(soa.minZ.data() + baseIndex);
    const __m256 maxX = _mm256_loadu_ps(soa.maxX.data() + baseIndex);
    const __m256 maxY = _mm256_loadu_ps(soa.maxY.data() + baseIndex);
    const __m256 maxZ = _mm256_loadu_ps(soa.maxZ.data() + baseIndex);

    const __m256 half = _mm256_set1_ps(0.5f);
    const __m256 cx = _mm256_mul_ps(_mm256_add_ps(minX, maxX), half);
    const __m256 cy = _mm256_mul_ps(_mm256_add_ps(minY, maxY), half);
    const __m256 cz = _mm256_mul_ps(_mm256_add_ps(minZ, maxZ), half);

    const __m256 ex = _mm256_mul_ps(_mm256_sub_ps(maxX, minX), half);
    const __m256 ey = _mm256_mul_ps(_mm256_sub_ps(maxY, minY), half);
    const __m256 ez = _mm256_mul_ps(_mm256_sub_ps(maxZ, minZ), half);

    const __m256 zero = _mm256_setzero_ps();
    __m256 insideMask = _mm256_castsi256_ps(_mm256_set1_epi32(-1));

    for (int i = 0; i < 6; ++i) {
        const __m256 nx = _mm256_set1_ps(fp.nx[i]);
        const __m256 ny = _mm256_set1_ps(fp.ny[i]);
        const __m256 nz = _mm256_set1_ps(fp.nz[i]);
        const __m256 d  = _mm256_set1_ps(fp.d[i]);

        const __m256 dist = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(nx, cx), _mm256_mul_ps(ny, cy)),
                                          _mm256_add_ps(_mm256_mul_ps(nz, cz), d));

        const __m256 anx = _mm256_set1_ps(fp.anx[i]);
        const __m256 any = _mm256_set1_ps(fp.any[i]);
        const __m256 anz = _mm256_set1_ps(fp.anz[i]);
        const __m256 rad = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(ex, anx), _mm256_mul_ps(ey, any)),
                                         _mm256_mul_ps(ez, anz));

        const __m256 sum = _mm256_add_ps(dist, rad);
        const __m256 inside = _mm256_cmp_ps(sum, zero, _CMP_GE_OQ);
        insideMask = _mm256_and_ps(insideMask, inside);

        if (_mm256_movemask_ps(insideMask) == 0) {
            break;
        }
    }

    return _mm256_castps_si256(insideMask);
}

// ============================================================================
// Culling de sub-chunks — versão escalar para sub-chunks individuais
// ============================================================================
static inline bool SubChunkFrustumTest(const std::array<mt::vec4, 6> &planes,
                                       const GrassSubChunk &sub)
{
    const float cx = (sub.minX + sub.maxX) * 0.5f;
    const float cy = (sub.minY + sub.maxY) * 0.5f;
    const float cz = (sub.minZ + sub.maxZ) * 0.5f;
    
    const float ex = (sub.maxX - sub.minX) * 0.5f;
    const float ey = (sub.maxY - sub.minY) * 0.5f;
    const float ez = (sub.maxZ - sub.minZ) * 0.5f;
    
    for (int i = 0; i < 6; ++i) {
        const float nx = planes[i].x;
        const float ny = planes[i].y;
        const float nz = planes[i].z;
        const float d  = planes[i].w;
        
        const float dist = nx * cx + ny * cy + nz * cz + d;
        const float rad = std::fabs(nx) * ex + std::fabs(ny) * ey + std::fabs(nz) * ez;
        
        if (dist + rad < 0.0f) {
            return false; // Totalmente fora
        }
    }
    
    return true; // Visível
}

static inline bool LodUpdateRangeSimd(GrassChunkLodSoA &soa,
                                     int firstChunk,
                                     int chunkCount,
                                     const SimdFrustumPlanes *fp,
                                     bool megaInside,
                                     float camX,
                                     float camY,
                                     float lodStartSq,
                                     float lodEndSq)
{
    const float denom = std::max(0.0001f, lodEndSq - lodStartSq);
    const float invRange = 1.0f / denom;

    const __m256 camXv = _mm256_set1_ps(camX);
    const __m256 camYv = _mm256_set1_ps(camY);
    const __m256 lodStartSqv = _mm256_set1_ps(lodStartSq);
    const __m256 invRangev = _mm256_set1_ps(invRange);
    const __m256 one = _mm256_set1_ps(1.0f);
    const __m256 zero = _mm256_setzero_ps();
    const __m256 thrFrac = _mm256_set1_ps(kLodUpdateThresholdFraction);

    const __m256i zeroI = _mm256_setzero_si256();

    bool anyDraw = false;

    for (int off = 0; off < chunkCount; off += 8) {
        const int lanes = std::min(8, chunkCount - off);
        const __m256i activeMask = LaneMaskI32(lanes);
        const int baseIndex = firstChunk + off;

        __m256i visibleMask = activeMask;
        if (!megaInside) {
            visibleMask = _mm256_and_si256(visibleMask, FrustumNotOutsideMask8(*fp, soa, baseIndex));
        }

        const __m256 cx = _mm256_loadu_ps(soa.centerX.data() + baseIndex);
        const __m256 cy = _mm256_loadu_ps(soa.centerY.data() + baseIndex);

        const __m256 dx = _mm256_sub_ps(cx, camXv);
        const __m256 dy = _mm256_sub_ps(cy, camYv);
        const __m256 distSq = _mm256_add_ps(_mm256_mul_ps(dx, dx), _mm256_mul_ps(dy, dy));

        __m256 t = _mm256_mul_ps(_mm256_sub_ps(distSq, lodStartSqv), invRangev);
        t = _mm256_max_ps(zero, _mm256_min_ps(one, t));
        const __m256 t2 = _mm256_mul_ps(t, t);
        t = _mm256_mul_ps(t2, _mm256_sub_ps(_mm256_set1_ps(3.0f), _mm256_mul_ps(_mm256_set1_ps(2.0f), t)));

        const __m256 density = _mm256_sub_ps(one, t);

        const __m256i instCountI = _mm256_loadu_si256((const __m256i *)(soa.instanceCount.data() + baseIndex));
        const __m256 instCountF = _mm256_cvtepi32_ps(instCountI);

        const __m256 prod = _mm256_mul_ps(instCountF, density);
        const __m256i targetI = _mm256_cvttps_epi32(prod);

        const __m256i oldLastI = _mm256_loadu_si256((const __m256i *)(soa.lastLODCount.data() + baseIndex));
        const __m256i oldDrawI = _mm256_loadu_si256((const __m256i *)(soa.drawInstanceCount.data() + baseIndex));

        const __m256i deltaI = AbsI32(_mm256_sub_epi32(targetI, oldLastI));
        const __m256 deltaF = _mm256_cvtepi32_ps(deltaI);
        const __m256 thrF = _mm256_mul_ps(instCountF, thrFrac);

        const __m256 updF = _mm256_cmp_ps(deltaF, thrF, _CMP_GT_OQ);
        __m256i updateMask = _mm256_castps_si256(updF);
        updateMask = _mm256_or_si256(updateMask, _mm256_cmpeq_epi32(targetI, zeroI));
        updateMask = _mm256_or_si256(updateMask, _mm256_cmpeq_epi32(targetI, instCountI));
        updateMask = _mm256_and_si256(updateMask, visibleMask);

        const __m256i newLastI = SelectI(updateMask, targetI, oldLastI);

        const __m256i nonZeroMask = _mm256_cmpgt_epi32(newLastI, zeroI);
        const __m256i drawMask = _mm256_and_si256(visibleMask, nonZeroMask);
        const __m256i newDrawI = _mm256_and_si256(newLastI, drawMask);

        const __m256i storeLastI = SelectI(activeMask, newLastI, oldLastI);
        const __m256i storeDrawI = SelectI(activeMask, newDrawI, oldDrawI);

        _mm256_storeu_si256((__m256i *)(soa.lastLODCount.data() + baseIndex), storeLastI);
        _mm256_storeu_si256((__m256i *)(soa.drawInstanceCount.data() + baseIndex), storeDrawI);

        const __m256i anyMask = _mm256_cmpgt_epi32(storeDrawI, zeroI);
        anyDraw = anyDraw || (_mm256_movemask_ps(_mm256_castsi256_ps(anyMask)) != 0);
    }

    return anyDraw;
}

static inline void ZeroDrawRangeSimd(GrassChunkLodSoA &soa, int firstChunk, int chunkCount)
{
    const __m256i zeroI = _mm256_setzero_si256();
    for (int off = 0; off < chunkCount; off += 8) {
        const int lanes = std::min(8, chunkCount - off);
        const __m256i activeMask = LaneMaskI32(lanes);
        const int baseIndex = firstChunk + off;
        const __m256i oldDrawI = _mm256_loadu_si256((const __m256i *)(soa.drawInstanceCount.data() + baseIndex));
        const __m256i storeDrawI = SelectI(activeMask, zeroI, oldDrawI);
        _mm256_storeu_si256((__m256i *)(soa.drawInstanceCount.data() + baseIndex), storeDrawI);
    }
}
}  // namespace

static float InverseSmoothstep(float s)
{
	s = std::max(0.0f, std::min(1.0f, s));
	float lo = 0.0f;
	float hi = 1.0f;
	for (int i = 0; i < 24; ++i) {
		const float mid = 0.5f * (lo + hi);
		const float f = mid * mid * (3.0f - 2.0f * mid);
		if (f < s) {
			lo = mid;
		}
		else {
			hi = mid;
		}
	}
	return 0.5f * (lo + hi);
}

static inline int FastFloorToInt(float x)
{
    const int i = (int)x;
    return (x < (float)i) ? (i - 1) : i;
}

// ============================================================================
// Vertex shader
// ============================================================================
#if KX_GRASS_DENSITY_SHADER_MERGE
static const char *s_vert =
"#extension GL_ARB_bindless_texture : require\n"
// Posições das 4 gramas fundidas na instância
"in vec3 worldXYZ;\n"   // grama 1
"in vec3 worldXYZ2;\n"  // grama 2
"in vec3 worldXYZ3;\n"  // grama 3
"in vec3 worldXYZ4;\n"  // grama 4
"in vec2 instSinCos;\n"
"in vec3 instTilt;\n"
"in float instBlen;\n"
"\n"
"layout(std140, binding=2) uniform GrassLightBlock {\n"
"    vec4  timerCamLight;\n"
"    vec4  windParams;\n"
"    uvec2 hTexNoise;\n"
"    uvec2 hTexGrass0;\n"
"    vec4  lType[4];\n"
"    vec4  lDiffuse[4];\n"
"    vec4  lPosition[4];\n"
"    vec4  lSpotDir[4];\n"
"    vec4  lParams[4];\n"
"};\n"
"\n"
"out vec2  v_Coord;\n"
"out vec3  v_Light;\n"
"\n"
"vec3 calcSun(vec3 norm) {\n"
"    int cnt = int(timerCamLight.w);\n"
"    for (int l = 0; l < 4; l++) {\n"
"        if (l >= cnt) break;\n"
"        if (lType[l].x == 1.0)\n"
"            return lDiffuse[l].rgb * max(0.0, dot(norm, -lSpotDir[l].xyz));\n"
"    }\n"
"    return vec3(0.0);\n"
"}\n"
"\n"
"vec3 calcLight(vec3 pos, vec3 norm) {\n"
"    vec3 d = vec3(0.0);\n"
"    int cnt = int(timerCamLight.w);\n"
"    for (int l = 0; l < 4; l++) {\n"
"        if (l >= cnt) break;\n"
"        vec3 lp; float att;\n"
"        if (lType[l].x == 1.0) {\n"
"            lp = -lSpotDir[l].xyz; att = 1.0;\n"
"        } else {\n"
"            vec3 delta = lPosition[l].xyz - pos;\n"
"            float d2 = dot(delta, delta) + 1e-4;\n"
"            float radiusSq = lParams[l].w;\n"
"            if (radiusSq > 0.0 && d2 > radiusSq) continue;\n"
"            float inv = inversesqrt(d2);\n"
"            float sd = d2 * inv;\n"
"            lp = delta * inv;\n"
"            float x = sd * lParams[l].x + 1.0;\n"
"            float x2 = x * x; att = 1.0 / (x2 * x2 * x);\n"
"            if (lType[l].x == 0.0) {\n"
"                float sv = max(0.0, dot(-lp, lSpotDir[l].xyz));\n"
"                att *= smoothstep(0.0, 1.0,\n"
"                           (sv - lParams[l].y) / max(lParams[l].z, 0.0001));\n"
"            }\n"
"        }\n"
"        d += lDiffuse[l].rgb * max(0.0, dot(norm, lp)) * att;\n"
"    }\n"
"    return d;\n"
"}\n"
"\n"
"void main() {\n"
"    float timer          = timerCamLight.x;\n"
"    vec2  camPosXY       = timerCamLight.yz;\n"
"    float strength       = windParams.x;\n"
"    float range          = windParams.y;\n"
"    float windDisableDist = windParams.z;\n"
"    float onlySunDist    = windParams.w;\n"
"    float sinA  = instSinCos.x;\n"
"    float cosA  = instSinCos.y;\n"
"\n"
"    // Cada instância tem 24 vértices: 4 gramas × 6 vértices (2 triangulos de 3v).\n"
"    // vtxInInst: posição dentro dos 24 vértices da instância.\n"
"    // bladeIdx:  qual das 4 gramas (0, 1, 2 ou 3).\n"
"    // vtxInBlade: posição dentro dos 6 vértices da grama (0-5).\n"
"    int vtxInInst  = gl_VertexID % 24;\n"
"    int bladeIdx   = vtxInInst / 6;\n"
"    int vtxInBlade = vtxInInst % 6;\n"
"\n"
"    float bvX, bvY;\n"
"    int local = vtxInBlade % 3;\n"
"    if (local == 0) { bvX = -instBlen; bvY = 0.0; }\n"  // base esquerda
"    else if (local == 1) { bvX =  instBlen; bvY = 0.0; }\n"  // base direita
"    else                 { bvX = 0.0;       bvY = 1.0; }\n"  // topo
"\n"
"    vec3 pos;\n"
"    if (bladeIdx == 0)      pos = worldXYZ;\n"
"    else if (bladeIdx == 1) pos = worldXYZ2;\n"
"    else if (bladeIdx == 2) pos = worldXYZ3;\n"
"    else                    pos = worldXYZ4;\n"
"\n"
"    vec2 posXY = pos.xy;\n"
#if KX_GRASS_CAMERA_RELATIVE_XY
"    posXY -= camPosXY;\n"
#endif
"\n"
"    bool isSecondTri = (vtxInBlade >= 3);\n"
"    if (isSecondTri) {\n"
"        float tmp = sinA;\n"
"        sinA =  cosA;\n"
"        cosA = -tmp;\n"
"    }\n"
"\n"
"    vec3 tilt = instTilt;\n"
"\n"
"#if KX_GRASS_CAMERA_RELATIVE_XY\n"
"    vec2 d = posXY;\n"
"#else\n"
"    vec2 d = pos.xy - camPosXY;\n"
"#endif\n"
"    float dist2 = dot(d, d);\n"
"    float wind = 0.0;\n"
"    bool nearGrass    = dist2 <= windDisableDist;\n"
"    bool nearGrassSun = dist2 <= onlySunDist;\n"
"    if (nearGrass) {\n"
"        sampler2D noiseMap = sampler2D(hTexNoise);\n"
"        wind = texture2D(noiseMap, pos.xy * range + timer).r;\n"
"        wind = (wind * 2.0 - 1.0) * strength;\n"
"    }\n"
"\n"
"    float blen = instBlen;\n"
"\n"
"    if (bvY < 0.5) {\n"
"        vec2 delta = vec2(cosA * bvX, sinA * bvX);\n"
"        vec2 local2d = delta + posXY;\n"
"        // Compensa Z para manter a base da grama na superfície inclinada.\n"
"        // Projeta o offset lateral no plano tangente definido pela normal do terreno.\n"
"        float baseZ = pos.z;\n"
"        if (tilt.z > 0.001) {\n"
"            baseZ -= (tilt.x * delta.x + tilt.y * delta.y) / tilt.z;\n"
"        }\n"
"        v_Coord = (bvX < 0.0) ? vec2(0.0, -0.5) : vec2(1.0, -0.5);\n"
"        gl_Position = gl_ModelViewProjectionMatrix * vec4(local2d, baseZ, 1.0);\n"
"    } else {\n"
"        v_Coord = vec2(0.5, 0.0);\n"
"        vec3 tip = vec3(posXY, pos.z) + vec3(\n"
"            tilt.x * blen - wind,\n"
"            tilt.y * blen - wind,\n"
"            tilt.z * blen);\n"
"        gl_Position = gl_ModelViewProjectionMatrix * vec4(tip, 1.0);\n"
"    }\n"
"\n"
"    if (nearGrassSun)\n"
"        v_Light = calcLight(pos, tilt);\n"
"    else\n"
"        v_Light = calcSun(tilt);\n"
"    v_Light = v_Light * (2.5 / (1.0 + v_Light));\n"
"}\n";
#else
static const char *s_vert =
"#extension GL_ARB_bindless_texture : require\n"
"in vec3 worldXYZ;\n"
"in vec2 instSinCos;\n"
"in vec3 instTilt;\n"
"in float instBlen;\n"
"\n"
"layout(std140, binding=2) uniform GrassLightBlock {\n"
"    vec4  timerCamLight;\n"
"    vec4  windParams;\n"
"    uvec2 hTexNoise;\n"
"    uvec2 hTexGrass0;\n"
"    vec4  lType[4];\n"
"    vec4  lDiffuse[4];\n"
"    vec4  lPosition[4];\n"
"    vec4  lSpotDir[4];\n"
"    vec4  lParams[4];\n"
"};\n"
"\n"
"out vec2  v_Coord;\n"
"out vec3  v_Light;\n"
"\n"
"vec3 calcSun(vec3 norm) {\n"
"    int cnt = int(timerCamLight.w);\n"
"    for (int l = 0; l < 4; l++) {\n"
"        if (l >= cnt) break;\n"
"        if (lType[l].x == 1.0)\n"
"            return lDiffuse[l].rgb * max(0.0, dot(norm, -lSpotDir[l].xyz));\n"
"    }\n"
"    return vec3(0.0);\n"
"}\n"
"\n"
"vec3 calcLight(vec3 pos, vec3 norm) {\n"
"    vec3 d = vec3(0.0);\n"
"    int cnt = int(timerCamLight.w);\n"
"    for (int l = 0; l < 4; l++) {\n"
"        if (l >= cnt) break;\n"
"        vec3 lp; float att;\n"
"        if (lType[l].x == 1.0) {\n"
"            lp = -lSpotDir[l].xyz; att = 1.0;\n"
"        } else {\n"
"            vec3 delta = lPosition[l].xyz - pos;\n"
"            float d2 = dot(delta, delta) + 1e-4;\n"
"            float radiusSq = lParams[l].w;\n"
"            if (radiusSq > 0.0 && d2 > radiusSq) continue;\n"
"            float inv = inversesqrt(d2);\n"
"            float sd = d2 * inv;\n"
"            lp = delta * inv;\n"
"            float x = sd * lParams[l].x + 1.0;\n"
"            float x2 = x * x; att = 1.0 / (x2 * x2 * x);\n"
"            if (lType[l].x == 0.0) {\n"
"                float sv = max(0.0, dot(-lp, lSpotDir[l].xyz));\n"
"                att *= smoothstep(0.0, 1.0,\n"
"                           (sv - lParams[l].y) / max(lParams[l].z, 0.0001));\n"
"            }\n"
"        }\n"
"        d += lDiffuse[l].rgb * max(0.0, dot(norm, lp)) * att;\n"
"    }\n"
"    return d;\n"
"}\n"
"\n"
"void main() {\n"
"    float timer          = timerCamLight.x;\n"
"    vec2  camPosXY       = timerCamLight.yz;\n"
"    float strength       = windParams.x;\n"
"    float range          = windParams.y;\n"
"    float windDisableDist = windParams.z;\n"
"    float onlySunDist    = windParams.w;\n"
"    float sinA  = instSinCos.x;\n"
"    float cosA  = instSinCos.y;\n"
"\n"
"    // Geometria local calculada diretamente: evita leitura fora do geomVBO\n"
"    // com múltiplas instâncias. Layout: v0,v1=base, v2=topo (tri1); v3,v4=base, v5=topo (tri2)\n"
"    int vtxInBlade = gl_VertexID % 6;\n"
"    int localV = vtxInBlade % 3;\n"
"    float bvX, bvY;\n"
"    if (localV == 0)      { bvX = -instBlen; bvY = 0.0; }\n"
"    else if (localV == 1) { bvX =  instBlen; bvY = 0.0; }\n"
"    else                  { bvX = 0.0;       bvY = 1.0; }\n"
"\n"
"    // Segundo triângulo: rotação +90° (sin(A+90)=cosA, cos(A+90)=-sinA)\n"
"    bool isSecondTri = (vtxInBlade >= 3);\n"
"    if (isSecondTri) {\n"
"        float tmp = sinA;\n"
"        sinA =  cosA;\n"
"        cosA = -tmp;\n"
"    }\n"
"\n"
"    vec3 tilt = instTilt;\n"
"\n"
"    vec2 posXY = worldXYZ.xy;\n"
#if KX_GRASS_CAMERA_RELATIVE_XY
"    posXY -= camPosXY;\n"
#endif
"\n"
"#if KX_GRASS_CAMERA_RELATIVE_XY\n"
"    vec2 d = posXY;\n"
"#else\n"
"    vec2 d = worldXYZ.xy - camPosXY;\n"
"#endif\n"
"    float dist2 = dot(d, d);\n"
"    float wind = 0.0;\n"
"    bool nearGrass = dist2 <= windDisableDist;\n"
"    bool nearGrassSun = dist2 <= onlySunDist;\n"
"    if (nearGrass) {\n"
"        sampler2D noiseMap = sampler2D(hTexNoise);\n"
"        wind = texture2D(noiseMap, worldXYZ.xy * range + timer).r;\n"
"        wind = (wind * 2.0 - 1.0) * strength;\n"
"    }\n"
"\n"
"    float blen = instBlen;\n"
"\n"
"    if (bvY < 0.5) {\n"
"        vec2 delta = vec2(cosA * bvX, sinA * bvX);\n"
"        vec2 local2d = delta + posXY;\n"
"        float baseZ = worldXYZ.z;\n"
"        if (tilt.z > 0.001) {\n"
"            baseZ -= (tilt.x * delta.x + tilt.y * delta.y) / tilt.z;\n"
"        }\n"
"        v_Coord = (bvX < 0.0) ? vec2(0.0, -0.5) : vec2(1.0, -0.5);\n"
"        gl_Position = gl_ModelViewProjectionMatrix * vec4(local2d, baseZ, 1.0);\n"
"    } else {\n"
"        v_Coord = vec2(0.5, 0.0);\n"
"        vec3 tip = vec3(posXY, worldXYZ.z) + vec3(\n"
"            tilt.x * blen - wind,\n"
"            tilt.y * blen - wind,\n"
"            tilt.z * blen);\n"
"        gl_Position = gl_ModelViewProjectionMatrix * vec4(tip, 1.0);\n"
"    }\n"
"\n"
"    if (nearGrassSun)\n"
"        v_Light = calcLight(worldXYZ, tilt);\n"
"    else\n"
"        v_Light = calcSun(tilt);\n"
"    v_Light = v_Light * (2.5 / (1.0 + v_Light));\n"
"}\n";
#endif
// ============================================================================
// Fragment shader
// ============================================================================
static const char *s_frag =
"#extension GL_ARB_bindless_texture : require\n"
"in vec2  v_Coord;\n"
"in vec3  v_Light;\n"
"\n"
"layout(std140, binding=2) uniform GrassLightBlock {\n"
"    vec4  timerCamLight;\n"
"    vec4  windParams;\n"
"    uvec2 hTexNoise;\n"
"    uvec2 hTexGrass0;\n"
"    vec4  lType[4];\n"
"    vec4  lDiffuse[4];\n"
"    vec4  lPosition[4];\n"
"    vec4  lSpotDir[4];\n"
"    vec4  lParams[4];\n"
"};\n"
"\n"
"void main() {\n"
"    sampler2D grassMap = sampler2D(hTexGrass0);\n"
"    vec4 g0 = texture2D(grassMap, v_Coord);\n"
"    if (g0.a < 0.1) discard;\n"
"    gl_FragColor = vec4(g0.rgb * v_Light, 1.0);\n"
"}\n";

// ============================================================================
// Helpers GL
// ============================================================================
static inline void attribVec(GLint loc, GLsizei stride, intptr_t offset,
                              GLint size, GLuint divisor)
{
    if (loc < 0) return;
    glEnableVertexAttribArray((GLuint)loc);
    glVertexAttribPointer((GLuint)loc, size, GL_FLOAT, GL_FALSE,
                          stride, reinterpret_cast<const void *>(offset));
    glVertexAttribDivisor((GLuint)loc, divisor);
}

static inline void attribNorm(GLint loc, GLsizei stride, intptr_t offset,
                               GLint size, GLenum type, GLuint divisor)
{
    if (loc < 0) return;
    glEnableVertexAttribArray((GLuint)loc);
    glVertexAttribPointer((GLuint)loc, size, type, GL_TRUE,
                          stride, reinterpret_cast<const void *>(offset));
    glVertexAttribDivisor((GLuint)loc, divisor);
}

static inline uint32_t mix32(uint32_t h)
{
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

static inline uint32_t hash32f(float x, float y, uint32_t seed)
{
    const uint32_t ix = (uint32_t)(x * 1000.0f + 0.5f) ^ 0x9e3779b9u;
    const uint32_t iy = (uint32_t)(y * 1000.0f + 0.5f) ^ 0x6c62272eu;
    const uint32_t is = seed * 0x85ebca6bu;
    return mix32(ix ^ iy ^ is);
}

static inline float u01(uint32_t h)
{
    return (float)(h & 0xFFFFFFu) / (float)0x1000000u;
}

static inline float ComputeBladeLen(uint8_t randomByte, const GrassParams &p)
{
    const float rnd = (float)randomByte / 255.0f;
    const float raw = p.scale * 2.0f * p.lengthMax * rnd;
    const float clamped = std::max(p.lengthMin, std::min(p.lengthMax, raw));
    return clamped * p.heightScale;
}

static inline float Smoothstep01(float x)
{
    x = std::max(0.0f, std::min(1.0f, x));
    return x * x * (3.0f - 2.0f * x);
}

static inline int8_t PackSnorm8(float x)
{
    x = std::max(-1.0f, std::min(1.0f, x));
    const int v = (int)std::lround(x * 127.0f);
    return (int8_t)std::max(-127, std::min(127, v));
}

static inline mt::vec3 ComputeTiltFromTerrainNormal(const mt::vec3 &terrNorm, float flatness)
{
    const float t = Smoothstep01(terrNorm.z);
    const float f = flatness * (1.0f - t);
    mt::vec3 v = terrNorm * (1.0f - f) + mt::vec3(0.0f, 0.0f, 1.0f) * f;
    const float len = v.Length();
    if (len > 1e-12f) {
        v *= 1.0f / len;
    }
    else {
        v = mt::vec3(0.0f, 0.0f, 1.0f);
    }
    return v;
}

static inline uint32_t PackNormalBytes(const GrassPackedNormal &n)
{
    const uint32_t bx = (uint32_t)(uint8_t)n.x;
    const uint32_t by = (uint32_t)(uint8_t)n.y;
    const uint32_t bz = (uint32_t)(uint8_t)n.z;
    return bx | (by << 8) | (bz << 16);
}

static inline void GetSinCosTable(const int8_t *&outSin, const int8_t *&outCos)
{
    static int8_t sinTable[256];
    static int8_t cosTable[256];
    static bool initialized = false;
    if (!initialized) {
        for (int i = 0; i < 256; ++i) {
            const float a = (float)i * (6.28318530f / 256.0f);
            sinTable[i] = (int8_t)std::lround(std::sin(a) * 127.0f);
            cosTable[i] = (int8_t)std::lround(std::cos(a) * 127.0f);
        }
        initialized = true;
    }
    outSin = sinTable;
    outCos = cosTable;
}

static inline const float *GetSqrtLut()
{
    static constexpr int kSize = 512;
    static float table[kSize];
    static bool initialized = false;
    if (!initialized) {
        for (int i = 0; i < kSize; ++i) {
            const float u = ((float)i + 0.5f) * (1.0f / (float)kSize);
            table[i] = std::sqrt(u);
        }
        initialized = true;
    }
    return table;
}

static inline void RadixSortBySelectionKey(GrassInstance *begin, GrassInstance *end,
                                          std::vector<GrassInstance> &scratch)
{
    const size_t n = (size_t)(end - begin);
    if (n <= 1) {
        return;
    }

    scratch.resize(n);

    GrassInstance *src = begin;
    GrassInstance *dst = scratch.data();

    for (uint32_t pass = 0; pass < 4; ++pass) {
        const uint32_t shift = pass * 8u;

        size_t count[256];
        std::memset(count, 0, sizeof(count));
        for (size_t i = 0; i < n; ++i) {
            ++count[(src[i].selectionKey >> shift) & 0xFFu];
        }

        size_t offset[256];
        offset[0] = 0;
        for (size_t i = 1; i < 256; ++i) {
            offset[i] = offset[i - 1] + count[i - 1];
        }

        for (size_t i = 0; i < n; ++i) {
            const uint32_t b = (src[i].selectionKey >> shift) & 0xFFu;
            dst[offset[b]++] = src[i];
        }

        std::swap(src, dst);
        dst = (src == begin) ? scratch.data() : begin;
    }

    if (src != begin) {
        std::memcpy(begin, src, n * sizeof(GrassInstance));
    }
}

static inline void RadixSortBySelectionKey(GrassInstance *instances,
                                           GrassPackedNormal *baseNormals,
                                           size_t n,
                                           std::vector<GrassInstance> &scratchInstances,
                                           std::vector<GrassPackedNormal> &scratchNormals)
{
    if (n <= 1) {
        return;
    }

    scratchInstances.resize(n);
    scratchNormals.resize(n);

    GrassInstance *srcI = instances;
    GrassPackedNormal *srcN = baseNormals;
    GrassInstance *dstI = scratchInstances.data();
    GrassPackedNormal *dstN = scratchNormals.data();

    for (uint32_t pass = 0; pass < 4; ++pass) {
        const uint32_t shift = pass * 8u;

        size_t count[256];
        std::memset(count, 0, sizeof(count));
        for (size_t i = 0; i < n; ++i) {
            ++count[(srcI[i].selectionKey >> shift) & 0xFFu];
        }

        size_t offset[256];
        offset[0] = 0;
        for (size_t i = 1; i < 256; ++i) {
            offset[i] = offset[i - 1] + count[i - 1];
        }

        for (size_t i = 0; i < n; ++i) {
            const uint32_t b = (srcI[i].selectionKey >> shift) & 0xFFu;
            const size_t out = offset[b]++;
            dstI[out] = srcI[i];
            dstN[out] = srcN[i];
        }

        std::swap(srcI, dstI);
        std::swap(srcN, dstN);
        dstI = (srcI == instances) ? scratchInstances.data() : instances;
        dstN = (srcN == baseNormals) ? scratchNormals.data() : baseNormals;
    }

    if (srcI != instances) {
        std::memcpy(instances, srcI, n * sizeof(GrassInstance));
        std::memcpy(baseNormals, srcN, n * sizeof(GrassPackedNormal));
    }
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

KX_GrassSystem::KX_GrassSystem(KX_Scene *scene)
    : m_scene(scene),
      m_lastCullingFrame(0),
      m_lastCullingTime(0.0)
{
    m_chunkDrawOrder.reserve(1024);
    m_visibleDrawCommands.reserve(1024);
    m_terrainData.reserve(16);
    m_lightMultipliers.reserve(8);

    // Inicia worker thread SPSC
    m_workerRunning.store(true, std::memory_order_relaxed);
    m_workerThread = std::thread(&KX_GrassSystem::WorkerLoop, this);
}

// ============================================================================
// Shutdown — sinaliza o worker e aguarda o join de forma segura.
// Idempotente: pode ser chamado mais de uma vez sem efeito colateral.
// ============================================================================

void KX_GrassSystem::Shutdown()
{
    if (!m_workerRunning.load(std::memory_order_relaxed))
        return; // já foi chamado

    // Sinaliza o worker para parar na próxima oportunidade.
    // release garante que o worker veja todas as escritas anteriores.
    m_workerRunning.store(false, std::memory_order_release);
    m_workerWakeups.fetch_add(1, std::memory_order_release);
    m_workerCv.notify_all();

    if (m_workerThread.joinable()) {

        m_workerThread.join();
    }
}

KX_GrassSystem::~KX_GrassSystem()
{
    Shutdown();
    // Torna handles bindless não-residentes antes de destruir qualquer recurso GL
    if (m_hTexNoise  != 0) { glMakeTextureHandleNonResidentARB(m_hTexNoise);  m_hTexNoise  = 0; }
    if (m_hTexGrass0 != 0) { glMakeTextureHandleNonResidentARB(m_hTexGrass0); m_hTexGrass0 = 0; }
    if (m_prog)        { GPU_shader_free(m_prog);                m_prog = nullptr; }
    if (m_lightUBO)    { glDeleteBuffers(1, &m_lightUBO);        m_lightUBO = 0; }
    if (m_computeProg) { glDeleteProgram(m_computeProg);          m_computeProg = 0; }
    for (auto &pair : m_terrainData) {
        TerrainGrassData &td = pair.second;
        if (td.normalSSBO) {
            glDeleteBuffers(1, &td.normalSSBO);
            td.normalSSBO = 0;
        }
    }
    if (m_vao)         { glDeleteVertexArrays(1, &m_vao);       m_vao = 0; }
    if (m_instanceVBO) {
        if (m_instancePtr) {
            glUnmapNamedBuffer(m_instanceVBO);
            m_instancePtr = nullptr;
        }
        glDeleteBuffers(1, &m_instanceVBO);
        m_instanceVBO = 0;
    }
    for (int i = 0; i < kIndirectRingSize; ++i) {
        DeleteIndirectSlotFence(m_indirectFence[i]);
        if (m_indirectVBO[i]) {
            if (m_indirectPtr[i]) {
                glUnmapNamedBuffer(m_indirectVBO[i]);
                m_indirectPtr[i] = nullptr;
            }
            glDeleteBuffers(1, &m_indirectVBO[i]);
            m_indirectVBO[i] = 0;
        }
    }
}

// ============================================================================
// RegisterTerrain — só empilha; nenhum GL aqui
// ============================================================================

void KX_GrassSystem::RegisterTerrain(KX_GameObject *terrain)
{
    if (!terrain) return;

    EnsureGPUResources();
#if KX_GRASS_ENABLE_OCCLUSION
    m_scene->SetDbvtOcclusionRes(256);
#endif

    auto it = m_terrainData.find(terrain);
    if (it != m_terrainData.end()) {
        DeallocateVBO(it->second.vboOffset, it->second.instanceCount);
        EnqueueBuildJob(terrain);
#if KX_GRASS_ENABLE_OCCLUSION
        if (CcdPhysicsEnvironment *occlusionEnv = dynamic_cast<CcdPhysicsEnvironment *>(m_scene->GetPhysicsEnvironment())) {
            occlusionEnv->RegisterStaticOccluder(terrain);
#if KX_GRASS_OCCLUSION_SCAN_SCENE
            EXP_ListValue<KX_GameObject> *objects = m_scene->GetObjectList();
            for (int i = 0; i < objects->GetCount(); ++i) {
                KX_GameObject *obj = static_cast<KX_GameObject *>(objects->GetValue(i));
                if (obj && obj != terrain && !obj->GetMeshList().empty()) {
                    occlusionEnv->RegisterStaticOccluder(obj);
                }
            }
#endif // KX_GRASS_OCCLUSION_SCAN_SCENE
        }
#endif // KX_GRASS_ENABLE_OCCLUSION
        m_commandLayoutDirty = true;
        return;
    }

    TerrainGrassData &data = m_terrainData[terrain];
    data.terrain = terrain;
    data.terrainToken = m_nextTerrainToken++;
    data.buildEpoch = 0;

    EnqueueBuildJob(terrain); 
#if KX_GRASS_ENABLE_OCCLUSION
    if (CcdPhysicsEnvironment *occlusionEnv = dynamic_cast<CcdPhysicsEnvironment *>(m_scene->GetPhysicsEnvironment())) {
        // O terrain é registrado como occluder — um morro ocluda a grama
        // do outro lado quando vista de fora. A query usa chunk.minZ
        // (abaixo da superfície) para evitar z-fighting.
        occlusionEnv->RegisterStaticOccluder(terrain);
#if KX_GRASS_OCCLUSION_SCAN_SCENE
        EXP_ListValue<KX_GameObject> *objects = m_scene->GetObjectList();
        for (int i = 0; i < objects->GetCount(); ++i) {
            KX_GameObject *obj = static_cast<KX_GameObject *>(objects->GetValue(i));
            if (obj && obj != terrain && !obj->GetMeshList().empty()) {
                occlusionEnv->RegisterStaticOccluder(obj);
            }
        }
#endif // KX_GRASS_OCCLUSION_SCAN_SCENE
    }
#endif // KX_GRASS_ENABLE_OCCLUSION
    
    m_terrains.push_back(terrain);
    m_commandLayoutDirty = true;

    // Usa a cena do próprio terrain — robusto mesmo se um GrassSystem
    // eventualmente receber terrains de cenas distintas no futuro.
    ++s_sceneTerrainCount[terrain->GetScene()];
}

void KX_GrassSystem::UnregisterTerrain(KX_GameObject *terrain)
{
    if (!terrain) return;

    auto it = m_terrainData.find(terrain);
    if (it != m_terrainData.end()) {
        it->second.buildEpoch++;
        if (it->second.normalSSBO) {
            glDeleteBuffers(1, &it->second.normalSSBO);
            it->second.normalSSBO = 0;
        }
        DeallocateVBO(it->second.vboOffset, it->second.instanceCount);
        
        m_terrainData.erase(it);

#if KX_GRASS_ENABLE_OCCLUSION
        if (CcdPhysicsEnvironment *occlusionEnv = dynamic_cast<CcdPhysicsEnvironment *>(m_scene->GetPhysicsEnvironment())) {
            occlusionEnv->UnregisterStaticOccluder(terrain);
        }
#endif
        
        auto itT = std::find(m_terrains.begin(), m_terrains.end(), terrain);
        if (itT != m_terrains.end()) m_terrains.erase(itT);
        m_commandLayoutDirty = true;

        // Decrementa contador; remove a entrada quando chega a 0 — O(1), sem varredura
        auto itCount = s_sceneTerrainCount.find(terrain->GetScene());
        if (itCount != s_sceneTerrainCount.end()) {
            if (--itCount->second == 0)
                s_sceneTerrainCount.erase(itCount);
        }
    }
}

void KX_GrassSystem::CaptureTerrainSnapshot(KX_GameObject *terrain, GrassJobInput &out)
{
    // Main thread — única seção que toca em KX_GameObject / RAS_Mesh.
    // Objetivo: copiar dados brutos contíguos; toda a matemática fica para o worker.

    out.terrain  = terrain;
    out.params   = m_params;
    out.worldMat = terrain->NodeGetWorldTransform(); // cópia POD de mt::mat3x4

    const std::vector<KX_Mesh *> &meshes = terrain->GetMeshList();
    if (meshes.empty()) return;

    RAS_Mesh *mesh = static_cast<RAS_Mesh *>(meshes[0]);
    terrain->UpdateBounds(true);
    mt::vec3 localMin, localMax;
    terrain->GetBoundsAabb(localMin, localMax);

    const mt::vec3 corners[8] = {
        {localMin.x, localMin.y, localMin.z}, {localMin.x, localMin.y, localMax.z},
        {localMin.x, localMax.y, localMin.z}, {localMin.x, localMax.y, localMax.z},
        {localMax.x, localMin.y, localMin.z}, {localMax.x, localMin.y, localMax.z},
        {localMax.x, localMax.y, localMin.z}, {localMax.x, localMax.y, localMax.z},
    };

    mt::vec3 wMin = out.worldMat * corners[0];
    mt::vec3 wMax = wMin;
    for (int i = 1; i < 8; ++i) {
        const mt::vec3 w = out.worldMat * corners[i];
        wMin.x = std::min(wMin.x, w.x); wMin.y = std::min(wMin.y, w.y);
        wMax.x = std::max(wMax.x, w.x); wMax.y = std::max(wMax.y, w.y);
    }

    out.invChunkSize = 1.0f / CHUNK_SIZE;
    out.minChunkX = (int)std::floor(wMin.x / CHUNK_SIZE);
    out.maxChunkX = (int)std::floor(wMax.x / CHUNK_SIZE);
    out.minChunkY = (int)std::floor(wMin.y / CHUNK_SIZE);
    out.maxChunkY = (int)std::floor(wMax.y / CHUNK_SIZE);


    const RAS_MeshMaterialList &mats = mesh->GetMeshMaterialList();

    // Pré-calcula tamanhos para reserve único
    size_t totalVerts  = 0;
    size_t totalTris   = 0;
    bool   anyColors   = false;
    for (RAS_MeshMaterial *mm : mats) {
        RAS_DisplayArray *da = mm->GetDisplayArray();
        if (!da) continue;
        totalVerts += da->GetVertexCount();
        totalTris  += da->GetTriangleIndexCount() / 3;
        if (da->GetFormat().colorSize > 0) anyColors = true;
    }

    out.hasColors = anyColors;
    out.positions.clear();   out.positions.reserve(totalVerts);
    out.triIndices.clear();  out.triIndices.reserve(totalTris * 3);
    out.surfaceAlpha.clear();
    if (anyColors) out.surfaceAlpha.reserve(totalVerts);

    uint32_t vertexOffset = 0;
    for (RAS_MeshMaterial *mm : mats) {
        RAS_DisplayArray *da = mm->GetDisplayArray();
        if (!da) continue;

        const unsigned int vCount = da->GetVertexCount();
        const unsigned int iCount = da->GetTriangleIndexCount();
        if (vCount == 0 || iCount == 0) continue;

        // Posições — memcpy direto do array contíguo
        const mt::vec3_packed *posPtr = da->GetPositionsData();
        out.positions.insert(out.positions.end(), posPtr, posPtr + vCount);

        // Alpha de superfície por vértice (0 = não-surface, >0 = surface)
        if (anyColors) {
            if (da->GetFormat().colorSize > 0) {
                for (unsigned int v = 0; v < vCount; ++v) {
                    out.surfaceAlpha.push_back(da->GetColor(v, 0)[3]);
                }
            } else {
                // Material sem cor: todos os vértices são surface
                out.surfaceAlpha.insert(out.surfaceAlpha.end(), vCount, 255u);
            }
        }

        // Índices de triângulos com offset global
        const unsigned int *idxPtr = da->GetTriangleIndicesData();
        for (unsigned int i = 0; i < iCount; ++i) {
            out.triIndices.push_back(idxPtr[i] + vertexOffset);
        }

        vertexOffset += vCount;
    }
}

// ============================================================================

void KX_GrassSystem::EnqueueBuildJob(KX_GameObject *terrain)
{
    auto itData = m_terrainData.find(terrain);
    if (itData == m_terrainData.end()) {
        return;
    }
    TerrainGrassData &terrainData = itData->second;

    // Verifica se ainda há slot disponível — ring cheio = drop (não deve
    // acontecer com kJobRingSize=64 em uso normal)
    const int wi = m_writeIdx.load(std::memory_order_relaxed);
    const int ri = m_readIdx.load(std::memory_order_acquire);
    if (wi - ri >= kJobRingSize) {
        // Ring cheio; fallback síncrono para não perder o terrain
        BuildTerrainData(terrain);
        return;
    }

    JobSlot &slot = m_jobSlots[wi % kJobRingSize];
    slot.result.ready.store(false, std::memory_order_relaxed);
    slot.result.terrain = terrain;
    slot.result.terrainToken = terrainData.terrainToken;
    slot.result.buildEpoch = terrainData.buildEpoch + 1;
    slot.result.instances.clear();
    slot.result.baseNormals.clear();
    slot.result.chunks.clear();
    slot.result.lodSoA.Clear();
    slot.result.megaChunks.clear();
    slot.result.subChunks.clear();
    slot.result.instanceCount = 0;

    CaptureTerrainSnapshot(terrain, slot.input);
    slot.input.terrainToken = terrainData.terrainToken;
    slot.input.buildEpoch = ++terrainData.buildEpoch;
    slot.input.bladeLenVersion = m_bladeLenVersion;

    // Publica o slot para o worker
    m_writeIdx.store(wi + 1, std::memory_order_release);
    m_workerWakeups.fetch_add(1, std::memory_order_release);
    m_workerCv.notify_one();
}

// ============================================================================

void KX_GrassSystem::WorkerLoop()
{
    const float *sqrtLut       = GetSqrtLut();
    const int8_t *sinTable     = nullptr;
    const int8_t *cosTable     = nullptr;
    GetSinCosTable(sinTable, cosTable);

    static constexpr size_t kArenaSize     = 4 * 1024 * 1024; // 4 MB — chunk builders
    static constexpr size_t kMegaArenaSize =      256 * 1024; // 256 KB — mega builders

#ifdef NDEBUG
    std::pmr::memory_resource *fallback = std::pmr::get_default_resource();
#else
    std::pmr::memory_resource *fallback = std::pmr::null_memory_resource();
#endif

    ThreadArena chunkArena(kArenaSize,     fallback);
    ThreadArena megaArena( kMegaArenaSize, fallback);

    while (m_workerRunning.load(std::memory_order_acquire)) {
        // ── SPSC load correto ───────────────────────────────────────────────
        // acquire em wi garante que toda escrita do producer no slot é visível
        // antes de começarmos a ler. ri é relaxed porque só o worker o modifica.
        const int wi = m_writeIdx.load(std::memory_order_acquire);
        const int ri = m_readIdx.load(std::memory_order_relaxed);

        if (ri == wi) {
            const uint32_t seq = m_workerWakeups.load(std::memory_order_acquire);
            if (!m_workerRunning.load(std::memory_order_acquire)) {
                break;
            }
            if (m_writeIdx.load(std::memory_order_acquire) != m_readIdx.load(std::memory_order_relaxed)) {
                continue;
            }
            std::unique_lock<std::mutex> lock(m_workerCvMutex);
            m_workerCv.wait(lock, [this, seq]() {
                return !m_workerRunning.load(std::memory_order_acquire) ||
                       (m_workerWakeups.load(std::memory_order_acquire) != seq) ||
                       (m_writeIdx.load(std::memory_order_acquire) != m_readIdx.load(std::memory_order_relaxed));
            });
            continue;
        }

        // Guard de backpressure: nunca consumir além do que foi publicado.
        // Producer já impede wi-ri > kJobRingSize, mas checar aqui
        // torna o invariante explícito e detecta bugs em debug.
        if (wi - ri > kJobRingSize) {
            std::this_thread::yield();
            continue;
        }

        // ── Reset das arenas — O(1), sem malloc/free ─────────────────────
        chunkArena.reset();
        megaArena.reset();

        // Verificação de cancelamento antes de começar o job.
        // Se o destrutor setou m_workerRunning=false enquanto esperávamos,
        // sai imediatamente sem tocar nos slots (que podem estar sendo destruídos).
        if (!m_workerRunning.load(std::memory_order_acquire))
            break;

        JobSlot &slot = m_jobSlots[ri % kJobRingSize];
        const GrassJobInput &in = slot.input;
        GrassJobResult &out     = slot.result;

        // ── Mesma lógica de BuildTerrainData, sem nenhuma chamada GL ──────

        const float densityScale         = 0.15f;
        // Densidade negativa reduz na proporção inversa: density=-N equivale a 1/N gramas por unidade
        const float effectiveDensity     = (in.params.density >= 0)
            ? (float)in.params.density
            : (in.params.density != 0 ? 1.0f / (float)(-in.params.density) : 0.0f);
        const float densityPerSquareMeter = effectiveDensity * densityScale;
        const float invChunkSize          = in.invChunkSize;
        const int   minChunkX             = in.minChunkX;
        const int   minChunkY             = in.minChunkY;
        const size_t chunkSpanX = (size_t)std::max(1, in.maxChunkX - in.minChunkX + 1);
        const size_t chunkSpanY = (size_t)std::max(1, in.maxChunkY - in.minChunkY + 1);

        std::pmr::memory_resource *chunkResource = chunkArena.get();

        struct ChunkBuilder {
            std::pmr::vector<GrassInstance>     instances;
            std::pmr::vector<GrassPackedNormal> baseNormals;
            float minZ =  1e9f, maxZ = -1e9f;
            int32_t ccx = 0, ccy = 0;
#if KX_GRASS_DENSITY_SHADER_MERGE
            int  lastSlot = 4; // quantas gramas já foram escritas na última instância (0=nenhuma)
#endif
            explicit ChunkBuilder(std::pmr::memory_resource *r)
                : instances(r), baseNormals(r) {}
        };

        std::vector<int> chunkIndexGrid;
        chunkIndexGrid.assign(chunkSpanX * chunkSpanY, -1);
        std::vector<ChunkBuilder> builders;
        ankerl::unordered_dense::map<uint64_t, int> chunkIndexFallback;
        chunkIndexFallback.reserve(64);

        // Cache ponteiros brutos do input para acesso sem indireção
        const mt::vec3_packed *posData     = in.positions.data();
        const uint32_t        *idxData     = in.triIndices.data();
        const uint8_t         *alphaData   = in.hasColors ? in.surfaceAlpha.data() : nullptr;
        const size_t           triCount    = in.triIndices.size() / 3;

        // worldMat = mat3x4, column-major, kRows=3.
        // Usa o operador* da mathfu diretamente — evita indexação manual
        // que muda comportamento dependendo de MATHFU_COMPILE_WITH_PADDING.
        const mt::mat3x4 &wm = in.worldMat;

        for (size_t t = 0; t < triCount; ++t) {
            const uint32_t i0 = idxData[t * 3 + 0];
            const uint32_t i1 = idxData[t * 3 + 1];
            const uint32_t i2 = idxData[t * 3 + 2];

            // Surface flag — skip triângulo sem nenhum vértice de superfície
            if (alphaData) {
                if (alphaData[i0] <= 128 && alphaData[i1] <= 128 && alphaData[i2] <= 128)
                    continue;
            }

            // Transforma posições locais → world space usando o operador mathfu
            const mt::vec3_packed &p0 = posData[i0];
            const mt::vec3_packed &p1 = posData[i1];
            const mt::vec3_packed &p2 = posData[i2];

            const mt::vec3 wp0 = wm * mt::vec3(p0.x, p0.y, p0.z);
            const mt::vec3 wp1 = wm * mt::vec3(p1.x, p1.y, p1.z);
            const mt::vec3 wp2 = wm * mt::vec3(p2.x, p2.y, p2.z);

            const float x0 = wp0.x, y0 = wp0.y, z0 = wp0.z;
            const float x1 = wp1.x, y1 = wp1.y, z1 = wp1.z;
            const float x2 = wp2.x, y2 = wp2.y, z2 = wp2.z;

            const float cx = (x0 + x1 + x2) * (1.0f/3.0f);
            const float cy = (y0 + y1 + y2) * (1.0f/3.0f);

            const float ax = x1-x0, ay = y1-y0, az = z1-z0;
            const float bx = x2-x0, by = y2-y0, bz = z2-z0;
            const float cnx = ay*bz - az*by;
            const float cny = az*bx - ax*bz;
            const float cnz = ax*by - ay*bx;
            const float area = std::sqrt(cnx*cnx + cny*cny + cnz*cnz) * 0.5f;
            if (area <= 0.0f) continue;

            float nnx = cnx, nny = cny, nnz = cnz;
            if (nnz < 0.0f) { nnx=-nnx; nny=-nny; nnz=-nnz; }
            const float nlen = std::sqrt(nnx*nnx + nny*nny + nnz*nnz);
            if (nlen > 1e-12f) { nnx /= nlen; nny /= nlen; nnz /= nlen; }

            const int bladeCount = (int)(area * densityPerSquareMeter);
            for (int d = 0; d < bladeCount; ++d) {
                const uint32_t h  = hash32f(cx, cy, (uint32_t)d);
                const float    r1 = sqrtLut[(h >> 8) & 511u];
                const float    r2 = u01(mix32(h + 0x9e3779b9u));

                const float w0 = 1.0f - r1;
                const float w1 = r1 * (1.0f - r2);
                const float w2 = r1 * r2;

                const float wx = x0*w0 + x1*w1 + x2*w2;
                const float wy = y0*w0 + y1*w1 + y2*w2;
                const float wz = z0*w0 + z1*w1 + z2*w2;

                const uint32_t hw         = hash32f(wx, wy, (uint32_t)d ^ 0x85ebca6bu);
                const uint8_t  randomByte = (uint8_t)(hw & 0xFFu);
                const uint8_t  angByte    = (uint8_t)(mix32(hw + 0x27d4eb2du) & 0xFFu);

                const float invNoise         = 0.8004f + 0.4f * u01(mix32(hw + 0x68bc21ebu));
                const float slopeBase        = std::clamp((nnz - 0.85f) * 6.6666667f, 0.0f, 1.0f);
                const float vegetationWeight = std::clamp(slopeBase - 0.1f * invNoise, 0.0f, 1.0f);
                if (vegetationWeight <= 0.0f) continue;
                if (u01(mix32(hw + 0x9e3779b9u)) > vegetationWeight) continue;

                const int ccx = FastFloorToInt(wx * invChunkSize);
                const int ccy = FastFloorToInt(wy * invChunkSize);

                int *builderIndexPtr = nullptr;
                const int lx = ccx - minChunkX;
                const int ly = ccy - minChunkY;
                if (lx >= 0 && ly >= 0 && (size_t)lx < chunkSpanX && (size_t)ly < chunkSpanY) {
                    builderIndexPtr = &chunkIndexGrid[(size_t)ly * chunkSpanX + (size_t)lx];
                }
                else {
                    const uint64_t key = ((uint64_t)(uint32_t)ccx << 32) | (uint32_t)ccy;
                    auto [itF, inserted] = chunkIndexFallback.try_emplace(key, -1);
                    builderIndexPtr = &itF->second;
                }

                int &builderIndex = *builderIndexPtr;
                if (builderIndex < 0) {
                    builderIndex = (int)builders.size();
                    builders.emplace_back(chunkResource);
                    builders.back().ccx = (int32_t)ccx;
                    builders.back().ccy = (int32_t)ccy;
                }
                ChunkBuilder &builder = builders[(size_t)builderIndex];

#if KX_GRASS_DENSITY_SHADER_MERGE
                // Modo fusão: até 4 gramas por instância.
                // lastSlot==4 => a última instância está cheia, cria nova.
                if (builder.lastSlot >= 4) {
                    builder.instances.emplace_back();
                    GrassInstance &inst = builder.instances.back();
                    builder.baseNormals.emplace_back();
                    GrassPackedNormal &baseN = builder.baseNormals.back();

                    inst.worldX = wx; inst.worldY = wy; inst.worldZ = wz;
                    inst.worldX2 = wx; inst.worldY2 = wy; inst.worldZ2 = wz;
                    inst.worldX3 = wx; inst.worldY3 = wy; inst.worldZ3 = wz;
                    inst.worldX4 = wx; inst.worldY4 = wy; inst.worldZ4 = wz;
                    inst.random   = randomByte;
                    inst.bladeLen = ComputeBladeLen(inst.random, in.params);
                    inst.sinA     = sinTable[angByte];
                    inst.cosA     = cosTable[angByte];
                    baseN.x = (int8_t)(nnx * 127.0f);
                    baseN.y = (int8_t)(nny * 127.0f);
                    baseN.z = (int8_t)(nnz * 127.0f);
                    inst.tiltX = 0; inst.tiltY = 0; inst.tiltZ = 127;
                    inst.selectionKey = hw;
                    inst._pad[0] = 0; inst._pad[1] = 0;
                    builder.lastSlot = 1; // grama 1 preenchida
                }
                else {
                    // Preenche slot 2, 3 ou 4 na instância existente
                    GrassInstance &inst = builder.instances.back();
                    if (builder.lastSlot == 1) {
                        inst.worldX2 = wx; inst.worldY2 = wy; inst.worldZ2 = wz;
                    } else if (builder.lastSlot == 2) {
                        inst.worldX3 = wx; inst.worldY3 = wy; inst.worldZ3 = wz;
                    } else {
                        inst.worldX4 = wx; inst.worldY4 = wy; inst.worldZ4 = wz;
                    }
                    builder.lastSlot++;
                }
#else
                builder.instances.emplace_back();
                GrassInstance &inst = builder.instances.back();
                builder.baseNormals.emplace_back();
                GrassPackedNormal &baseN = builder.baseNormals.back();

                inst.worldX = wx; inst.worldY = wy; inst.worldZ = wz;
                inst.random   = randomByte;
                inst.bladeLen = ComputeBladeLen(inst.random, in.params);
                inst.sinA     = sinTable[angByte];
                inst.cosA     = cosTable[angByte];
                baseN.x = (int8_t)(nnx * 127.0f);
                baseN.y = (int8_t)(nny * 127.0f);
                baseN.z = (int8_t)(nnz * 127.0f);
                inst.tiltX = 0; inst.tiltY = 0; inst.tiltZ = 127;
                inst.selectionKey = hw;
                inst._pad[0] = 0; inst._pad[1] = 0;
#endif

                if (wz < builder.minZ) builder.minZ = wz;
                if (wz > builder.maxZ) builder.maxZ = wz;
            }
        }

        if (builders.empty()) {
            out.instanceCount = 0;
            out.chunks.clear();
            out.megaChunks.clear();
            out.lodSoA.Clear();
            out.lodSoA.Finalize(0);
            out.ready.store(true, std::memory_order_release);
            m_readIdx.store(ri + 1, std::memory_order_release);
            continue;
        }

        // MegaChunk hierárquico — megaArena já resetado com release() no topo do loop.
        struct MegaBuilder {
            float minX= 1e9f, minY= 1e9f, minZ= 1e9f;
            float maxX=-1e9f, maxY=-1e9f, maxZ=-1e9f;
            std::pmr::vector<int> chunkIndices;
            explicit MegaBuilder(std::pmr::memory_resource *r) : chunkIndices(r) {}
        };
        ankerl::unordered_dense::map<uint64_t, MegaBuilder> megaBuilders;
        megaBuilders.reserve((builders.size() / 64) + 1);

        for (size_t i = 0; i < builders.size(); ++i) {
            const int mcx = builders[i].ccx >> 3;
            const int mcy = builders[i].ccy >> 3;
            const uint64_t mkey = ((uint64_t)(uint32_t)mcx << 32) | (uint32_t)mcy;
            auto [itM, ins] = megaBuilders.try_emplace(mkey, megaArena.get());
            if (itM->second.chunkIndices.empty()) itM->second.chunkIndices.reserve(64);
            itM->second.chunkIndices.push_back((int)i);
        }

        out.chunks.reserve(builders.size());
        out.lodSoA.Clear();
        out.lodSoA.Reserve(builders.size());
        out.megaChunks.reserve(megaBuilders.size());

        size_t totalInstances = 0;
        for (const auto &b : builders) totalInstances += b.instances.size();

        out.instances.resize(totalInstances);
        out.baseNormals.resize(totalInstances);

        std::vector<GrassInstance>     radixScratch;
        std::vector<GrassPackedNormal> radixScratchNormals;
        size_t nextOffset = 0;

        const float bladeLenMax  = in.params.lengthMax * in.params.heightScale;
        const float lateralPad   = in.params.scale + bladeLenMax + std::fabs(in.params.strength);

        for (auto &kv : megaBuilders) {
            MegaBuilder &mb = kv.second;
            GrassMegaChunk mc;
            mc.firstChunk = (int)out.chunks.size();
            mc.chunkCount = (int)mb.chunkIndices.size();

            for (int ci : mb.chunkIndices) {
                ChunkBuilder &cb = builders[(size_t)ci];
                if (cb.instances.empty()) continue;

                const size_t count = cb.instances.size();
                RadixSortBySelectionKey(cb.instances.data(), cb.baseNormals.data(), count,
                                        radixScratch, radixScratchNormals);
                std::memcpy(out.instances.data() + nextOffset,   cb.instances.data(),   count * sizeof(GrassInstance));
                std::memcpy(out.baseNormals.data() + nextOffset, cb.baseNormals.data(), count * sizeof(GrassPackedNormal));

                GrassChunk chunk;
                chunk.ccx = cb.ccx; chunk.ccy = cb.ccy;
                chunk.baseInstance = (int)nextOffset;
                chunk.instanceCount = (int)count;
                const float cellMinX = (float)cb.ccx * CHUNK_SIZE;
                const float cellMinY = (float)cb.ccy * CHUNK_SIZE;
                chunk.minX = cellMinX - lateralPad;
                chunk.minY = cellMinY - lateralPad;
                chunk.maxX = cellMinX + CHUNK_SIZE + lateralPad;
                chunk.maxY = cellMinY + CHUNK_SIZE + lateralPad;
                chunk.minZ = cb.minZ;
                chunk.baseMaxZ = cb.maxZ;
                chunk.maxZ = cb.maxZ + bladeLenMax;
                chunk.lastLODCount = (unsigned int)count;
                chunk.bladeLenVersion = in.bladeLenVersion;
                
                // ============================================================
                // Criar sub-chunks (subdivisão 2x2 = 4 sub-chunks por chunk)
                // ============================================================
                chunk.firstSubChunk = (int)out.subChunks.size();
                const float subChunkSizeX = CHUNK_SIZE * 0.5f;
                const float subChunkSizeY = CHUNK_SIZE * 0.5f;
                
                // Sub-dividir instâncias por quadrante espacial
                std::array<std::vector<int>, 4> subChunkInstanceIndices;
                
                for (size_t i = 0; i < count; ++i) {
                    const GrassInstance &inst = out.instances[nextOffset + i];
                    const float relX = inst.worldX - cellMinX;
                    const float relY = inst.worldY - cellMinY;
                    
                    int subX = (relX < subChunkSizeX) ? 0 : 1;
                    int subY = (relY < subChunkSizeY) ? 0 : 1;
                    int subIdx = subY * 2 + subX;
                    
                    subChunkInstanceIndices[subIdx].push_back((int)i);
                }
                
                // Criar sub-chunks apenas para quadrantes com instâncias
                int validSubChunks = 0;
                for (int subIdx = 0; subIdx < 4; ++subIdx) {
                    if (subChunkInstanceIndices[subIdx].empty()) continue;
                    
                    int subX = subIdx % 2;
                    int subY = subIdx / 2;
                    
                    GrassSubChunk subChunk;
                    subChunk.baseInstance = (int)(nextOffset + subChunkInstanceIndices[subIdx][0]);
                    subChunk.instanceCount = (int)subChunkInstanceIndices[subIdx].size();
                    
                    // Calcular AABB do sub-chunk
                    float subMinX = cellMinX + subX * subChunkSizeX;
                    float subMinY = cellMinY + subY * subChunkSizeY;
                    float subMaxX = subMinX + subChunkSizeX;
                    float subMaxY = subMinY + subChunkSizeY;
                    
                    subChunk.minX = subMinX - lateralPad;
                    subChunk.minY = subMinY - lateralPad;
                    subChunk.minZ = chunk.minZ;
                    subChunk.maxX = subMaxX + lateralPad;
                    subChunk.maxY = subMaxY + lateralPad;
                    subChunk.maxZ = chunk.maxZ;
                    
                    out.subChunks.push_back(subChunk);
                    validSubChunks++;
                }
                
                chunk.subChunkCount = validSubChunks;
                
                out.chunks.push_back(chunk);
                out.lodSoA.PushChunk(chunk.minX, chunk.minY, chunk.minZ,
                                     chunk.maxX, chunk.maxY, chunk.maxZ,
                                     cellMinX + 0.5f * CHUNK_SIZE,
                                     cellMinY + 0.5f * CHUNK_SIZE,
                                     (uint32_t)count, (uint32_t)count,
                                     0u, 0u);

                mb.minX = std::min(mb.minX, chunk.minX);
                mb.minY = std::min(mb.minY, chunk.minY);
                mb.minZ = std::min(mb.minZ, chunk.minZ);
                mb.maxX = std::max(mb.maxX, chunk.maxX);
                mb.maxY = std::max(mb.maxY, chunk.maxY);
                mb.maxZ = std::max(mb.maxZ, chunk.maxZ);
                nextOffset += count;
            }

            mc.minX = mb.minX; mc.minY = mb.minY; mc.minZ = mb.minZ;
            mc.maxX = mb.maxX; mc.maxY = mb.maxY; mc.maxZ = mb.maxZ;
            out.megaChunks.push_back(mc);
        }

        out.instances.resize(nextOffset);
        out.baseNormals.resize(nextOffset);
        out.instanceCount = (int)nextOffset;
        out.lodSoA.Finalize((int)out.chunks.size());

        // Sinaliza resultado pronto — main thread pode consumir agora
        out.ready.store(true, std::memory_order_release);
        m_readIdx.store(ri + 1, std::memory_order_release);
    }
}

// ============================================================================

void KX_GrassSystem::ProcessFinishedJobs()
{
    // Percorre os slots na ordem de envio a partir de m_doneIdx.
    // Para quando encontra um slot ainda não pronto (preserva ordem).
    const int wi = m_writeIdx.load(std::memory_order_acquire);

    while (m_doneIdx < wi) {
        JobSlot &slot = m_jobSlots[m_doneIdx % kJobRingSize];
        GrassJobResult &job = slot.result;

        if (!job.ready.load(std::memory_order_acquire))
            break; // worker ainda não terminou este slot

        KX_GameObject *terrain = job.terrain;
        auto it = m_terrainData.find(terrain);
        if (it == m_terrainData.end()) {
            // Terrain foi removido enquanto job estava em voo — descarta
            job.instances.clear();
            job.baseNormals.clear();
            job.chunks.clear();
            job.lodSoA.Clear();
            job.megaChunks.clear();
            ++m_doneIdx;
            continue;
        }

        TerrainGrassData &data = it->second;
        if (data.terrainToken != job.terrainToken || data.buildEpoch != job.buildEpoch) {
            job.instances.clear();
            job.baseNormals.clear();
            job.chunks.clear();
            job.lodSoA.Clear();
            job.megaChunks.clear();
            ++m_doneIdx;
            continue;
        }
        data.chunks    = std::move(job.chunks);
        data.lodSoA    = std::move(job.lodSoA);
        data.megaChunks = std::move(job.megaChunks);
        data.subChunks = std::move(job.subChunks);
        data.baseNormals = std::move(job.baseNormals);

        if (job.instanceCount > 0) {
            // instanceCount é setado ANTES de AppendToGPUBuffer.
            // Se o AllocateVBO falhar e retornar cedo, instanceCount já reflete
            // o tamanho real de baseNormals — evita descompasso entre os dois
            // que causava violação de acesso no loop do normalSSBO.
            data.instanceCount = job.instanceCount;
            AppendToGPUBuffer(job.instances, data);
        }
        else {
            data.instanceCount = 0;
            data.vboOffset = 0;
        }
        // tiltVersion=0 força o compute shader a recalcular tilt na 1ª passagem,
        // igual ao que BuildTerrainData faz diretamente.
        data.tiltVersion = 0;

#if KX_GRASS_ENABLE_COMPUTE
        if (data.normalSSBO == 0) glCreateBuffers(1, &data.normalSSBO);
        if (data.normalSSBO && data.instanceCount > 0) {
            const int normalCount = (int)data.baseNormals.size();
            // Usa o mínimo entre instanceCount e baseNormals.size() para evitar
            // acesso fora dos bounds se o worker gerou contagens inconsistentes.
            const int packCount = std::min(data.instanceCount, normalCount);
            if (packCount > 0) {
                std::vector<uint32_t> packed((size_t)packCount);
                for (int i = 0; i < packCount; ++i)
                    packed[(size_t)i] = PackNormalBytes(data.baseNormals[(size_t)i]);
                glNamedBufferData(data.normalSSBO,
                                  (GLsizeiptr)(packed.size() * sizeof(uint32_t)),
                                  packed.data(), GL_STATIC_DRAW);
            }
        }
#endif
        // Limpa o slot para reutilização
        job.instances.clear();
        job.baseNormals.clear();

        m_commandLayoutDirty = true;
        ++m_doneIdx;
    }
}

// ============================================================================

void KX_GrassSystem::SetTexture(int slot, RAS_Texture *tex)
{
    if (slot < 0 || slot >= 4) return;

    RAS_Texture *old = m_textures[slot];
    if (old == tex) return;

    // Torna o handle anterior não-residente antes de substituir
    if (slot == 0 && m_hTexNoise != 0) {
        glMakeTextureHandleNonResidentARB(m_hTexNoise);
        m_hTexNoise = 0;
        m_lightBlockCPU.hTexNoise = 0;
    }
    else if (slot == 1 && m_hTexGrass0 != 0) {
        glMakeTextureHandleNonResidentARB(m_hTexGrass0);
        m_hTexGrass0 = 0;
        m_lightBlockCPU.hTexGrass0 = 0;
    }

    m_textures[slot] = tex;
    m_texturesDirty = true; // força AcquireBindlessHandles() no próximo Draw()
}

// Cria ou revalida os handles bindless para texNoise (slot 0) e texGrass0 (slot 1).
// Retorna false se algum handle obrigatório não pôde ser obtido.
// Chamada apenas quando m_texturesDirty=true; após handles adquiridos a flag é limpa.
bool KX_GrassSystem::AcquireBindlessHandles()
{
    bool ok = true;

    // Slot 0 — texNoise: só processa se handle ainda não existe
    if (m_hTexNoise == 0) {
        if (m_textures[0] && m_textures[0]->Ok()) {
            const GLuint texId = (GLuint)m_textures[0]->GetBindCode();
            if (texId != 0) {
                m_hTexNoise = glGetTextureHandleARB(texId);
                if (m_hTexNoise != 0) {
                    glMakeTextureHandleResidentARB(m_hTexNoise);
                    m_lightBlockCPU.hTexNoise = m_hTexNoise;
                }
                else { ok = false; }
            }
            else { ok = false; }
        }
        else { ok = false; }
    }

    // Slot 1 — texGrass0: só processa se handle ainda não existe
    if (m_hTexGrass0 == 0) {
        if (m_textures[1] && m_textures[1]->Ok()) {
            const GLuint texId = (GLuint)m_textures[1]->GetBindCode();
            if (texId != 0) {
                m_hTexGrass0 = glGetTextureHandleARB(texId);
                if (m_hTexGrass0 != 0) {
                    glMakeTextureHandleResidentARB(m_hTexGrass0);
                    m_lightBlockCPU.hTexGrass0 = m_hTexGrass0;
                }
                else { ok = false; }
            }
            else { ok = false; }
        }
        else { ok = false; }
    }

    // Limpa a flag quando ambos os handles estão válidos
    if (m_hTexNoise != 0 && m_hTexGrass0 != 0)
        m_texturesDirty = false;

    return ok;
}

void KX_GrassSystem::SetTextureObject(KX_GameObject *obj)
{
    m_textureObject = obj;
    if (obj) {
        KX_BlenderMaterial *mat = obj->GetFirstBlenderMaterial();
        if (mat) {
            for (int i = 0; i < 4; ++i) {
                RAS_Texture *tex = mat->GetTexture(i + 3);
                if (tex && tex->Ok()) {
                    SetTexture(i, tex); // invalida handle bindless do slot se mudou
                }
            }
        }
    }
}
void KX_GrassSystem::ClearLightMultipliers()
{
    this->m_lightMultipliers.clear();
}

void KX_GrassSystem::AddLightMultiplier(const std::string& name, float multiplier, const mt::vec3& color)
{
    GrassLightOverride override;
    override.multiplier = multiplier;
    override.color = color;
    this->m_lightMultipliers[name] = override;
}

void KX_GrassSystem::SetParams(const GrassParams &p)
{
    const GrassParams old = m_params;
    m_params = p;

    const bool bladeParamsChanged =
        old.scale != p.scale ||
        old.lengthMax != p.lengthMax ||
        old.lengthMin != p.lengthMin ||
        old.heightScale != p.heightScale;

    const bool boundsParamsChanged =
        old.scale != p.scale ||
        old.lengthMax != p.lengthMax ||
        old.heightScale != p.heightScale ||
        old.strength != p.strength;

    const bool uniformsChanged =
        old.strength != p.strength ||
        old.range != p.range ||
        old.distance != p.distance;

    const bool tiltParamsChanged = (old.flatness != p.flatness);

    m_boundsDirty |= boundsParamsChanged;
    if (bladeParamsChanged) {
        ++m_bladeLenVersion;
        if (m_bladeLenVersion == 0) {
            m_bladeLenVersion = 1;
        }
    }
    if (tiltParamsChanged) {
        ++m_tiltVersion;
        if (m_tiltVersion == 0) {
            m_tiltVersion = 1;
        }
    }
    m_paramsChanged |= uniformsChanged;
    m_forceCullingUpdate |= bladeParamsChanged || boundsParamsChanged || (old.distance != p.distance);
}

// ============================================================================
// Fase 1 — BuildTerrainData
// Lê mesh do terreno, gera instâncias agrupadas diretamente em chunks.
// ============================================================================

TerrainGrassData* KX_GrassSystem::BuildTerrainData(KX_GameObject *terrain)
{
    auto it = m_terrainData.find(terrain);
    if (it == m_terrainData.end()) return nullptr;
    TerrainGrassData &data = it->second;

    const std::vector<KX_Mesh *> &meshes = terrain->GetMeshList();
    if (meshes.empty()) return &data;

    const mt::mat3x4 &worldMat = terrain->NodeGetWorldTransform();
    RAS_Mesh *mesh = static_cast<RAS_Mesh *>(meshes[0]);
    const unsigned int polyCount = mesh->GetNumPolygons();
    const float densityScale = 0.15f;
    // Densidade negativa reduz na proporção inversa: density=-N equivale a 1/N gramas por unidade
    const float effectiveDensity     = (m_params.density >= 0)
        ? (float)m_params.density
        : (m_params.density != 0 ? 1.0f / (float)(-m_params.density) : 0.0f);
    const float densityPerSquareMeter = effectiveDensity * densityScale;
    const float invChunkSize = 1.0f / CHUNK_SIZE;

    const int8_t *sinTable = nullptr;
    const int8_t *cosTable = nullptr;
    GetSinCosTable(sinTable, cosTable);
    const float *sqrtLut = GetSqrtLut();

    terrain->UpdateBounds(true);
    mt::vec3 localAabbMin;
    mt::vec3 localAabbMax;
    terrain->GetBoundsAabb(localAabbMin, localAabbMax);

    const mt::vec3 corners[8] = {
        mt::vec3(localAabbMin.x, localAabbMin.y, localAabbMin.z),
        mt::vec3(localAabbMin.x, localAabbMin.y, localAabbMax.z),
        mt::vec3(localAabbMin.x, localAabbMax.y, localAabbMin.z),
        mt::vec3(localAabbMin.x, localAabbMax.y, localAabbMax.z),
        mt::vec3(localAabbMax.x, localAabbMin.y, localAabbMin.z),
        mt::vec3(localAabbMax.x, localAabbMin.y, localAabbMax.z),
        mt::vec3(localAabbMax.x, localAabbMax.y, localAabbMin.z),
        mt::vec3(localAabbMax.x, localAabbMax.y, localAabbMax.z),
    };

    mt::vec3 worldAabbMin = worldMat * corners[0];
    mt::vec3 worldAabbMax = worldAabbMin;
    for (int i = 1; i < 8; ++i) {
        const mt::vec3 w = worldMat * corners[i];
        worldAabbMin.x = std::min(worldAabbMin.x, w.x);
        worldAabbMin.y = std::min(worldAabbMin.y, w.y);
        worldAabbMin.z = std::min(worldAabbMin.z, w.z);
        worldAabbMax.x = std::max(worldAabbMax.x, w.x);
        worldAabbMax.y = std::max(worldAabbMax.y, w.y);
        worldAabbMax.z = std::max(worldAabbMax.z, w.z);
    }

    const int minChunkX = (int)std::floor(worldAabbMin.x / CHUNK_SIZE);
    const int maxChunkX = (int)std::floor(worldAabbMax.x / CHUNK_SIZE);
    const int minChunkY = (int)std::floor(worldAabbMin.y / CHUNK_SIZE);
    const int maxChunkY = (int)std::floor(worldAabbMax.y / CHUNK_SIZE);
    const size_t chunkSpanX = (size_t)std::max(1, maxChunkX - minChunkX + 1);
    const size_t chunkSpanY = (size_t)std::max(1, maxChunkY - minChunkY + 1);
    const size_t estimatedChunkCount = chunkSpanX * chunkSpanY;


    std::pmr::monotonic_buffer_resource chunkArena;
    std::pmr::memory_resource *chunkResource = &chunkArena;
    struct ChunkBuilder {
        std::pmr::vector<GrassInstance> instances;
        std::pmr::vector<GrassPackedNormal> baseNormals;
        float minZ = 1e9f;
        float maxZ = -1e9f;
        int32_t ccx = 0;
        int32_t ccy = 0;
#if KX_GRASS_DENSITY_SHADER_MERGE
        int lastSlot = 4;
#endif

        explicit ChunkBuilder(std::pmr::memory_resource *resource)
            : instances(resource),
              baseNormals(resource)
        {
        }
    };

    std::vector<int> chunkIndexGrid;
    chunkIndexGrid.assign(estimatedChunkCount, -1);
    std::vector<ChunkBuilder> builders;
    ankerl::unordered_dense::map<uint64_t, int> chunkIndexFallback;
    chunkIndexFallback.reserve(64);

    for (unsigned int p = 0; p < polyCount; ++p) {
        RAS_Mesh::PolygonInfo poly = mesh->GetPolygon(p);
        if (!poly.array || poly.array->GetVertexCount() == 0) continue;

        mt::vec3 px[3];
        bool is_surface[3] = { true, true, true };
        bool poly_valid = true;
        const unsigned int max_idx = poly.array->GetVertexCount();

        for (int v = 0; v < 3; ++v) {
            if (poly.indices[v] >= max_idx) { poly_valid = false; break; }
            const mt::vec3_packed &pos = poly.array->GetPosition(poly.indices[v]);
            px[v] = worldMat * mt::vec3(pos.x, pos.y, pos.z);
            if (poly.array->GetFormat().colorSize > 0) {
                const unsigned char(&c)[4] = poly.array->GetColor(poly.indices[v], 0);
                is_surface[v] = (c[3] > 128);
            }
        }
        if (!poly_valid) continue;

        if (!is_surface[0] && !is_surface[1] && !is_surface[2]) continue;

        const float cx = (px[0].x + px[1].x + px[2].x) / 3.0f;
        const float cy = (px[0].y + px[1].y + px[2].y) / 3.0f;

        const mt::vec3 a_vec = px[1] - px[0];
        const mt::vec3 b_vec = px[2] - px[0];
        const mt::vec3 cross = mt::vec3::CrossProduct(a_vec, b_vec);
        const float area = cross.Length() * 0.5f;
        if (area <= 0.0f) continue;

        mt::vec3 n = cross;
        if (n.z < 0.0f) n = -n;
        n.Normalize();
        const float nnx = n.x, nny = n.y, nnz = n.z;

        const int bladeCount = (int)(area * densityPerSquareMeter);
        for (int d = 0; d < bladeCount; ++d) {
            const uint32_t h = hash32f(cx, cy, (uint32_t)d);
            const float r1 = sqrtLut[(h >> 8) & 511u];
            const float r2 = u01(mix32(h + 0x9e3779b9u));

            const float w0 = 1.0f - r1;
            const float w1 = r1 * (1.0f - r2);
            const float w2 = r1 * r2;

            const float wx = px[0].x * w0 + px[1].x * w1 + px[2].x * w2;
            const float wy = px[0].y * w0 + px[1].y * w1 + px[2].y * w2;
            const float wz = px[0].z * w0 + px[1].z * w1 + px[2].z * w2;

            const uint32_t hw = hash32f(wx, wy, (uint32_t)d ^ 0x85ebca6bu);
            const uint8_t randomByte = (uint8_t)(hw & 0xFFu);
            const uint8_t angByte = (uint8_t)(mix32(hw + 0x27d4eb2du) & 0xFFu);

            const float invNoise = 0.8004f + 0.4f * u01(mix32(hw + 0x68bc21ebu));
            const float slopeBase = std::clamp((nnz - 0.85f) * 6.6666667f, 0.0f, 1.0f);
            const float vegetationWeight = std::clamp(slopeBase - 0.1f * invNoise, 0.0f, 1.0f);
            if (vegetationWeight <= 0.0f) {
                continue;
            }
            const float gate = u01(mix32(hw + 0x9e3779b9u));
            if (gate > vegetationWeight) {
                continue;
            }

            const int ccx = FastFloorToInt(wx * invChunkSize);
            const int ccy = FastFloorToInt(wy * invChunkSize);

            int *builderIndexPtr = nullptr;
            const int lx = ccx - minChunkX;
            const int ly = ccy - minChunkY;
            if (lx >= 0 && ly >= 0 && (size_t)lx < chunkSpanX && (size_t)ly < chunkSpanY) {
                const size_t cellIndex = (size_t)ly * chunkSpanX + (size_t)lx;
                builderIndexPtr = &chunkIndexGrid[cellIndex];
            }
            else {
                const uint64_t key = ((uint64_t)(uint32_t)ccx << 32) | (uint32_t)ccy;
                auto [itF, inserted] = chunkIndexFallback.try_emplace(key, -1);
                builderIndexPtr = &itF->second;
            }

            int &builderIndex = *builderIndexPtr;
            if (builderIndex < 0) {
                builderIndex = (int)builders.size();
                builders.emplace_back(chunkResource);
                builders.back().ccx = (int32_t)ccx;
                builders.back().ccy = (int32_t)ccy;
            }
            ChunkBuilder &builder = builders[(size_t)builderIndex];

#if KX_GRASS_DENSITY_SHADER_MERGE
            if (builder.lastSlot >= 4) {
                builder.instances.emplace_back();
                GrassInstance &inst = builder.instances.back();
                builder.baseNormals.emplace_back();
                GrassPackedNormal &baseN = builder.baseNormals.back();
                inst.worldX = wx; inst.worldY = wy; inst.worldZ = wz;
                inst.worldX2 = wx; inst.worldY2 = wy; inst.worldZ2 = wz;
                inst.worldX3 = wx; inst.worldY3 = wy; inst.worldZ3 = wz;
                inst.worldX4 = wx; inst.worldY4 = wy; inst.worldZ4 = wz;
                inst.random = randomByte;
                inst.bladeLen = ComputeBladeLen(inst.random, m_params);
                inst.sinA   = sinTable[angByte];
                inst.cosA   = cosTable[angByte];
                baseN.x = (int8_t)(nnx * 127.0f);
                baseN.y = (int8_t)(nny * 127.0f);
                baseN.z = (int8_t)(nnz * 127.0f);
                inst.tiltX = 0;
                inst.tiltY = 0;
                inst.tiltZ = 127;
                inst.selectionKey = hw;
                inst._pad[0] = 0;
                inst._pad[1] = 0;
                builder.lastSlot = 1;
            }
            else {
                GrassInstance &inst = builder.instances.back();
                if (builder.lastSlot == 1) {
                    inst.worldX2 = wx; inst.worldY2 = wy; inst.worldZ2 = wz;
                } else if (builder.lastSlot == 2) {
                    inst.worldX3 = wx; inst.worldY3 = wy; inst.worldZ3 = wz;
                } else {
                    inst.worldX4 = wx; inst.worldY4 = wy; inst.worldZ4 = wz;
                }
                builder.lastSlot++;
            }
#else
            builder.instances.emplace_back();
            GrassInstance &inst = builder.instances.back();
            builder.baseNormals.emplace_back();
            GrassPackedNormal &baseN = builder.baseNormals.back();
            inst.worldX = wx; inst.worldY = wy; inst.worldZ = wz;
            inst.random = randomByte;
            inst.bladeLen = ComputeBladeLen(inst.random, m_params);
            inst.sinA   = sinTable[angByte];
            inst.cosA   = cosTable[angByte];
            baseN.x = (int8_t)(nnx * 127.0f);
            baseN.y = (int8_t)(nny * 127.0f);
            baseN.z = (int8_t)(nnz * 127.0f);
            inst.tiltX = 0;
            inst.tiltY = 0;
            inst.tiltZ = 127;
            inst.selectionKey = hw;
            inst._pad[0] = 0;
            inst._pad[1] = 0;
#endif

            if (wz < builder.minZ) builder.minZ = wz;
            if (wz > builder.maxZ) builder.maxZ = wz;
        }
    }

    data.chunks.clear();
    data.lodSoA.Clear();
    data.megaChunks.clear();
    data.subChunks.clear();
    data.baseNormals.clear();

    if (builders.empty()) {
        data.instanceCount = 0;
        data.vboOffset = 0;
        data.tiltVersion = m_tiltVersion;
        data.lodSoA.Finalize(0);
        m_commandLayoutDirty = true;
        return &data;
    }

    std::pmr::monotonic_buffer_resource arena;
    struct MegaBuilder {
        float minX = 1e9f, minY = 1e9f, minZ = 1e9f;
        float maxX = -1e9f, maxY = -1e9f, maxZ = -1e9f;
        std::pmr::vector<int> chunkIndices;

        MegaBuilder()
            : chunkIndices(std::pmr::get_default_resource())
        {
        }

        explicit MegaBuilder(std::pmr::memory_resource *resource)
            : chunkIndices(resource)
        {
        }
    };
    ankerl::unordered_dense::map<uint64_t, MegaBuilder> megaBuilders;
    megaBuilders.reserve((builders.size() / 64) + 1);

    for (size_t i = 0; i < builders.size(); ++i) {
        const int ccx = builders[i].ccx;
        const int ccy = builders[i].ccy;

        const int mcx = ccx >> 3;
        const int mcy = ccy >> 3;
        const uint64_t mkey = ((uint64_t)(uint32_t)mcx << 32) | (uint32_t)mcy;
        auto [itM, inserted] = megaBuilders.try_emplace(mkey, &arena);
        MegaBuilder &mb = itM->second;
        if (mb.chunkIndices.empty()) {
            mb.chunkIndices.reserve(64);
        }
        mb.chunkIndices.push_back((int)i);
    }

    data.chunks.reserve(builders.size());
    data.lodSoA.Clear();
    data.lodSoA.Reserve(builders.size());
    data.megaChunks.reserve(megaBuilders.size());

    size_t totalInstances = 0;
    for (const ChunkBuilder &b : builders) {
        totalInstances += b.instances.size();
    }

    std::vector<GrassInstance> terrainInstances;
    terrainInstances.resize(totalInstances);
    std::vector<GrassPackedNormal> terrainBaseNormals;
    terrainBaseNormals.resize(totalInstances);

    std::vector<GrassInstance> radixScratch;
    std::vector<GrassPackedNormal> radixScratchNormals;
    size_t nextOffset = 0;

    for (auto &kv : megaBuilders) {
        MegaBuilder &mb = kv.second;
        GrassMegaChunk mc;
        mc.firstChunk = (int)data.chunks.size();
        mc.chunkCount = (int)mb.chunkIndices.size();

        for (int chunkIndex : mb.chunkIndices) {
            ChunkBuilder &cb = builders[(size_t)chunkIndex];
            if (cb.instances.size() == 0) {
                continue;
            }

            const int ccx = cb.ccx;
            const int ccy = cb.ccy;
            const size_t count = cb.instances.size();

            RadixSortBySelectionKey(cb.instances.data(), cb.baseNormals.data(), count, radixScratch, radixScratchNormals);
            std::memcpy(terrainInstances.data() + nextOffset, cb.instances.data(), count * sizeof(GrassInstance));
            std::memcpy(terrainBaseNormals.data() + nextOffset, cb.baseNormals.data(), count * sizeof(GrassPackedNormal));

            GrassChunk chunk;
            chunk.ccx = ccx;
            chunk.ccy = ccy;
            chunk.baseInstance = (int)nextOffset;
            chunk.instanceCount = (int)count;
            const float cellMinX = (float)ccx * CHUNK_SIZE;
            const float cellMinY = (float)ccy * CHUNK_SIZE;
            const float bladeLen = m_params.lengthMax * m_params.heightScale;

            const float lateralPad = m_params.scale + bladeLen + std::fabs(m_params.strength);
            chunk.minX = cellMinX - lateralPad;
            chunk.minY = cellMinY - lateralPad;
            chunk.maxX = cellMinX + CHUNK_SIZE + lateralPad;
            chunk.maxY = cellMinY + CHUNK_SIZE + lateralPad;
            chunk.minZ = cb.minZ;
            chunk.baseMaxZ = cb.maxZ;
            chunk.maxZ = cb.maxZ + bladeLen;
            chunk.lastLODCount = (unsigned int)chunk.instanceCount;
            chunk.bladeLenVersion = m_bladeLenVersion;

            // ============================================================
            // Criar sub-chunks (subdivisão 2x2 = 4 sub-chunks por chunk)
            // ============================================================
            chunk.firstSubChunk = (int)data.subChunks.size();
            const float subChunkSizeX = CHUNK_SIZE * 0.5f;
            const float subChunkSizeY = CHUNK_SIZE * 0.5f;
            
            // Sub-dividir instâncias por quadrante espacial
            std::array<std::vector<int>, 4> subChunkInstanceIndices;
            
            for (int i = 0; i < (int)count; ++i) {
                const GrassInstance &inst = terrainInstances[nextOffset + i];
                const float relX = inst.worldX - cellMinX;
                const float relY = inst.worldY - cellMinY;
                
                int subX = (relX < subChunkSizeX) ? 0 : 1;
                int subY = (relY < subChunkSizeY) ? 0 : 1;
                int subIdx = subY * 2 + subX;
                
                subChunkInstanceIndices[subIdx].push_back(i);
            }
            
            // Criar sub-chunks apenas para quadrantes com instâncias
            int validSubChunks = 0;
            for (int subIdx = 0; subIdx < 4; ++subIdx) {
                if (subChunkInstanceIndices[subIdx].empty()) continue;
                
                int subX = subIdx % 2;
                int subY = subIdx / 2;
                
                GrassSubChunk subChunk;
                subChunk.baseInstance = (int)(nextOffset + subChunkInstanceIndices[subIdx][0]);
                subChunk.instanceCount = (int)subChunkInstanceIndices[subIdx].size();
                
                // Calcular AABB do sub-chunk
                float subMinX = cellMinX + subX * subChunkSizeX;
                float subMinY = cellMinY + subY * subChunkSizeY;
                float subMaxX = subMinX + subChunkSizeX;
                float subMaxY = subMinY + subChunkSizeY;
                
                subChunk.minX = subMinX - lateralPad;
                subChunk.minY = subMinY - lateralPad;
                subChunk.minZ = chunk.minZ;
                subChunk.maxX = subMaxX + lateralPad;
                subChunk.maxY = subMaxY + lateralPad;
                subChunk.maxZ = chunk.maxZ;
                
                data.subChunks.push_back(subChunk);
                validSubChunks++;
            }
            
            chunk.subChunkCount = validSubChunks;

            data.chunks.push_back(chunk);
            data.lodSoA.PushChunk(chunk.minX, chunk.minY, chunk.minZ,
                                  chunk.maxX, chunk.maxY, chunk.maxZ,
                                  cellMinX + 0.5f * CHUNK_SIZE,
                                  cellMinY + 0.5f * CHUNK_SIZE,
                                  (uint32_t)count, (uint32_t)count,
                                  0u, 0u);
            nextOffset += count;

            mb.minX = std::min(mb.minX, chunk.minX);
            mb.minY = std::min(mb.minY, chunk.minY);
            mb.minZ = std::min(mb.minZ, chunk.minZ);
            mb.maxX = std::max(mb.maxX, chunk.maxX);
            mb.maxY = std::max(mb.maxY, chunk.maxY);
            mb.maxZ = std::max(mb.maxZ, chunk.maxZ);
        }

        mc.minX = mb.minX; mc.minY = mb.minY; mc.minZ = mb.minZ;
        mc.maxX = mb.maxX; mc.maxY = mb.maxY; mc.maxZ = mb.maxZ;
        data.megaChunks.push_back(mc);
    }

    terrainInstances.resize(nextOffset);
    terrainBaseNormals.resize(nextOffset);
    data.lodSoA.Finalize((int)data.chunks.size());

    data.instanceCount = (int)terrainInstances.size();
    data.baseNormals = std::move(terrainBaseNormals);
    data.tiltVersion = 0;
    AppendToGPUBuffer(terrainInstances, data);
    m_commandLayoutDirty = true;

#if KX_GRASS_ENABLE_COMPUTE
    if (data.normalSSBO == 0) {
        glCreateBuffers(1, &data.normalSSBO);
    }
    if (data.normalSSBO) {
        std::vector<uint32_t> packed;
        packed.resize((size_t)data.instanceCount);
        for (int i = 0; i < data.instanceCount; ++i) {
            packed[(size_t)i] = PackNormalBytes(data.baseNormals[(size_t)i]);
        }
        glNamedBufferData(data.normalSSBO, (GLsizeiptr)(packed.size() * sizeof(uint32_t)), packed.data(), GL_STATIC_DRAW);
    }
#endif

    return &data;
}

// ============================================================================
// EnsureGPUResources — Shader, VAO e VBO inicial
// ============================================================================

void KX_GrassSystem::EnsureGPUResources()
{
    if (m_prog) return;

    m_prog = GPU_shader_create(s_vert, s_frag, nullptr, nullptr, nullptr, 0, 0, 0);
    if (!m_prog) return;

    const GLuint prog = (GLuint)GPU_shader_program(m_prog);

    // Vincula o UBO "GrassLightBlock" ao binding point 2 (era 0, mudado para evitar conflito com SceneLightBlock)
    const GLuint blockIndex = glGetUniformBlockIndex(prog, "GrassLightBlock");
    if (blockIndex != GL_INVALID_INDEX)
        glUniformBlockBinding(prog, blockIndex, 2);

    // Cria o UBO persistente — contém timer, camPos, luzes, escalares de vento e handles bindless
    glCreateBuffers(1, &m_lightUBO);
    glNamedBufferData(m_lightUBO, sizeof(GrassLightBlock), nullptr, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 2, m_lightUBO);

    // Todos os escalares e handles de textura foram movidos para o UBO.
    // Nenhum glProgramUniform de sampler/scalar necessário aqui.

    UploadStaticUniforms();

    // Geometria da blade — cross-billboard:
    // Modo normal:  2 triângulos × 3 vértices × 2 floats = 12 floats (6 vértices por instância)
    // VBO de instâncias inicial
    m_vboCapacity = 1024 * 128; // 128k instâncias iniciais
    glCreateBuffers(1, &m_instanceVBO);
    const GLbitfield storageFlags = GL_DYNAMIC_STORAGE_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    const GLbitfield mapFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    glNamedBufferStorage(m_instanceVBO, (GLsizeiptr)(m_vboCapacity * sizeof(GrassInstance)), nullptr, storageFlags);
    m_instancePtr = glMapNamedBufferRange(m_instanceVBO, 0, (GLsizeiptr)(m_vboCapacity * sizeof(GrassInstance)), mapFlags);
    m_hasPersistentInstance = (m_instancePtr != nullptr);

    // Inicializa lista de blocos livres
    m_freeBlocks.clear();
    m_freeBlocks.push_back({0, m_vboCapacity});
    m_totalInstances = 0;

    // bladeVert removido — geometria calculada diretamente no vertex shader via gl_VertexID
    m_locXYZ  = glGetAttribLocation(prog, "worldXYZ");
    m_locSC   = glGetAttribLocation(prog, "instSinCos");
    m_locTilt = glGetAttribLocation(prog, "instTilt");
    m_locBlen = glGetAttribLocation(prog, "instBlen");
#if KX_GRASS_DENSITY_SHADER_MERGE
    m_locXYZ2 = glGetAttribLocation(prog, "worldXYZ2");
    m_locXYZ3 = glGetAttribLocation(prog, "worldXYZ3");
    m_locXYZ4 = glGetAttribLocation(prog, "worldXYZ4");
#endif

    glCreateVertexArrays(1, &m_vao);

    // Binding 0: Instance data (geomVBO removido — sem bladeVert attribute)
    glVertexArrayVertexBuffer(m_vao, 0, m_instanceVBO, 0, sizeof(GrassInstance));
    glVertexArrayBindingDivisor(m_vao, 0, 1);

    auto setupInstanceAttrib = [&](GLint loc, GLint size, GLenum type, GLboolean normalized, GLuint offset) {
        if (loc < 0) return;
        glEnableVertexArrayAttrib(m_vao, loc);
        glVertexArrayAttribFormat(m_vao, loc, size, type, normalized, offset);
        glVertexArrayAttribBinding(m_vao, loc, 0);
    };

    setupInstanceAttrib(m_locXYZ,  3, GL_FLOAT, GL_FALSE, offsetof(GrassInstance, worldX));
#if KX_GRASS_DENSITY_SHADER_MERGE
    setupInstanceAttrib(m_locXYZ2, 3, GL_FLOAT, GL_FALSE, offsetof(GrassInstance, worldX2));
    setupInstanceAttrib(m_locXYZ3, 3, GL_FLOAT, GL_FALSE, offsetof(GrassInstance, worldX3));
    setupInstanceAttrib(m_locXYZ4, 3, GL_FLOAT, GL_FALSE, offsetof(GrassInstance, worldX4));
#endif
    setupInstanceAttrib(m_locSC,   2, GL_BYTE,  GL_TRUE,  offsetof(GrassInstance, sinA));
    setupInstanceAttrib(m_locTilt, 3, GL_BYTE,  GL_TRUE,  offsetof(GrassInstance, tiltX));
    setupInstanceAttrib(m_locBlen, 1, GL_FLOAT, GL_FALSE, offsetof(GrassInstance, bladeLen));

    m_hasBaseInstance = true;
    m_hasIndirect     = true;

    ResizeIndirectBuffer(4096);

#if KX_GRASS_ENABLE_COMPUTE
    static const char *s_comp =
    "#version 430\n"
    "layout(local_size_x = 256) in;\n"
    "layout(std430, binding=0) readonly buffer Normals { uint nPacked[]; };\n"
    "layout(std430, binding=1) buffer Instances { uint inst[]; };\n"
    "uniform uint baseInstance;\n"
    "uniform uint count;\n"
    "uniform float flatness;\n"
    "float smoothstep01(float x) {\n"
    "    x = clamp(x, 0.0, 1.0);\n"
    "    return x * x * (3.0 - 2.0 * x);\n"
    "}\n"
    "vec3 computeTiltFromTerrainNormal(vec3 terrNorm, float flatness) {\n"
    "    float t = smoothstep01(terrNorm.z);\n"
    "    float f = flatness * (1.0 - t);\n"
    "    vec3 v = terrNorm * (1.0 - f) + vec3(0.0, 0.0, 1.0) * f;\n"
    "    float len2 = dot(v, v);\n"
    "    if (len2 > 1e-24) {\n"
    "        v *= inversesqrt(len2);\n"
    "    } else {\n"
    "        v = vec3(0.0, 0.0, 1.0);\n"
    "    }\n"
    "    return v;\n"
    "}\n"
    "uint replaceByte(uint word, uint byteIndex, uint value) {\n"
    "    uint shift = byteIndex * 8u;\n"
    "    uint mask = 0xFFu << shift;\n"
    "    return (word & ~mask) | ((value & 0xFFu) << shift);\n"
    "}\n"
    "int unpackS8(uint p, uint shift) {\n"
    "    int v = int((p >> shift) & 0xFFu);\n"
    "    return (v >= 128) ? (v - 256) : v;\n"
    "}\n"
    "uint packS8(float x) {\n"
    "    float c = clamp(x, -1.0, 1.0);\n"
    "    int v = int(round(c * 127.0));\n"
    "    v = clamp(v, -127, 127);\n"
    "    return uint(v & 255);\n"
    "}\n"
    "void main() {\n"
    "    uint id = gl_GlobalInvocationID.x;\n"
    "    if (id >= count) return;\n"
    "    uint p = nPacked[id];\n"
    "    vec3 terrNorm = vec3(float(unpackS8(p, 0u)), float(unpackS8(p, 8u)), float(unpackS8(p, 16u))) * (1.0 / 127.0);\n"
    "    float len2 = dot(terrNorm, terrNorm);\n"
    "    if (len2 > 1e-24) terrNorm *= inversesqrt(len2);\n"
    "    if (terrNorm.z < 0.0) terrNorm = -terrNorm;\n"
    "    vec3 tilt = computeTiltFromTerrainNormal(terrNorm, flatness);\n"
    "    uint tx = packS8(tilt.x);\n"
    "    uint ty = packS8(tilt.y);\n"
    "    uint tz = packS8(tilt.z);\n"
    "    uint g = baseInstance + id;\n"
#if KX_GRASS_DENSITY_SHADER_MERGE
    // sizeof(GrassInstance) = 64 bytes = 16 uint32s
    // tiltX @ byte 55 = word13, byteIndex 3
    // tiltY @ byte 56 = word14, byteIndex 0
    // tiltZ @ byte 57 = word14, byteIndex 1
    "    uint word0 = (g * 64u) >> 2;\n"
    "    uint w10 = inst[word0 + 13u];\n"
    "    w10 = replaceByte(w10, 3u, tx);\n"
    "    inst[word0 + 13u] = w10;\n"
    "    uint w11 = inst[word0 + 14u];\n"
    "    w11 = replaceByte(w11, 0u, ty);\n"
    "    w11 = replaceByte(w11, 1u, tz);\n"
    "    inst[word0 + 14u] = w11;\n"
#else
    // sizeof(GrassInstance) = 28 bytes = 7 uint32s
    // tiltX @ byte 19 = word4, byteIndex 3
    // tiltY @ byte 20 = word5, byteIndex 0
    // tiltZ @ byte 21 = word5, byteIndex 1
    "    uint word0 = (g * 28u) >> 2;\n"
    "    uint w4 = inst[word0 + 4u];\n"
    "    w4 = replaceByte(w4, 3u, tx);\n"
    "    inst[word0 + 4u] = w4;\n"
    "    uint w5 = inst[word0 + 5u];\n"
    "    w5 = replaceByte(w5, 0u, ty);\n"
    "    w5 = replaceByte(w5, 1u, tz);\n"
    "    inst[word0 + 5u] = w5;\n"
#endif
    "}\n";

    GLuint cs = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(cs, 1, &s_comp, nullptr);
    glCompileShader(cs);
    GLint ok = GL_FALSE;
    glGetShaderiv(cs, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE) {
        m_computeProg = glCreateProgram();
        glAttachShader(m_computeProg, cs);
        glLinkProgram(m_computeProg);
        GLint linkOk = GL_FALSE;
        glGetProgramiv(m_computeProg, GL_LINK_STATUS, &linkOk);
        if (linkOk != GL_TRUE) {
            glDeleteProgram(m_computeProg);
            m_computeProg = 0;
        }
    }
    glDeleteShader(cs);

    if (m_computeProg) {
        m_uComputeBaseInstance = glGetUniformLocation(m_computeProg, "baseInstance");
        m_uComputeCount = glGetUniformLocation(m_computeProg, "count");
        m_uComputeFlatness = glGetUniformLocation(m_computeProg, "flatness");
    }
#endif
}

// ============================================================================
// ResizeIndirectBuffer — Expande o buffer de comandos indiretos
// ============================================================================

void KX_GrassSystem::ResizeIndirectBuffer(int newCapacity)
{
    if (newCapacity <= m_indirectCapacity) return;

    GLsizeiptr bufSize = (GLsizeiptr)newCapacity * sizeof(DrawArraysIndirectCommand);

    for (int i = 0; i < kIndirectRingSize; ++i) {
        WaitIndirectSlotFence(m_indirectFence[i]);
        DeleteIndirectSlotFence(m_indirectFence[i]);
        if (m_indirectVBO[i]) {
            if (m_indirectPtr[i]) {
                glUnmapNamedBuffer(m_indirectVBO[i]);
                m_indirectPtr[i] = nullptr;
            }
            glDeleteBuffers(1, &m_indirectVBO[i]);
            m_indirectVBO[i] = 0;
        }

        glCreateBuffers(1, &m_indirectVBO[i]);
#if KX_GRASS_PERSISTENT_INDIRECT
        const GLbitfield storageFlags = GL_DYNAMIC_STORAGE_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        const GLbitfield mapFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        glNamedBufferStorage(m_indirectVBO[i], bufSize, nullptr, storageFlags);
        m_indirectPtr[i] = (DrawArraysIndirectCommand*)glMapNamedBufferRange(m_indirectVBO[i], 0, bufSize, mapFlags);
#else
        glNamedBufferStorage(m_indirectVBO[i], bufSize, nullptr, GL_DYNAMIC_STORAGE_BIT);
        m_indirectPtr[i] = nullptr;
#endif
        m_indirectSlotVersion[i] = 0;
    }

    m_indirectWriteIndex = 0;
    m_indirectCapacity = newCapacity;
}

// ============================================================================
// Gestão de memória VBO (Free List)
// ============================================================================

bool KX_GrassSystem::AllocateVBO(int count, int &outOffset)
{
    // First-fit
    for (size_t i = 0; i < m_freeBlocks.size(); ++i) {
        if (m_freeBlocks[i].size >= count) {
            outOffset = m_freeBlocks[i].offset;
            m_freeBlocks[i].offset += count;
            m_freeBlocks[i].size -= count;
            
            // Atualiza o maior offset usado
            if (outOffset + count > m_highestUsedOffset) {
                m_highestUsedOffset = outOffset + count;
            }

            // Remove bloco se ficou vazio
            if (m_freeBlocks[i].size == 0) {
                m_freeBlocks.erase(m_freeBlocks.begin() + i);
            }
            return true;
        }
    }
    return false;
}

void KX_GrassSystem::DeallocateVBO(int offset, int count)
{
    if (count <= 0) return;

    m_totalInstances -= count;

    // Insere mantendo ordem de offset para facilitar merge
    GrassFreeBlock newBlock = {offset, count};
    auto it = std::lower_bound(m_freeBlocks.begin(), m_freeBlocks.end(), newBlock, 
        [](const GrassFreeBlock &a, const GrassFreeBlock &b) {
            return a.offset < b.offset;
        });
    
    it = m_freeBlocks.insert(it, newBlock);

    // Merge com próximo
    auto next = std::next(it);
    if (next != m_freeBlocks.end() && it->offset + it->size == next->offset) {
        it->size += next->size;
        m_freeBlocks.erase(next);
    }

    // Merge com anterior
    if (it != m_freeBlocks.begin()) {
        auto prev = std::prev(it);
        if (prev->offset + prev->size == it->offset) {
            prev->size += it->size;
            m_freeBlocks.erase(it);
            it = prev; // Atualiza it para o bloco fundido
        }
    }
}

// ============================================================================
// AppendToGPUBuffer — Upload incremental com reuso de memória
// ============================================================================

void KX_GrassSystem::AppendToGPUBuffer(const std::vector<GrassInstance> &instances, TerrainGrassData &data)
{
    if (instances.size() == 0) return;

    int count = (int)instances.size();
    int offset = 0;

    if (!AllocateVBO(count, offset)) {
        // Sem espaço: Grow VBO
        int oldCapacity = m_vboCapacity;
        int newCapacity = std::max(m_vboCapacity * 2, m_highestUsedOffset + count + 1024);
        
        GLuint newVBO;
        glCreateBuffers(1, &newVBO);
        const GLbitfield storageFlags = GL_DYNAMIC_STORAGE_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        glNamedBufferStorage(newVBO, (GLsizeiptr)(newCapacity * sizeof(GrassInstance)), nullptr, storageFlags);

        // Copia apenas o range realmente utilizado
        if (m_instanceVBO != 0 && m_highestUsedOffset > 0) {
            if (m_instancePtr) {
                glUnmapNamedBuffer(m_instanceVBO);
                m_instancePtr = nullptr;
            }
            glCopyNamedBufferSubData(m_instanceVBO, newVBO, 0, 0, (GLsizeiptr)(m_highestUsedOffset * sizeof(GrassInstance)));
            glDeleteBuffers(1, &m_instanceVBO);
        }

        const GLbitfield mapFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        void *newPtr = glMapNamedBufferRange(newVBO, 0, (GLsizeiptr)(newCapacity * sizeof(GrassInstance)), mapFlags);

        m_instanceVBO = newVBO;
        m_instancePtr = newPtr;
        m_vboCapacity = newCapacity;

        // Atualiza free list
        bool expanded = false;
        if (!m_freeBlocks.empty()) {
            GrassFreeBlock &last = m_freeBlocks.back();
            if (last.offset + last.size == oldCapacity) {
                last.size += (newCapacity - oldCapacity);
                expanded = true;
            }
        }
        if (!expanded) {
            m_freeBlocks.push_back({oldCapacity, newCapacity - oldCapacity});
        }

        // Tenta alocar novamente
        if (!AllocateVBO(count, offset)) return;

        // Atualiza VAO para apontar para o novo VBO (binding 0 agora é instance data)
        glVertexArrayVertexBuffer(m_vao, 0, m_instanceVBO, 0, sizeof(GrassInstance));
    }

    // Upload incremental
    data.vboOffset = offset;
    if (m_instancePtr) {
        std::memcpy((char *)m_instancePtr + (size_t)offset * sizeof(GrassInstance),
                    instances.data(),
                    (size_t)instances.size() * sizeof(GrassInstance));
    }
    else {
        glNamedBufferSubData(m_instanceVBO, (GLintptr)(offset * sizeof(GrassInstance)),
                             (GLsizeiptr)(instances.size() * sizeof(GrassInstance)),
                             instances.data());
    }

    m_totalInstances += count;
}

void KX_GrassSystem::UploadStaticUniforms()
{
    // Todos os escalares de vento/LOD agora vivem no GrassLightBlock UBO.
    const float u          = InverseSmoothstep(kLodUpdateThresholdFraction);
    const float lodEndSq   = m_params.distance;
    const float lodStartSq = (m_params.lodStart >= 0.0f) ? m_params.lodStart : lodEndSq * 0.5f;

    // windLod: distância explícita do Python, ou automática (8% do range do LOD)
    float windDisableDistSq;
    if (m_params.windLod >= 0.0f) {
        const float wEnd   = m_params.windLod;
        const float wStart = wEnd * 0.5f;
        windDisableDistSq  = wStart + u * (wEnd - wStart) * 0.08f;
    } else {
        windDisableDistSq  = lodStartSq + u * (lodEndSq - lodStartSq) * 0.08f;
    }

    // lightLod: distância explícita do Python, ou automática (20% do range do LOD)
    float onlySunDistSq;
    if (m_params.lightLod >= 0.0f) {
        const float lEnd   = m_params.lightLod;
        const float lStart = lEnd * 0.5f;
        onlySunDistSq      = lStart + u * (lEnd - lStart) * 0.20f;
    } else {
        onlySunDistSq      = lodStartSq + u * (lodEndSq - lodStartSq) * 0.20f;
    }

    m_lightBlockCPU.strength        = m_params.strength;
    m_lightBlockCPU.range           = m_params.range / 100.0f;
    m_lightBlockCPU.windDisableDist = windDisableDistSq;
    m_lightBlockCPU.onlySunDist     = onlySunDistSq;
    m_paramsChanged = false;
}

void KX_GrassSystem::UpdateTiltIfNeeded()
{
    for (auto &pair : m_terrainData) {
        TerrainGrassData &td = pair.second;
        if (td.instanceCount <= 0) {
            td.tiltVersion = m_tiltVersion;
            continue;
        }
        if (td.tiltVersion == m_tiltVersion) {
            continue;
        }
#if KX_GRASS_ENABLE_COMPUTE
        if (m_computeProg && td.normalSSBO && m_instanceVBO) {
            GLint prevProgram = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
            glUseProgram(m_computeProg);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, td.normalSSBO);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_instanceVBO);
            glProgramUniform1ui(m_computeProg, m_uComputeBaseInstance, (GLuint)td.vboOffset);
            glProgramUniform1ui(m_computeProg, m_uComputeCount,        (GLuint)td.instanceCount);
            glProgramUniform1f (m_computeProg, m_uComputeFlatness,     m_params.flatness);
            const GLuint groups = (GLuint)(((uint32_t)td.instanceCount + 255u) >> 8);
            glDispatchCompute(groups, 1, 1);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT);
            glUseProgram((GLuint)prevProgram);
        }
#endif

        td.tiltVersion = m_tiltVersion;
    }
}

void KX_GrassSystem::RebuildCommandLayoutIfNeeded()
{
    if (!m_commandLayoutDirty) {
        return;
    }

    size_t totalChunks = 0;
    for (KX_GameObject *terrain : m_terrains) {
        auto it = m_terrainData.find(terrain);
        if (it == m_terrainData.end()) {
            continue;
        }
        totalChunks += it->second.chunks.size();
    }

    m_chunkDrawOrder.clear();
    m_chunkDrawOrder.reserve(totalChunks);

    for (KX_GameObject *terrain : m_terrains) {
        auto it = m_terrainData.find(terrain);
        if (it == m_terrainData.end()) {
            continue;
        }

        TerrainGrassData &td = it->second;
        td.lodSoA.Finalize((int)td.chunks.size());
        for (uint32_t chunkIndex = 0; chunkIndex < (uint32_t)td.chunks.size(); ++chunkIndex) {
            GrassChunk &chunk = td.chunks[chunkIndex];
            const uint32_t base = (uint32_t)(td.vboOffset + chunk.baseInstance);
            chunk.drawBaseInstance = base;
            chunk.drawInstanceCount = 0;
            td.lodSoA.drawBaseInstance[chunkIndex] = base;
            td.lodSoA.drawInstanceCount[chunkIndex] = 0;
            m_chunkDrawOrder.push_back({terrain, chunkIndex, base});
        }
    }

    std::sort(m_chunkDrawOrder.begin(), m_chunkDrawOrder.end(),
              [](const KX_GrassSystem::GrassChunkDrawRef &a, const KX_GrassSystem::GrassChunkDrawRef &b) {
                  return a.drawBaseInstance < b.drawBaseInstance;
              });

    if ((int)m_chunkDrawOrder.size() > m_indirectCapacity) {
        ResizeIndirectBuffer((int)m_chunkDrawOrder.size() + 512);
    }

    m_visibleDrawCount = 0;
    m_visibleIndirectIndex = 0;
    ++m_visibleCommandsVersion;

    m_forceCullingUpdate = true;
    m_visibleDrawCommandsDirty = true;
    m_commandLayoutDirty = false;
}

void KX_GrassSystem::UpdateBoundsIfNeeded()
{
    const bool needsUpdate = m_boundsDirty ||
                             m_cachedBoundsScale != m_params.scale ||
                             m_cachedBoundsLenMax != m_params.lengthMax ||
                             m_cachedBoundsHeightScale != m_params.heightScale ||
                             m_cachedBoundsStrength != m_params.strength;
    if (!needsUpdate) {
        return;
    }

    const float bladeLenMax = m_params.lengthMax * m_params.heightScale;
    const float lateralPad = m_params.scale + bladeLenMax + std::fabs(m_params.strength);

    for (auto &pair : m_terrainData) {
        TerrainGrassData &td = pair.second;
        td.lodSoA.Finalize((int)td.chunks.size());
        for (uint32_t chunkIndex = 0; chunkIndex < (uint32_t)td.chunks.size(); ++chunkIndex) {
            GrassChunk &chunk = td.chunks[chunkIndex];
            const float cellMinX = (float)chunk.ccx * CHUNK_SIZE;
            const float cellMinY = (float)chunk.ccy * CHUNK_SIZE;
            chunk.minX = cellMinX - lateralPad;
            chunk.minY = cellMinY - lateralPad;
            chunk.maxX = cellMinX + CHUNK_SIZE + lateralPad;
            chunk.maxY = cellMinY + CHUNK_SIZE + lateralPad;
            chunk.maxZ = chunk.baseMaxZ + bladeLenMax;

            td.lodSoA.minX[chunkIndex] = chunk.minX;
            td.lodSoA.minY[chunkIndex] = chunk.minY;
            td.lodSoA.minZ[chunkIndex] = chunk.minZ;
            td.lodSoA.maxX[chunkIndex] = chunk.maxX;
            td.lodSoA.maxY[chunkIndex] = chunk.maxY;
            td.lodSoA.maxZ[chunkIndex] = chunk.maxZ;
        }

        for (GrassMegaChunk &mc : td.megaChunks) {
            if (mc.chunkCount <= 0) {
                continue;
            }

            const GrassChunk *chunks = td.chunks.data() + mc.firstChunk;
            float minX = 1e9f, minY = 1e9f, minZ = 1e9f;
            float maxX = -1e9f, maxY = -1e9f, maxZ = -1e9f;
            for (int i = 0; i < mc.chunkCount; ++i) {
                const GrassChunk &c = chunks[i];
                minX = std::min(minX, c.minX);
                minY = std::min(minY, c.minY);
                minZ = std::min(minZ, c.minZ);
                maxX = std::max(maxX, c.maxX);
                maxY = std::max(maxY, c.maxY);
                maxZ = std::max(maxZ, c.maxZ);
            }
            mc.minX = minX; mc.minY = minY; mc.minZ = minZ;
            mc.maxX = maxX; mc.maxY = maxY; mc.maxZ = maxZ;
        }
    }

    m_cachedBoundsScale = m_params.scale;
    m_cachedBoundsLenMax = m_params.lengthMax;
    m_cachedBoundsHeightScale = m_params.heightScale;
    m_cachedBoundsStrength = m_params.strength;
    m_boundsDirty = false;
}

// CacheLightUniforms — removida: luzes agora vão via UBO (GrassLightBlock, binding 0)

// ============================================================================
// Init — orquestra as 3 fases
// ============================================================================

bool KX_GrassSystem::Init()
{
    if (m_terrains.empty()) {
        return false;
    }

    EnsureGPUResources();

    // Se já temos texturas (via SetTexture ou SetTextureObject), não sobrescreve
    bool textures_found = false;
    for (int i = 0; i < 4; ++i) if (m_textures[i]) textures_found = true;

    if (!textures_found) {
        if (m_textureObject) {
            SetTextureObject(m_textureObject);
            for (int i = 0; i < 4; ++i) if (m_textures[i]) textures_found = true;
        }

        if (!textures_found) {
            // Fallback para o primeiro terreno que tiver texturas
            for (KX_GameObject *terrain : m_terrains) {
                KX_BlenderMaterial *mat = terrain->GetFirstBlenderMaterial();
                if (mat) {
                    for (int i = 0; i < 4; ++i) {
                        RAS_Texture *tex = mat->GetTexture(i + 3);
                        if (tex && tex->Ok()) {
                            m_textures[i] = tex;
                            textures_found = true;
                        }
                    }
                    if (textures_found) break;
                }
            }
        }
    }

    return m_prog != nullptr;
}

// ============================================================================
// Draw — Fase 3: culling por chunk + draw calls
// ============================================================================

void KX_GrassSystem::Draw(RAS_Rasterizer *rasty)
{
    if (m_terrains.empty()) return;

    // Garante que os recursos GL existem ANTES de ProcessFinishedJobs,
    // pois AppendToGPUBuffer (chamado de lá) usa m_instanceVBO e m_freeBlocks.
    // Na primeira execução m_instanceVBO=0 causava crash no grow path.
    EnsureGPUResources();

    // Consome resultados prontos do worker antes de renderizar
    ProcessFinishedJobs();

    if (!m_initialized) {
        if (!Init()) return;
        m_initialized = true;
    }

    if (!m_prog || m_terrainData.empty()) return;

    KX_Camera *renderCam = m_scene->GetActiveCamera();
    if (!renderCam) return;

    KX_Camera *frustumCam = m_scene->GetOverrideCullingCamera();
    if (!frustumCam) {
        frustumCam = renderCam;
    }

    // Re-envia uniforms estáticos se parâmetros mudaram (campos no UBO)
    if (m_paramsChanged)
        UploadStaticUniforms();

    // Adquire (ou revalida) handles bindless para as texturas ativas.
    // Só executa quando m_texturesDirty=true — i.e., na primeira vez ou quando
    // SetTexture/SetTextureObject substituiu uma textura.
    if (m_texturesDirty)
        AcquireBindlessHandles();

    GPU_shader_bind(m_prog);

    // ── UBO: timer + camPos + luzes (1 upload por frame) ────────────────────
    const float t = KX_GetActiveEngine()->GetFrameTime() * (m_params.speed / 100.0f);
    const mt::vec3 camPos = frustumCam->NodeGetWorldPosition();

    GrassLightBlock &lb = m_lightBlockCPU;
    lb.timer    = t;
    lb.camPosX  = camPos.x;
    lb.camPosY  = camPos.y;

    int activeLayers = ~0;
    if (Scene *blenderScene = m_scene->GetBlenderScene()) {
        activeLayers = blenderScene->lay;
    }

    EXP_ListValue<KX_LightObject> *lights = m_scene->GetLightList();
    const int maxLights = 4;
    const int listCount = lights ? lights->GetCount() : 0;
    int uploadCount = 0;

    for (int i = 0; i < listCount && uploadCount < maxLights; ++i) {
        KX_LightObject  *kxL = lights->GetValue(i);
        if (!kxL) continue;
        if ((kxL->GetLayer() & activeLayers) == 0) continue;
        RAS_ILightObject *ld = kxL->GetLightData();
        if (!ld) continue;

        const mt::vec3 vpos    = kxL->NodeGetWorldPosition();
        const mt::vec3 ldir    = -kxL->NodeGetWorldOrientation().GetColumn(2);
        const mt::vec3 spotDir = ldir.Normalized();

        float e = ld->m_energy;
        mt::vec3 col = ld->m_color;
        auto it = m_lightMultipliers.find(kxL->GetName());
        if (it != m_lightMultipliers.end()) {
            e *= it->second.multiplier;
            if (it->second.color.x >= 0.0f) col = it->second.color;
        }

        const float dist = ld->m_distance;
        const int u = uploadCount;
        lb.lType    [u][0] = (float)ld->m_type;
        lb.lType    [u][1] = lb.lType[u][2] = lb.lType[u][3] = 0.0f;
        lb.lDiffuse [u][0] = col.x * e;
        lb.lDiffuse [u][1] = col.y * e;
        lb.lDiffuse [u][2] = col.z * e;
        lb.lDiffuse [u][3] = 0.0f;
        lb.lPosition[u][0] = vpos.x;
        lb.lPosition[u][1] = vpos.y;
        lb.lPosition[u][2] = vpos.z;
        lb.lPosition[u][3] = 0.0f;
        lb.lSpotDir [u][0] = spotDir.x;
        lb.lSpotDir [u][1] = spotDir.y;
        lb.lSpotDir [u][2] = spotDir.z;
        lb.lSpotDir [u][3] = 0.0f;
        lb.lParams  [u][0] = dist > 0.0f ? 1.0f / dist : 0.0f;
        lb.lParams  [u][1] = std::cos(ld->m_spotsize * 0.5f);
        lb.lParams  [u][2] = ld->m_spotblend / 5.0f;
        lb.lParams  [u][3] = dist > 0.0f ? dist * dist : 0.0f;
        ++uploadCount;
    }
    lb.lightCount = (float)uploadCount;

    glNamedBufferSubData(m_lightUBO, 0, sizeof(GrassLightBlock), &lb);

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);

    GPU_bind_vertex_array(m_vao);

    UpdateBoundsIfNeeded();
    UpdateTiltIfNeeded();
    RebuildCommandLayoutIfNeeded();

    // ============================================================================
    // 1. Culling Hierárquico com Throttling (Cache persistente)
    // ============================================================================
    const SG_Frustum &frustum = frustumCam->GetFrustum(RAS_Rasterizer::RAS_STEREO_LEFTEYE);

    uint64_t currentFrame = GetCurrentFrame();
    double currentTime = KX_GetActiveEngine()->GetClockTime();

    // Throttling: limite de 60 FPS (1/60s) e atualização a cada 2 frames
#if KX_GRASS_DISABLE_CULLING_LOD
    bool shouldUpdate = false;
#else
    bool shouldUpdate = (currentFrame - m_lastCullingFrame >= 2) && 
                        (currentTime - m_lastCullingTime >= (1.0 / 60.0));
    if (m_forceCullingUpdate) {
        shouldUpdate = true;
    }
#endif

    if (
#if KX_GRASS_DISABLE_CULLING_LOD
        false
#else
        (shouldUpdate || m_forceCullingUpdate)
#endif
    ) {
        m_lastCullingFrame = currentFrame;
        m_lastCullingTime = currentTime;
        m_forceCullingUpdate = false;
        m_visibleDrawCommandsDirty = true;

        // instanceVBOFence removido — o buffer persistente é mapeado com
        // GL_MAP_COHERENT_BIT, portanto não requer sincronização CPU→GPU manual
        // para escrita de bladeLen. Fences apenas para o ring indirect buffer.

        const float lodEndSq = m_params.distance;
        const float lodStartSq = (m_params.lodStart >= 0.0f) ? m_params.lodStart : lodEndSq * 0.5f;

        // Debug de occlusion removido

        CcdPhysicsEnvironment *occlusionEnv = nullptr;
#if KX_GRASS_ENABLE_OCCLUSION
        const int occlusionRes = m_scene->GetDbvtOcclusionRes();
        if (occlusionRes > 0) {
            occlusionEnv = dynamic_cast<CcdPhysicsEnvironment *>(m_scene->GetPhysicsEnvironment());
            if (occlusionEnv) {
                const int *viewport = KX_GetActiveEngine()->GetCanvas()->GetViewPort();
                occlusionEnv->BeginOcclusionBuffer(occlusionRes, viewport, frustum.GetMatrix());
            }
        }
#endif // KX_GRASS_ENABLE_OCCLUSION

        const SimdFrustumPlanes fp(frustum.GetPlanes());

        for (auto &pair : m_terrainData) {
            TerrainGrassData &td = pair.second;
            // Finalize() só é chamado quando o número de chunks mudou — evita
            // que o resize/padding-zero aconteça no meio do loop de culling e
            // zerife drawInstanceCount de chunks válidos (causava piscar aleatório).
            if (td.lodSoA.chunkCount != (int)td.chunks.size()) {
                td.lodSoA.Finalize((int)td.chunks.size());
            }
            for (const GrassMegaChunk &mc : td.megaChunks) {
                const mt::vec3 mc_min(mc.minX, mc.minY, mc.minZ);
                const mt::vec3 mc_max(mc.maxX, mc.maxY, mc.maxZ);

                const SG_Frustum::TestType megaResult = frustum.AabbInsideFrustumFast(mc_min, mc_max);
                GrassChunk* chunks = td.chunks.data();
                if (megaResult == SG_Frustum::OUTSIDE) {
                    ZeroDrawRangeSimd(td.lodSoA, mc.firstChunk, mc.chunkCount);
                    continue;
                }

                const bool megaInside = (megaResult == SG_Frustum::INSIDE);
                const bool megaHasDraw = LodUpdateRangeSimd(td.lodSoA,
                                                           mc.firstChunk, mc.chunkCount,
                                                           &fp, megaInside,
                                                           camPos.x, camPos.y,
                                                           lodStartSq, lodEndSq);

                // ============================================================
                // CULLING DE SUB-CHUNKS APÓS LOD DOS CHUNKS
                // ============================================================
                if (megaHasDraw) {
                    const std::array<mt::vec4, 6> &frustumPlanes = frustum.GetPlanes();
                    
                    for (int i = 0; i < mc.chunkCount; ++i) {
                        const int chunkIndex = mc.firstChunk + i;
                        const uint32_t chunkDrawCount = td.lodSoA.drawInstanceCount[(size_t)chunkIndex];
                        
                        // Se chunk passou totalmente ocluso do culling, zerar seus sub-chunks
                        if (chunkDrawCount == 0) {
                            GrassChunk &chunk = chunks[chunkIndex];
                            if (chunk.subChunkCount > 0) {
                                for (int s = 0; s < chunk.subChunkCount; ++s) {
                                    GrassSubChunk &sub = td.subChunks[chunk.firstSubChunk + s];
                                    sub.drawInstanceCount = 0;
                                }
                            }
                            continue;
                        }
                        
                        // Se chunk passou parcialmente ou totalmente visível, testar sub-chunks
                        GrassChunk &chunk = chunks[chunkIndex];
                        if (chunk.subChunkCount > 0) {
                            for (int s = 0; s < chunk.subChunkCount; ++s) {
                                GrassSubChunk &sub = td.subChunks[chunk.firstSubChunk + s];
                                
                                // Teste de frustum para sub-chunk
                                bool subVisible = megaInside || SubChunkFrustumTest(frustumPlanes, sub);
                                
                                if (subVisible) {
                                    sub.drawInstanceCount = sub.instanceCount;
                                    sub.drawBaseInstance = td.vboOffset + sub.baseInstance;
                                } else {
                                    sub.drawInstanceCount = 0;
                                }
                            }
                        }
                    }
                }

                if (megaHasDraw) {
                    for (int i = 0; i < mc.chunkCount; ++i) {
                        const int chunkIndex = mc.firstChunk + i;
                        if (td.lodSoA.drawInstanceCount[(size_t)chunkIndex] == 0) {
                            continue;
                        }
                        GrassChunk &chunk = chunks[chunkIndex];
                        if (chunk.bladeLenVersion != m_bladeLenVersion) {
                            const int globalBase = (int)chunk.drawBaseInstance;
                            GrassInstance *inst = nullptr;
                            bool needsUnmap = false;

                            if (m_instancePtr) {
                                GrassInstance *instances = (GrassInstance *)m_instancePtr;
                                inst = instances + globalBase;
                            }
                            else if (m_instanceVBO) {
                                const GLintptr byteOffset = (GLintptr)((size_t)globalBase * sizeof(GrassInstance));
                                const GLsizeiptr byteSize = (GLsizeiptr)((size_t)chunk.instanceCount * sizeof(GrassInstance));
                                const GLbitfield mapFlags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT;
                                inst = (GrassInstance *)glMapNamedBufferRange(m_instanceVBO, byteOffset, byteSize, mapFlags);
                                needsUnmap = (inst != nullptr);
                            }

                            if (inst) {
                                for (int j = 0; j < chunk.instanceCount; ++j) {
                                    inst[j].bladeLen = ComputeBladeLen(inst[j].random, m_params);
                                }
                                chunk.bladeLenVersion = m_bladeLenVersion;
                            }

                            if (needsUnmap) {
                                glUnmapNamedBuffer(m_instanceVBO);
                            }
                        }
                    }
                }

                if (occlusionEnv && megaHasDraw) {
#if KX_GRASS_ENABLE_OCCLUSION
                    const bool megaVisible = occlusionEnv->QueryOcclusionAabb(mc_min, mc_max);

                    if (!megaVisible) {
                        ZeroDrawRangeSimd(td.lodSoA, mc.firstChunk, mc.chunkCount);
                        // Zerar sub-chunks também
                        for (int i = 0; i < mc.chunkCount; ++i) {
                            const int chunkIndex = mc.firstChunk + i;
                            GrassChunk &chunk = chunks[chunkIndex];
                            if (chunk.subChunkCount > 0) {
                                for (int s = 0; s < chunk.subChunkCount; ++s) {
                                    td.subChunks[chunk.firstSubChunk + s].drawInstanceCount = 0;
                                }
                            }
                        }
                        continue;
                    }

                    for (int i = 0; i < mc.chunkCount; ++i) {
                        const int chunkIndex = mc.firstChunk + i;
                        GrassChunk &chunk = chunks[chunkIndex];
                        if (td.lodSoA.drawInstanceCount[(size_t)chunkIndex] == 0) {
                            continue;
                        }
                        
                        // Occlusion query nos sub-chunks ao invés de no chunk completo
                        if (chunk.subChunkCount > 0) {
                            for (int s = 0; s < chunk.subChunkCount; ++s) {
                                GrassSubChunk &sub = td.subChunks[chunk.firstSubChunk + s];
                                if (sub.drawInstanceCount == 0) continue;
                                
                                const mt::vec3 subMin(sub.minX, sub.minY, sub.minZ);
                                const mt::vec3 subMax(sub.maxX, sub.maxY, sub.maxZ);
                                const bool subVisible = occlusionEnv->QueryOcclusionAabb(subMin, subMax);
                                
                                if (!subVisible) {
                                    sub.drawInstanceCount = 0;
                                }
                            }
                        } else {
                            // Fallback: chunk sem sub-chunks, usar método original
                            const mt::vec3 bmin(chunk.minX, chunk.minY, chunk.minZ);
                            const mt::vec3 bmax(chunk.maxX, chunk.maxY, chunk.maxZ);
                            const bool chunkVisible = occlusionEnv->QueryOcclusionAabb(bmin, bmax);

                            if (!chunkVisible) {
                                td.lodSoA.drawInstanceCount[(size_t)chunkIndex] = 0;
                            }
                        }
                    }
#endif // KX_GRASS_ENABLE_OCCLUSION
                }
            }
        }
    }

    if (m_visibleDrawCommandsDirty) {
        if (!m_hasIndirect) {
            m_visibleDrawCount = 0;
            ++m_visibleCommandsVersion;
            m_visibleDrawCommandsDirty = false;
        }
        else {
            if ((int)m_chunkDrawOrder.size() > m_indirectCapacity) {
                ResizeIndirectBuffer((int)m_chunkDrawOrder.size() + 512);
            }

            int indirectIndex = -1;
            for (int attempt = 0; attempt < kIndirectRingSize; ++attempt) {
                const int idx = (m_indirectWriteIndex + attempt) % kIndirectRingSize;
                if (PollIndirectSlotFence(m_indirectFence[idx])) {
                    indirectIndex = idx;
                    break;
                }
            }

            if (indirectIndex < 0) {
                indirectIndex = m_indirectWriteIndex;
                WaitIndirectSlotFence(m_indirectFence[indirectIndex]);
            }

            size_t drawCount = 0;
#if KX_GRASS_PERSISTENT_INDIRECT
            DrawArraysIndirectCommand *cmds = m_indirectPtr[indirectIndex];
            if (!cmds) {
                m_visibleDrawCount = 0;
                ++m_visibleCommandsVersion;
                m_visibleDrawCommandsDirty = false;
            }
            else
#else
            m_visibleDrawCommands.clear();
            m_visibleDrawCommands.reserve(m_chunkDrawOrder.size());
#endif
            {
                for (const GrassChunkDrawRef &ref : m_chunkDrawOrder) {
                    auto itRef = m_terrainData.find(ref.terrainKey);
                    if (itRef == m_terrainData.end()) continue;
                    const TerrainGrassData &refTd = itRef->second;
#if KX_GRASS_DISABLE_CULLING_LOD
                    if (ref.chunkIndex >= (uint32_t)refTd.lodSoA.instanceCount.size()) continue;
                    const unsigned int instanceCount = refTd.lodSoA.instanceCount[ref.chunkIndex];
#else
                    if (ref.chunkIndex >= (uint32_t)refTd.lodSoA.drawInstanceCount.size()) continue;
                    const unsigned int instanceCount = refTd.lodSoA.drawInstanceCount[ref.chunkIndex];
#endif
                    if (instanceCount == 0) {
                        continue;
                    }

                    const unsigned int baseInstance = ref.drawBaseInstance;
#if KX_GRASS_DENSITY_SHADER_MERGE
                    const unsigned int kVertsPerInst = 24;
#else
                    const unsigned int kVertsPerInst = 6;
#endif

#if KX_GRASS_PERSISTENT_INDIRECT
                    if (drawCount != 0) {
                        DrawArraysIndirectCommand &prev = cmds[drawCount - 1];
                        if (prev.count == kVertsPerInst &&
                            prev.first == 0 &&
                            prev.baseInstance + prev.instanceCount == baseInstance)
                        {
                            prev.instanceCount += instanceCount;
                            continue;
                        }
                    }

                    DrawArraysIndirectCommand &cmd = cmds[drawCount++];
                    cmd.count = kVertsPerInst;
                    cmd.first = 0;
                    cmd.baseInstance = baseInstance;
                    cmd.instanceCount = instanceCount;
#else
                    if (!m_visibleDrawCommands.empty()) {
                        DrawArraysIndirectCommand &prev = m_visibleDrawCommands.back();
                        if (prev.count == kVertsPerInst &&
                            prev.first == 0 &&
                            prev.baseInstance + prev.instanceCount == baseInstance)
                        {
                            prev.instanceCount += instanceCount;
                            continue;
                        }
                    }

                    DrawArraysIndirectCommand cmd;
                    cmd.count = kVertsPerInst;
                    cmd.first = 0;
                    cmd.baseInstance = baseInstance;
                    cmd.instanceCount = instanceCount;
                    m_visibleDrawCommands.push_back(cmd);
#endif
                }

#if !KX_GRASS_PERSISTENT_INDIRECT
                drawCount = m_visibleDrawCommands.size();
                if (drawCount != 0) {
                    glNamedBufferSubData(m_indirectVBO[indirectIndex],
                                         0,
                                         (GLsizeiptr)(drawCount * sizeof(DrawArraysIndirectCommand)),
                                         m_visibleDrawCommands.data());
                }
#endif

                m_visibleDrawCount = drawCount;
                m_visibleIndirectIndex = indirectIndex;
                ++m_visibleCommandsVersion;
                m_visibleDrawCommandsDirty = false;

                m_indirectWriteIndex = (indirectIndex + 1) % kIndirectRingSize;
            }
        }
    }

    // ============================================================================
    // 2. Renderização (Indirect)
    // ============================================================================
    // GPU_shader_bind e GPU_bind_vertex_array já foram chamados antes do culling
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    
    // CORREÇÃO: Usar SetCullFace do rasterizer ao invés de chamada OpenGL direta
    // Isso mantém o estado interno sincronizado para que materiais funcionem corretamente
    if (rasty) {
        rasty->SetCullFace(false); // grama é visível dos dois lados
    } else {
        glDisable(GL_CULL_FACE); // fallback se rasterizer não disponível
    }

    if (m_visibleDrawCount != 0) {
        if (m_hasIndirect) {
            const int indirectIndex = m_visibleIndirectIndex;
            glBindBuffer(GL_DRAW_INDIRECT_BUFFER, m_indirectVBO[indirectIndex]);
#if KX_GRASS_CAMERA_RELATIVE_XY
            const mt::vec3 rc = renderCam->NodeGetWorldPosition();
            glMatrixMode(GL_MODELVIEW);
            glPushMatrix();
            glTranslatef(rc.x, rc.y, 0.0f);
#endif
            glMultiDrawArraysIndirect(GL_TRIANGLES, 0, (GLsizei)m_visibleDrawCount, 0);
#if KX_GRASS_CAMERA_RELATIVE_XY
            glPopMatrix();
#endif

            DeleteIndirectSlotFence(m_indirectFence[indirectIndex]);
            m_indirectFence[indirectIndex] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        }
    }

    GPU_unbind_vertex_array();
    glDepthMask(GL_TRUE);

    GPU_shader_unbind();
}

#endif // WITH_PYTHON

