/*
 * MDEI_MeshBuilder.cpp — Extract geometry directly from Mesh* into MDEI_Mesh.
 *
 * Supports up to MDEI_MAX_UV UV layers.  Each layer is stored interleaved in
 * the VBO: [pos(3f) | normal(3f) | uv0(2f) | uv1(2f) | ...].
 *
 * Vertex de-duplication key covers pos + normal + ALL used UV slots so that
 * vertices that differ only in UV are correctly split.
 */

#include "MDEI_MeshBuilder.h"
#include "MDEI_Mesh.h"

#include "DNA_object_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_customdata_types.h"   /* CD_MLOOPUV, CD_NORMAL */

extern "C" {
#  include "BKE_mesh.h"
#  include "BKE_customdata.h"   /* CustomData_number_of_layers, CustomData_get_layer_n */
}

#include "MEM_guardedalloc.h"
#include "BLI_math.h"

#include <vector>
#include <string>
#include <unordered_map>
#include <cstdlib>
#include <cstring>

/* ── Vertex de-duplication key ───────────────────────────────────────────────
 * We cover pos + normal + all UV layers (up to MDEI_MAX_UV).
 * The uv[][] array is always MDEI_MAX_UV wide; unused slots remain zero.
 * ---------------------------------------------------------------------------- */
struct VKey {
    int  px, py, pz;
    int  nx, ny, nz;
    int  uv[MDEI_MAX_UV][2];   /* quantised per-layer UVs */

    bool operator==(const VKey &o) const {
        if (px != o.px || py != o.py || pz != o.pz) return false;
        if (nx != o.nx || ny != o.ny || nz != o.nz) return false;
        for (int i = 0; i < MDEI_MAX_UV; i++) {
            if (uv[i][0] != o.uv[i][0] || uv[i][1] != o.uv[i][1]) return false;
        }
        return true;
    }
};

struct VKeyHash {
    size_t operator()(const VKey &k) const {
        size_t h = 0;
        auto mix = [&](int val) {
            h ^= std::hash<int>()(val) + 0x9e3779b9 + (h << 6) + (h >> 2);
        };
        mix(k.px); mix(k.py); mix(k.pz);
        mix(k.nx); mix(k.ny); mix(k.nz);
        for (int i = 0; i < MDEI_MAX_UV; i++) {
            mix(k.uv[i][0]);
            mix(k.uv[i][1]);
        }
        return h;
    }
};

static inline int quantise(float f) { return (int)(f * 16384.0f); }

/* ── Build ───────────────────────────────────────────────────────────────── */

MDEI_Mesh *MDEI_MeshBuilder::Build(Object *ob, Scene * /*blenderScene*/)
{
    if (!ob || ob->type != OB_MESH || !ob->data)
        return nullptr;

    Mesh *me = static_cast<Mesh *>(ob->data);

    const int totvert = me->totvert;
    const int totloop = me->totloop;
    const int totpoly = me->totpoly;

    if (totvert == 0 || totloop == 0 || totpoly == 0)
        return nullptr;

    MVert  *mverts = me->mvert;
    MLoop  *mloops = me->mloop;
    MPoly  *mpolys = me->mpoly;

    /* ── 1. Compute MLoopTri array ───────────────────────────────────── */
    const int ntris = poly_to_tri_count(totpoly, totloop);
    MLoopTri *tris = (MLoopTri *)MEM_malloc_arrayN(
        (size_t)ntris, sizeof(MLoopTri), "mdei_looptri");
    BKE_mesh_recalc_looptri(mloops, mpolys, mverts, totloop, totpoly, tris);

    /* ── 2. Per-loop split normals ───────────────────────────────────── */
    BKE_mesh_calc_normals_split(me);
    const float (*loopNors)[3] = (const float (*)[3])CustomData_get_layer(
        &me->ldata, CD_NORMAL);

    /* ── 3. Collect ALL MLoopUV layers (up to MDEI_MAX_UV) ──────────── */
    const int totalUVLayers = CustomData_number_of_layers(&me->ldata, CD_MLOOPUV);
    const int uvCount       = (totalUVLayers < MDEI_MAX_UV)
                              ? totalUVLayers
                              : MDEI_MAX_UV;

    /* Pointers to each UV layer's data (nullptr if not present). */
    const MLoopUV *uvLayers[MDEI_MAX_UV];
    std::string    uvNames [MDEI_MAX_UV];
    for (int i = 0; i < MDEI_MAX_UV; i++) {
        uvLayers[i] = nullptr;
        uvNames [i] = "";
    }
    for (int i = 0; i < uvCount; i++) {
        uvLayers[i] = (const MLoopUV *)CustomData_get_layer_n(
            &me->ldata, CD_MLOOPUV, i);
        /* Layer name: stored in the CustomData.layers[n].name field.
         * CustomData_get_layer_name() returns the name for layer index n. */
        const char *name = CustomData_get_layer_name(&me->ldata, CD_MLOOPUV, i);
        if (name) uvNames[i] = name;
    }

    /* Active UV layer index (used as fallback when name == ""). */
    const int activeUv = CustomData_get_active_layer(&me->ldata, CD_MLOOPUV);

    /* ── 4. Iterate triangles — build de-duplicated vertex list ──────── */
    std::vector<MDEI_Vertex>  verts;
    std::vector<unsigned int> indices;
    std::unordered_map<VKey, unsigned int, VKeyHash> cache;
    verts.reserve((size_t)(ntris * 3));
    indices.reserve((size_t)(ntris * 3));

    for (int i = 0; i < ntris; i++) {
        const MLoopTri &lt = tris[i];
        for (int k = 0; k < 3; k++) {
            const unsigned int li = lt.tri[k];
            const unsigned int vi = mloops[li].v;
            const MVert       &mv = mverts[vi];

            /* Normal: prefer split normals; fall back to vertex normals. */
            float nx, ny, nz;
            if (loopNors) {
                nx = loopNors[li][0];
                ny = loopNors[li][1];
                nz = loopNors[li][2];
            }
            else {
                nx = mv.no[0] / 32767.0f;
                ny = mv.no[1] / 32767.0f;
                nz = mv.no[2] / 32767.0f;
            }

            /* Build de-duplication key (covers all UV layers). */
            VKey key;
            memset(&key, 0, sizeof(key));
            key.px = quantise(mv.co[0]);
            key.py = quantise(mv.co[1]);
            key.pz = quantise(mv.co[2]);
            key.nx = quantise(nx);
            key.ny = quantise(ny);
            key.nz = quantise(nz);
            for (int j = 0; j < uvCount; j++) {
                float uf = uvLayers[j] ? uvLayers[j][li].uv[0] : 0.0f;
                float vf = uvLayers[j] ? uvLayers[j][li].uv[1] : 0.0f;
                key.uv[j][0] = quantise(uf);
                key.uv[j][1] = quantise(vf);
            }

            auto it = cache.find(key);
            if (it != cache.end()) {
                indices.push_back(it->second);
            }
            else {
                MDEI_Vertex vert;
                memset(&vert, 0, sizeof(vert));
                vert.px = mv.co[0]; vert.py = mv.co[1]; vert.pz = mv.co[2];
                vert.nx = nx;       vert.ny = ny;       vert.nz = nz;
                for (int j = 0; j < uvCount; j++) {
                    vert.uv[j][0] = uvLayers[j] ? uvLayers[j][li].uv[0] : 0.0f;
                    vert.uv[j][1] = uvLayers[j] ? uvLayers[j][li].uv[1] : 0.0f;
                }
                const unsigned int idx = (unsigned int)verts.size();
                verts.push_back(vert);
                cache[key] = idx;
                indices.push_back(idx);
            }
        }
    }

    MEM_freeN(tris);

    if (verts.empty() || indices.empty())
        return nullptr;

    MDEI_Mesh *mesh = new MDEI_Mesh();
    mesh->Upload(verts, indices, uvCount, uvNames, activeUv >= 0 ? activeUv : 0);
    return mesh;
}
