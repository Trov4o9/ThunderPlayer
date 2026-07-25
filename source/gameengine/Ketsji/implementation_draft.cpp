#include "KX_GameObject.h"
#include "KX_Mesh.h"
#include "KX_Scene.h"
#include "BLI_noise.h"
#include <vector>
#include <map>
#include <string>
#include <cmath>
#include <algorithm>

// Auxiliares
static float smoothstep(float edge0, float edge1, float x) {
    x = std::max(0.0f, std::min(1.0f, (x - edge0) / (edge1 - edge0)));
    return x * x * (3 - 2 * x);
}

struct BiomeData {
    int best_idx;
    float max_blend_grainy;
    std::vector<std::pair<int, float>> all_biomes_smooth;
};

// Implementação de get_biome_data em C++
static BiomeData get_biome_data(const mt::vec3 &pos, PyObject *active_biomes) {
    BiomeData data;
    data.best_idx = -1;
    data.max_blend_grainy = 0.0f;
    
    float transition_start_factor = 0.7f;
    Py_ssize_t num_biomes = PyList_Size(active_biomes);

    for (Py_ssize_t i = 0; i < num_biomes; ++i) {
        PyObject *biome = PyList_GetItem(active_biomes, i);
        
        // Extrair center (Vector), radius (float), noise_scale (float), noise_offset (Vector), irregularity_strength (float)
        PyObject *pyCenter = PyDict_GetItemString(biome, "center");
        float radius = PyFloat_AsDouble(PyDict_GetItemString(biome, "radius"));
        float noise_scale = PyFloat_AsDouble(PyDict_GetItemString(biome, "noise_scale"));
        PyObject *pyNoiseOffset = PyDict_GetItemString(biome, "noise_offset");
        float irregularity_strength = PyFloat_AsDouble(PyDict_GetItemString(biome, "irregularity_strength"));

        // Converter PyObjects para mt::vec3 (Assumindo que são mathutils.Vector ou listas)
        mt::vec3 center(0, 0, 0);
        if (pyCenter) {
            PyArg_ParseTuple(PyObject_GetAttrString(pyCenter, "XYZ"), "fff", &center.x, &center.y, &center.z);
        }
        
        mt::vec3 diff = mt::vec3(pos.x, pos.y, 0.0f) - mt::vec3(center.x, center.y, 0.0f);
        float dist_sq = diff.length_squared();
        float max_dist = radius * 1.5f;

        if (dist_sq > max_dist * max_dist) continue;

        float dist_base = std::sqrt(dist_sq);
        
        mt::vec3 noise_pos = mt::vec3(pos.x, pos.y, 0.0f) * noise_scale;
        if (pyNoiseOffset) {
            mt::vec3 n_off(0, 0, 0);
            PyArg_ParseTuple(PyObject_GetAttrString(pyNoiseOffset, "XYZ"), "fff", &n_off.x, &n_off.y, &n_off.z);
            noise_pos += n_off;
        }

        float irregular = BLI_gNoise(1.0f, noise_pos.x, noise_pos.y, noise_pos.z, 0, 0) * radius * irregularity_strength;
        float dist_grainy = dist_base + irregular;

        if (dist_grainy <= radius) {
            float bg = 1.0f;
            if (dist_grainy >= radius * transition_start_factor) {
                bg = 1.0f - smoothstep(radius * transition_start_factor, radius, dist_grainy);
            }
            if (bg > data.max_blend_grainy) {
                data.max_blend_grainy = bg;
                data.best_idx = (int)i;
            }
        }

        float smooth_irregular = BLI_gNoise(0.5f, noise_pos.x, noise_pos.y, noise_pos.z, 0, 0) * radius * (irregularity_strength * 0.15f);
        float dist_smooth = dist_base + smooth_irregular;

        if (dist_smooth <= radius) {
            float bs = 1.0f;
            if (dist_smooth >= radius * transition_start_factor) {
                bs = 1.0f - smoothstep(radius * transition_start_factor, radius, dist_smooth);
            }
            if (bs > 0) {
                data.all_biomes_smooth.push_back({(int)i, bs});
            }
        }
    }
    return data;
}

PyObject *KX_GameObject::PyApplyRecipe(PyObject *args) {
    PyObject *pyTerrains, *pyRecipe;
    if (!PyArg_ParseTuple(args, "OO", &pyTerrains, &pyRecipe)) {
        return nullptr;
    }

    // 1. Extract Recipe Data
    int seed = PyLong_AsLong(PyDict_GetItemString(pyRecipe, "seed"));
    float tree_chance = PyFloat_AsDouble(PyDict_GetItemString(pyRecipe, "tree_chance"));
    PyObject *pyOffset = PyDict_GetItemString(pyRecipe, "offset");
    mt::vec3 offset(0, 0, 0);
    if (pyOffset) {
        PyArg_ParseTuple(PyObject_GetAttrString(pyOffset, "XYZ"), "fff", &offset.x, &offset.y, &offset.z);
    }
    PyObject *active_biomes = PyDict_GetItemString(pyRecipe, "ACTIVE_BIOMES");

    // 2. Vertex Collection
    struct VertexInfo {
        KX_GameObject *obj;
        KX_Mesh *mesh;
        int index;
    };
    std::vector<VertexInfo> combined_vertices;
    Py_ssize_t num_terrains = PyList_Size(pyTerrains);
    for (Py_ssize_t i = 0; i < num_terrains; ++i) {
        PyObject *pyObj = PyList_GetItem(pyTerrains, i);
        KX_GameObject *gameobj = static_cast<KX_GameObject *>(EXP_PROXY_REF(pyObj));
        if (gameobj && !gameobj->GetMeshList().empty()) {
            KX_Mesh *mesh = gameobj->GetMeshList()[0];
            int num_v = mesh->GetNumVertices(); // Simplified, should check all slots/arrays
            for (int idx = 0; idx < num_v; ++idx) {
                combined_vertices.push_back({gameobj, mesh, idx});
            }
        }
    }

    // 3. Biome Grid Generation (Simplified for now, using 64x64 if needed or calculating on the fly)
    // In Python, it's used for interpolation. Here we might calculate per vertex or pre-calculate.

    // 4. Main Loop
    for (auto &vinfo : combined_vertices) {
        RAS_Mesh::Vertex *vertex = vinfo.mesh->GetVertex(0, vinfo.index);
        mt::vec3 local_pos = vertex->getXYZ();
        mt::vec3 world_pos = vinfo.obj->NodeGetWorldPosition() + (vinfo.obj->NodeGetWorldOrientation() * local_pos);

        mt::vec3 pos_noise = world_pos * 0.002f + offset;

        // Base Height
        float base_height = mg_HeteroTerrain(pos_noise.x, pos_noise.y, pos_noise.z, 0.5f, 1.5f, 2.0f, 1.8f, 0) * 8.0f;
        float final_height = base_height;

        // Biome blending
        BiomeData bdata = get_biome_data(world_pos, active_biomes);
        PyObject *best_biome = (bdata.best_idx != -1) ? PyList_GetItem(active_biomes, bdata.best_idx) : nullptr;

        if (!bdata.all_biomes_smooth.empty()) {
            float target_height_sum = 0.0f;
            float total_applied_weight = 0.0f;

            for (auto &pair : bdata.all_biomes_smooth) {
                int b_idx = pair.first;
                float b_blend = pair.second;
                PyObject *biome = PyList_GetItem(active_biomes, b_idx);
                PyObject *pyHetero = PyDict_GetItemString(biome, "hetero");

                if (pyHetero) {
                    float scale = PyFloat_AsDouble(PyDict_GetItemString(pyHetero, "scale"));
                    float H = PyFloat_AsDouble(PyDict_GetItemString(pyHetero, "H"));
                    float lacunarity = PyFloat_AsDouble(PyDict_GetItemString(pyHetero, "lacunarity"));
                    int octaves = PyLong_AsLong(PyDict_GetItemString(pyHetero, "octaves"));
                    float amplitude = PyFloat_AsDouble(PyDict_GetItemString(pyHetero, "amplitude"));
                    float mix_factor = PyFloat_AsDouble(PyDict_GetItemString(pyHetero, "mix_factor"));

                    mt::vec3 b_pos_noise = pos_noise * scale;
                    float b_h = mg_HeteroTerrain(b_pos_noise.x, b_pos_noise.y, b_pos_noise.z, H, lacunarity, (float)octaves, 1.8f, 0) * amplitude;
                    
                    float app_weight = b_blend * mix_factor;
                    target_height_sum += b_h * app_weight;
                    total_applied_weight += app_weight;
                }
            }

            if (total_applied_weight > 0) {
                float avg_biome_height = target_height_sum / total_applied_weight;
                float final_blend = std::min(1.0f, total_applied_weight);
                final_height = final_height * (1.0f - final_blend) + avg_biome_height * final_blend;
            }
        }

        vertex->setXYZ(mt::vec3(local_pos.x, local_pos.y, final_height));
        
        // 5. Color Assignment
        mt::vec4 color(0.0f, 0.9f, 0.5f, 1.0f); // Default plains
        if (best_biome) {
            PyObject *pyName = PyDict_GetItemString(best_biome, "nome");
            const char *name = _PyUnicode_AsString(pyName);
            if (strcmp(name, "Floresta") == 0) color = mt::vec4(0.0f, 0.6f, 0.0f, 1.0f);
            else if (strcmp(name, "Planicie") == 0) color = mt::vec4(0.0f, 0.9f, 0.5f, 1.0f);
            else if (strcmp(name, "Savanna") == 0) color = mt::vec4(0.0f, 0.8f, 0.4f, 1.0f);
            else if (strcmp(name, "mountain") == 0) color = mt::vec4(0.0f, 0.5f, 0.5f, 1.0f);
            else if (strcmp(name, "Neve") == 0) color = mt::vec4(1.0f, 0.9f, 1.0f, 1.0f);
            else if (strcmp(name, "Tundra") == 0) color = mt::vec4(1.0f, 0.8f, 0.9f, 1.0f);
        } else {
            if (final_height < 120.0f) color = mt::vec4(0.0f, 0.9f, 0.5f, 1.0f);
            else if (final_height < 160.0f) color = mt::vec4(0.0f, 0.5f, 0.5f, 1.0f);
            else color = mt::vec4(1.0f, 0.9f, 1.0f, 1.0f);
        }
        vertex->setRGBA(color);

        // 6. Spawning Logic (Simplified)
        if (best_biome && tree_chance > 0) {
            // Noise-based spawning check
            float spawn_noise = BLI_gNoise(1.0f, world_pos.x * 3.0f, world_pos.y * 3.0f, (float)seed * 0.001f, 0, 0);
            float rand_main = (spawn_noise + 1.0f) * 0.5f;

            PyObject *obj_configs = PyDict_GetItemString(best_biome, "objects");
            if (obj_configs && PyDict_Check(obj_configs)) {
                PyObject *key, *value;
                Py_ssize_t pos = 0;
                while (PyDict_Next(obj_configs, &pos, &key, &value)) {
                    const char *obj_type = _PyUnicode_AsString(key);
                    float base_chance = (float)PyFloat_AsDouble(value);
                    
                    if (strcmp(obj_type, "tree") == 0) {
                        float final_prob = tree_chance * 185.0f * base_chance;
                        if (rand_main < final_prob && final_height <= 45.0f) {
                            KX_Scene *scene = vinfo.obj->GetScene();
                            SCA_LogicManager *logic = scene->GetLogicManager();
                            
                            KX_GameObject *tree003 = scene->AddReplicaObject(logic->GetGameObjectByName("tree.003"), vinfo.obj);
                            if (tree003) {
                                tree003->NodeSetLocalPosition(world_pos + mt::vec3(0, 0, 1.0f));
                                // Sub-parts
                                if (BLI_gNoise(1.0f, world_pos.x * 0.021f, world_pos.y * 0.019f, (float)seed * 0.001f, 0, 0) > -0.8f) { // Noise is -1 to 1
                                    KX_GameObject *tree002 = scene->AddReplicaObject(logic->GetGameObjectByName("tree.002"), tree003);
                                    if (tree002) {
                                        tree002->SetParent(tree003, false, false);
                                        if (BLI_gNoise(1.0f, world_pos.x * 0.031f, world_pos.y * 0.027f, (float)seed * 0.001f, 0, 0) < 0.6f) {
                                            KX_GameObject *tree001 = scene->AddReplicaObject(logic->GetGameObjectByName("tree.001"), tree002);
                                            if (tree001) tree001->SetParent(tree002, false, false);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // 5. Update Normals
    PyUpdateTerrainNormals();

    Py_RETURN_NONE;
}

// Python Binding Boilerplate
PyObject *KX_GameObject::sPyApplyRecipe(PyObject *self, PyObject *args) {
    KX_GameObject *gameobj = static_cast<KX_GameObject *>(EXP_PROXY_REF(self));
    if (!gameobj) {
        PyErr_SetString(PyExc_RuntimeError, "Invalid game object");
        return nullptr;
    }
    return gameobj->PyApplyRecipe(args);
}

const char *KX_GameObject::ApplyRecipe_doc = 
"apply_recipe(terrains, recipe)\n"
"Applies a terrain recipe to a list of terrain objects.\n"
"terrains: list of KX_GameObject\n"
"recipe: dict containing seed, offset, ACTIVE_BIOMES, etc.";
