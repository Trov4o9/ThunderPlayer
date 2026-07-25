/**
 * KX_GrassInstancer.cpp — standalone GPU-instanced grass pass.
 *
 * Otimizações aplicadas:
 *   1. Sem glGet* por frame — estado GL assumido, sem save/restore
 *   2. Uniforms de luz cacheados — sem snprintf/GPU_shader_get_uniform por frame
 *   3. normalize() → inversesqrt() no fragment (instrução nativa)
 *   4. hashf() sem sin() — hash inteiro bit-mixing (muito mais rápido no loading)
 *   5. Sem blend GL — alpha-test via discard no threshold 0.5
 *   6. sqrt() em vez de pow(x, 0.5)
 *   7. lp.z em vez de dot(gl_NormalMatrix*norm, lp)
 */

#ifdef WITH_PYTHON

#include "KX_GrassInstancer.h"
#include "KX_GameObject.h"
#include "KX_Scene.h"
#include "KX_LightObject.h"
#include "KX_Globals.h"
#include "KX_Camera.h"
#include "KX_BlenderMaterial.h"
#include "KX_Mesh.h"
#include "RAS_Mesh.h"
#include "RAS_DisplayArray.h"
#include "RAS_ILightObject.h"
#include "RAS_Texture.h"
#include "EXP_PyObjectPlus.h"

extern "C" {
#  include "GPU_glew.h"
#  include "GPU_shader.h"
#  include "GPU_vertex_array.h"
}

#include <cmath>
#include <cstdio>
#include <algorithm>

// ============================================================================
// Vertex shader
// ============================================================================
static const char *s_vert =
"in vec2 bladeVert;\n"
"in vec3 worldXYZ;\n"
"in vec4 instData;\n"
"in vec2 instSinCos;\n"
"in vec2 instNorm;\n"          // nx, ny compactados (s8norm → [-1,1]); nz reconstruído
"\n"
"uniform sampler2D texNoise;\n"
"uniform float timer, scale, lengthMax, lengthMin, heightScale, strength, range, dist, flatness;\n"
"\n"
"struct Light {\n"
"    int   type;\n"
"    float dist;\n"
"    float invDist;\n"
"    float spotCutoff;\n"
"    float spotExponent;\n"
"    vec3  diffuse;\n"
"    vec3  position;\n"
"    vec3  spotDirection;\n"
"};\n"
"uniform Light lights[4];\n"
"uniform int   lightCount;\n"
"\n"
"out vec2  v_UV;\n"
"out vec2  v_Coord;\n"
"out float v_Layer;\n"
"out float v_Random;\n"
"out float v_Alpha;\n"
"out vec3  v_Light;\n"
"\n"
"vec3 calcLight(vec3 pos, vec3 norm) {\n"
"    vec3 d = vec3(0.0);\n"
"    for (int l = 0; l < 4; l++) {\n"
"        if (l >= lightCount) break;\n"
"        vec3 lp;\n"
"        float att;\n"
"        if (lights[l].type == 0) {\n"
"            vec3 sp = lights[l].position - pos;\n"
"            float d2 = dot(sp, sp);\n"
"            float inv = inversesqrt(max(d2, 0.0001));\n"
"            float sd = d2 * inv;\n"
"            lp = sp * inv;\n"
"            float x = sd * lights[l].invDist + 1.0;\n"
"            float x2 = x * x;\n"
"            att = 1.0 / (x2 * x2 * x);\n"
"            float sv = max(0.0, dot(-lp, lights[l].spotDirection));\n"
"            float cut = lights[l].spotCutoff / 114.6;\n"
"            float ex = lights[l].spotExponent / 5.0;\n"
"            att *= mix(0.0, 1.0, smoothstep(0.0, 1.0, (sv - cos(cut)) / max(ex, 0.0001)));\n"
"        } else if (lights[l].type == 1) {\n"
"            lp = normalize(-lights[l].spotDirection);\n"
"            att = 1.0;\n"
"        } else {\n"
"            vec3 delta = lights[l].position - pos;\n"
"            float d2 = dot(delta, delta);\n"
"            float inv = inversesqrt(max(d2, 0.0001));\n"
"            float sd = d2 * inv;\n"
"            lp = delta * inv;\n"
"            float x = sd * lights[l].invDist + 1.0;\n"
"            float x2 = x * x;\n"
"            att = 1.0 / (x2 * x2 * x);\n"
"        }\n"
"        d += lights[l].diffuse * max(0.0, dot(norm, lp)) * att;\n"
"    }\n"
"    return d;\n"
"}\n"
"\n"
"void main() {\n"
"    float rnd   = instData.r;\n"
"    float paint = instData.g;\n"
"    float sinA  = instSinCos.x;\n"
"    float cosA  = instSinCos.y;\n"
"\n"
"    // Reconstrói normal do terreno a partir dos 2 bytes compactados\n"
"    // instNorm já vem normalizado [-1,1] via GL_BYTE + GL_TRUE\n"
"    float tnx = instNorm.x;\n"
"    float tny = instNorm.y;\n"
"    float tnz = sqrt(max(0.0, 1.0 - tnx*tnx - tny*tny));\n"
"    vec3 terrNorm = vec3(tnx, tny, tnz);\n"
"    // Suavização não-linear: em terreno plano (tnz≈1) quase não inclina;\n"
"    // em morros (tnz baixo) suaviza mais, evitando que a grama cole demais\n"
"    vec3 up = vec3(0.0, 0.0, 1.0);\n"
"    float t = smoothstep(0.0, 1.0, tnz);\n"
"    vec3 terrNormSmooth = normalize(mix(terrNorm, up, flatness * (1.0 - t)));\n"
"\n"
"    v_UV     = worldXYZ.xy * 0.1;\n"
"    v_Random = rnd;\n"
"    float camDist = -(gl_ModelViewMatrix * vec4(worldXYZ, 1.0)).z;\n"
"    if (paint > 0.25 && camDist < dist) {\n"
"        float wind = texture2D(texNoise, worldXYZ.xy * range + timer).r;\n"
"        wind = (wind * 2.0 - 1.0) * paint * strength;\n"
"        float blen = clamp(paint * scale * 2.0 * lengthMax * rnd, lengthMin, lengthMax)\n"
"                     * heightScale;\n"
"        if (bladeVert.y < 0.5) {\n"
"            vec2 local = vec2(cosA * bladeVert.x, sinA * bladeVert.x) + worldXYZ.xy;\n"
"            v_Coord = (bladeVert.x < 0.0) ? vec2(0.0, -0.5) : vec2(1.0, -0.5);\n"
"            gl_Position = gl_ModelViewProjectionMatrix * vec4(local, worldXYZ.z, 1.0);\n"
"        } else {\n"
"            v_Coord = vec2(0.5, 0.0);\n"
"            // Inclina o topo da grama seguindo a normal suavizada do terreno\n"
"            vec3 tip = worldXYZ + vec3(\n"
"                terrNormSmooth.x * blen - wind,\n"
"                terrNormSmooth.y * blen - wind,\n"
"                terrNormSmooth.z * blen\n"
"            );\n"
"            gl_Position = gl_ModelViewProjectionMatrix * vec4(tip, 1.0);\n"
"        }\n"
"        v_Layer = 0.0;\n"
"        v_Alpha = 1.0;\n"
"        vec3 posVS  = vec3(gl_ModelViewMatrix * vec4(worldXYZ, 1.0));\n"
"        // Normal para iluminação: usa a normal suavizada\n"
"        vec3 normVS = mat3(gl_ModelViewMatrix) * terrNormSmooth;\n"
"        v_Light = calcLight(posVS, normVS);\n"
"    } else {\n"
"        gl_Position = vec4(0.0, 0.0, -2.0, 1.0);\n"
"        v_Coord = vec2(0.0);\n"
"        v_Layer = 1.0;\n"
"        v_Alpha = 0.0;\n"
"        v_Light = vec3(0.0);\n"
"    }\n"
"}\n";

// ============================================================================
// Fragment shader
// Otimizações:
//   - inversesqrt(dot) em vez de normalize() — instrução nativa
//   - lp.z em vez de dot(gl_NormalMatrix*norm, lp) — sem matrix multiply
//   - sqrt() em vez de pow(x, 0.5)
//   - alpha-test via discard no threshold 0.5 (sem blend)
//   - textura selecionada por id inteiro — 1 sample, sem branching triplo
// ============================================================================
static const char *s_frag =
"in vec2  v_UV;\n"
"in vec2  v_Coord;\n"
"in float v_Layer;\n"
"in float v_Random;\n"
"in float v_Alpha;\n"
"in vec3  v_Light;\n"
"\n"
"uniform sampler2D texGrass0;\n"
"uniform sampler2D texGrass1;\n"
"uniform sampler2D texGrass2;\n"
"\n"
"void main() {\n"
"    float id = floor(v_Random * 3.0);\n"
"    vec4 g0 = (id < 0.5) ? texture2D(texGrass0, v_Coord) :\n"
"              (id < 1.5) ? texture2D(texGrass1, v_Coord) :\n"
"                           texture2D(texGrass2, v_Coord);\n"
"    vec3 lit = v_Light * (2.5 / (1.0 + v_Light));\n"
"    float alpha = step(0.1, g0.a) * v_Alpha;\n"
"    if (alpha < 0.5) discard;\n"
"    gl_FragColor = vec4(g0.rgb * lit, 1.0);\n"
"}\n";

// ============================================================================
// Helpers
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

// Normalized integer attribute — uint8 or int8 → [0,1] or [-1,1] in shader
static inline void attribNorm(GLint loc, GLsizei stride, intptr_t offset,
                               GLint size, GLenum type, GLuint divisor)
{
    if (loc < 0) return;
    glEnableVertexAttribArray((GLuint)loc);
    glVertexAttribPointer((GLuint)loc, size, type, GL_TRUE,
                          stride, reinterpret_cast<const void *>(offset));
    glVertexAttribDivisor((GLuint)loc, divisor);
}

// Hash inteiro bit-mixing — sem sin(), muito mais rápido que a versão anterior
// Retorna [0, 1) de forma determinística
static inline float hashf(float x, float y, int seed)
{
    // Converte para inteiros e aplica bit-mixing (estilo MurmurHash)
    unsigned int ix = (unsigned int)(x * 1000.0f + 0.5f) ^ 0x9e3779b9u;
    unsigned int iy = (unsigned int)(y * 1000.0f + 0.5f) ^ 0x6c62272eu;
    unsigned int is = (unsigned int)seed * 0x85ebca6bu;
    unsigned int h  = ix ^ iy ^ is;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return (float)(h & 0xFFFFFFu) / (float)0x1000000u;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

KX_GrassInstancer::KX_GrassInstancer(KX_GameObject *terrain, const Params &p)
    : m_terrain(terrain), m_params(p)
{}

KX_GrassInstancer::~KX_GrassInstancer()
{
    // Registro na cena removido — sistema migrado para KX_GrassSystem
    if (m_prog)        { GPU_shader_free(m_prog);                m_prog = nullptr; }
    if (m_vao)         { GPU_delete_vertex_arrays(1, &m_vao);    m_vao = 0; }
    if (m_geomVBO)     { glDeleteBuffers(1, &m_geomVBO);         m_geomVBO = 0; }
    if (m_instanceVBO) { glDeleteBuffers(1, &m_instanceVBO);     m_instanceVBO = 0; }
}

// ============================================================================
// BuildInstances — N blades por triângulo com jitter determinístico
// ============================================================================

void KX_GrassInstancer::BuildInstances()
{
    m_instances.clear();

    const std::vector<KX_Mesh *> &meshes = m_terrain->GetMeshList();
    if (meshes.empty()) return;

    RAS_Mesh *mesh = static_cast<RAS_Mesh *>(meshes[0]);
    const unsigned int polyCount = mesh->GetNumPolygons();
    const int density = std::max(1, m_params.density);
    m_instances.reserve(polyCount * density);

    for (unsigned int p = 0; p < polyCount; ++p) {
        RAS_Mesh::PolygonInfo poly = mesh->GetPolygon(p);
        if (!poly.array) continue;

        float px[3], py[3], pz[3], paint[3] = {1.f, 1.f, 1.f};
        for (int v = 0; v < 3; ++v) {
            const mt::vec3_packed &pos = poly.array->GetPosition(poly.indices[v]);
            px[v] = pos.x;
            py[v] = pos.y;
            pz[v] = pos.z;
            if (poly.array->GetFormat().colorSize > 0) {
                const unsigned char (&c)[4] = poly.array->GetColor(poly.indices[v], 0);
                paint[v] = c[0] / 255.0f;
            }
        }

        const float cx  = (px[0] + px[1] + px[2]) / 3.0f;
        const float cy  = (py[0] + py[1] + py[2]) / 3.0f;
        const float cz  = (pz[0] + pz[1] + pz[2]) / 3.0f;
        const float avg = (paint[0] + paint[1] + paint[2]) / 3.0f;

        // Normal do triângulo via produto vetorial
        const float ax = px[1]-px[0], ay = py[1]-py[0], az = pz[1]-pz[0];
        const float bx = px[2]-px[0], by = py[2]-py[0], bz = pz[2]-pz[0];
        float nnx = ay*bz - az*by;
        float nny = az*bx - ax*bz;
        float nnz = ax*by - ay*bx;
        // Garante que Z aponta para cima (normal do terreno)
        if (nnz < 0.0f) { nnx = -nnx; nny = -nny; nnz = -nnz; }
        const float invLen = 1.0f / std::sqrt(nnx*nnx + nny*nny + nnz*nnz + 1e-8f);
        nnx *= invLen;  nny *= invLen;

        const float dx = px[0] - cx, dy = py[0] - cy;
        const float triRadius = std::sqrt(dx*dx + dy*dy) * 0.7f;

        for (int d = 0; d < density; ++d) {
            const float jx = (hashf(cx, cy, d * 2)     * 2.0f - 1.0f) * triRadius;
            const float jy = (hashf(cy, cx, d * 2 + 1) * 2.0f - 1.0f) * triRadius;
            const float wx  = cx + jx;
            const float wy  = cy + jy;
            const float rnd = hashf(wx, wy, d + 7);

            GrassInstance inst;
            inst.worldX = wx;
            inst.worldY = wy;
            inst.worldZ = cz;
            inst.random = (uint8_t)(rnd * 255.0f + 0.5f);
            inst.paint  = (uint8_t)(avg * 255.0f + 0.5f);
            const float ang = hashf(wx, wy, d + 13);
            inst.sinA   = (int8_t)(std::sin(ang * 6.28318530f) * 127.0f);
            inst.cosA   = (int8_t)(std::cos(ang * 6.28318530f) * 127.0f);
            // Normal do terreno compactada — nz reconstruído no shader
            inst.nx     = (int8_t)(nnx * 127.0f);
            inst.ny     = (int8_t)(nny * 127.0f);
            inst._pad[0] = 0;
            inst._pad[1] = 0;
            m_instances.push_back(inst);
        }
    }

    m_instanceCount = static_cast<int>(m_instances.size());
}

// ============================================================================
// UploadGPUBuffers
// ============================================================================

void KX_GrassInstancer::UploadGPUBuffers()
{
    m_prog = GPU_shader_create(s_vert, s_frag, nullptr, nullptr, nullptr, 0, 0, 0);
    if (!m_prog) {
        fprintf(stderr, "[KX_GrassInstancer] GPU_shader_create failed\n");
        return;
    }

    // Cache todos os uniforms estáticos — zero lookup por frame
    m_uTimer      = GPU_shader_get_uniform(m_prog, "timer");
    m_uScale      = GPU_shader_get_uniform(m_prog, "scale");
    m_uLenMax     = GPU_shader_get_uniform(m_prog, "lengthMax");
    m_uLenMin     = GPU_shader_get_uniform(m_prog, "lengthMin");
    m_uHeightScale= GPU_shader_get_uniform(m_prog, "heightScale");
    m_uDist       = GPU_shader_get_uniform(m_prog, "dist");
    m_uStrength   = GPU_shader_get_uniform(m_prog, "strength");
    m_uRange      = GPU_shader_get_uniform(m_prog, "range");
    m_uLightCount = GPU_shader_get_uniform(m_prog, "lightCount");
    m_uGrassNorm  = -1;  // removido — normal agora vem por instância
    m_uFlatness   = GPU_shader_get_uniform(m_prog, "flatness");
    m_uTexNoise   = GPU_shader_get_uniform(m_prog, "texNoise");
    m_uTexGrass0  = GPU_shader_get_uniform(m_prog, "texGrass0");
    m_uTexGrass1  = GPU_shader_get_uniform(m_prog, "texGrass1");
    m_uTexGrass2  = GPU_shader_get_uniform(m_prog, "texGrass2");

    GPU_shader_bind(m_prog);
    GPU_shader_uniform_float(m_prog, m_uScale,       m_params.scale);
    GPU_shader_uniform_float(m_prog, m_uLenMax,      m_params.lengthMax);
    GPU_shader_uniform_float(m_prog, m_uLenMin,      m_params.lengthMin);
    GPU_shader_uniform_float(m_prog, m_uHeightScale, m_params.heightScale);
    GPU_shader_uniform_float(m_prog, m_uDist,        m_params.distance);
    GPU_shader_uniform_float(m_prog, m_uStrength,    m_params.strength / 2.0f);
    GPU_shader_uniform_float(m_prog, m_uRange,       m_params.range);
    GPU_shader_uniform_float(m_prog, m_uFlatness,    m_params.flatness);
    GPU_shader_uniform_int(m_prog, m_uTexNoise,  0);
    GPU_shader_uniform_int(m_prog, m_uTexGrass0, 1);
    GPU_shader_uniform_int(m_prog, m_uTexGrass1, 2);
    GPU_shader_uniform_int(m_prog, m_uTexGrass2, 3);
    GPU_shader_unbind();
    const float s = m_params.scale;
    const float geom[6] = { -s, 0.0f,   s, 0.0f,   0.0f, 1.0f };

    glGenBuffers(1, &m_geomVBO);
    glBindBuffer(GL_ARRAY_BUFFER, m_geomVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(geom), geom, GL_STATIC_DRAW);

    glGenBuffers(1, &m_instanceVBO);
    glBindBuffer(GL_ARRAY_BUFFER, m_instanceVBO);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(m_instanceCount * (int)sizeof(GrassInstance)),
                 m_instances.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    const GLuint prog = (GLuint)GPU_shader_program(m_prog);
    const GLint locBlade = glGetAttribLocation(prog, "bladeVert");
    const GLint locXY    = glGetAttribLocation(prog, "worldXYZ");
    const GLint locInst  = glGetAttribLocation(prog, "instData");
    const GLint locSC    = glGetAttribLocation(prog, "instSinCos");
    const GLint locNorm  = glGetAttribLocation(prog, "instNorm");

    GPU_create_vertex_arrays(1, &m_vao);
    GPU_bind_vertex_array(m_vao);

    glBindBuffer(GL_ARRAY_BUFFER, m_geomVBO);
    attribVec(locBlade, sizeof(float) * 2, 0, 2, 0);

    glBindBuffer(GL_ARRAY_BUFFER, m_instanceVBO);
    attribVec(locXY,   sizeof(GrassInstance), offsetof(GrassInstance, worldX), 3, 1);
    // instData: random(u8norm), paint(u8norm), sinA(s8norm), cosA(s8norm) — 4 bytes packed
    attribNorm(locInst, sizeof(GrassInstance), offsetof(GrassInstance, random), 4, GL_UNSIGNED_BYTE, 1);
    // instSinCos: sinA(s8norm), cosA(s8norm) — 2 bytes
    attribNorm(locSC,   sizeof(GrassInstance), offsetof(GrassInstance, sinA),   2, GL_BYTE, 1);
    // instNorm: nx, ny (s8norm → [-1,1]); nz reconstruído no shader
    attribNorm(locNorm, sizeof(GrassInstance), offsetof(GrassInstance, nx),     2, GL_BYTE, 1);

    GPU_unbind_vertex_array();
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// ============================================================================
// CacheLightUniforms — query feita UMA VEZ no Init()
// Elimina snprintf() + GPU_shader_get_uniform() por frame por luz
// ============================================================================

void KX_GrassInstancer::CacheLightUniforms()
{
    char buf[64];
    for (int i = 0; i < 4; ++i) {
        snprintf(buf, sizeof(buf), "lights[%d].type", i);
        m_lightUniforms[i].type = GPU_shader_get_uniform(m_prog, buf);
        snprintf(buf, sizeof(buf), "lights[%d].dist", i);
        m_lightUniforms[i].dist = GPU_shader_get_uniform(m_prog, buf);
        snprintf(buf, sizeof(buf), "lights[%d].invDist", i);
        m_lightUniforms[i].invDist = GPU_shader_get_uniform(m_prog, buf);
        snprintf(buf, sizeof(buf), "lights[%d].spotCutoff", i);
        m_lightUniforms[i].spotCutoff = GPU_shader_get_uniform(m_prog, buf);
        snprintf(buf, sizeof(buf), "lights[%d].spotExponent", i);
        m_lightUniforms[i].spotExponent = GPU_shader_get_uniform(m_prog, buf);
        snprintf(buf, sizeof(buf), "lights[%d].diffuse", i);
        m_lightUniforms[i].diffuse = GPU_shader_get_uniform(m_prog, buf);
        snprintf(buf, sizeof(buf), "lights[%d].position", i);
        m_lightUniforms[i].position = GPU_shader_get_uniform(m_prog, buf);
        snprintf(buf, sizeof(buf), "lights[%d].spotDirection", i);
        m_lightUniforms[i].spotDir = GPU_shader_get_uniform(m_prog, buf);
    }
}

void KX_GrassInstancer::RegisterSceneCallback()
{
    // Registro na cena removido — sistema migrado para KX_GrassSystem
    (void)m_terrain;
}

// ============================================================================
// Init
// ============================================================================

bool KX_GrassInstancer::Init()
{
    if (m_terrain->GetMeshList().empty()) {
        fprintf(stderr, "[KX_GrassInstancer] terrain '%s' has no mesh\n",
                m_terrain->GetName().c_str());
        return false;
    }

    BuildInstances();
    if (m_instanceCount == 0) {
        fprintf(stderr, "[KX_GrassInstancer] terrain '%s' has no polygons\n",
                m_terrain->GetName().c_str());
        return false;
    }

    UploadGPUBuffers();
    if (!m_prog) return false;

    // Cache uniforms de luz — feito uma vez aqui, zero custo por frame
    CacheLightUniforms();

    {
        KX_BlenderMaterial *mat = m_terrain->GetFirstBlenderMaterial();
        if (mat) {
            for (int i = 0; i < 5; ++i) {
                RAS_Texture *tex = mat->GetTexture(i);
                m_textures[i] = (tex && tex->Ok()) ? tex : nullptr;
            }
        }
    }

    RegisterSceneCallback();
    return true;
}

// ============================================================================
// Draw — chamado todo frame via KX_Scene::DrawGrassInstancers()
//
// Otimizações de CPU:
//   - Sem glGet* — estado GL assumido (sem sync CPU↔GPU)
//   - Uniforms de luz via m_lightUniforms[] cacheados — sem snprintf por frame
//   - Sem save/restore de blend — alpha-test via discard, sem blend necessário
// ============================================================================

void KX_GrassInstancer::Draw()
{
    if (!m_prog || m_instanceCount == 0) return;

    KX_Scene *scene = m_terrain->GetScene();
    if (!scene) return;

    KX_Camera *cam = scene->GetActiveCamera();
    if (!cam) return;

    GPU_shader_bind(m_prog);

    // Timer
    const float t = KX_GetActiveEngine()->GetFrameTime() * m_params.speed;
    GPU_shader_uniform_float(m_prog, m_uTimer, t);

    // Luzes — acesso direto aos campos C++ de RAS_ILightObject
    EXP_ListValue<KX_LightObject> *lights = scene->GetLightList();
    const int lcount = lights ? std::min(lights->GetCount(), 4) : 0;
    GPU_shader_uniform_int(m_prog, m_uLightCount, lcount);

    const mt::mat3x4 viewMat   = cam->NodeGetWorldTransform().Inverse();
    const mt::mat3   camOriInv = cam->NodeGetWorldOrientation().Inverse();

    for (int i = 0; i < lcount; ++i) {
        KX_LightObject  *kxL = lights->GetValue(i);
        RAS_ILightObject *ld = kxL->GetLightData();
        if (!ld) continue;

        const mt::vec3 vpos    = viewMat * kxL->NodeGetWorldPosition();
        const mt::vec3 ldir    = -kxL->NodeGetWorldOrientation().GetColumn(2);
        const mt::vec3 spotDir = camOriInv * ldir.Normalized();
        const mt::vec3 &col    = ld->m_color;
        const float e          = ld->m_energy;

        // Uniforms via cache — sem snprintf, sem hash lookup, sem driver stall
        const GrassLightUniforms &lu = m_lightUniforms[i];
        GPU_shader_uniform_int  (m_prog, lu.type,         (int)ld->m_type);
        GPU_shader_uniform_float(m_prog, lu.dist,         ld->m_distance);
        GPU_shader_uniform_float(m_prog, lu.invDist,      ld->m_distance > 0.0f ? 1.0f / ld->m_distance : 0.0f);
        GPU_shader_uniform_float(m_prog, lu.spotCutoff,   RAD2DEG(ld->m_spotsize));
        GPU_shader_uniform_float(m_prog, lu.spotExponent, ld->m_spotblend);
        glUniform3f(lu.diffuse,  col.x*e, col.y*e, col.z*e);
        glUniform3f(lu.position, vpos.x, vpos.y, vpos.z);
        glUniform3f(lu.spotDir,  spotDir.x, spotDir.y, spotDir.z);
    }

    // Ativa texturas: slots 0-3 (texNoise, grass0, grass1, grass2)
    // texGround removido — grama é objeto separado, não precisa blend com terreno
    for (int i = 0; i < 4; ++i) {
        if (m_textures[i])
            m_textures[i]->ActivateTexture(i);
    }

    // Estado GL assumido após RenderBuckets (sem glGet*):
    // depth test = ON, depthFunc = LEQUAL, blend = OFF (último material pode ter deixado ON)
    // Só desativamos depth write para grama não sobrescrever depth dos objetos
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);  // garante sem blend — usamos discard

    GPU_bind_vertex_array(m_vao);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 3, m_instanceCount);
    GPU_unbind_vertex_array();

    // Restaura apenas o que mudamos
    glDepthMask(GL_TRUE);

    for (int i = 0; i < 4; ++i) {
        if (m_textures[i])
            m_textures[i]->DisableTexture();
    }
    for (int i = 3; i >= 0; --i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glActiveTexture(GL_TEXTURE0);

    GPU_shader_unbind();
}

// ============================================================================
// Trampoline Python (não usado — mantido por compatibilidade)
// ============================================================================

PyObject *KX_GrassInstancer::PyDrawTrampoline(PyObject * /*self*/, PyObject * /*null*/)
{
    Py_RETURN_NONE;
}

#endif // WITH_PYTHON
