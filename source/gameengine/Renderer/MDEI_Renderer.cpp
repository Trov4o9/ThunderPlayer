/*
 * MDEI_Renderer.cpp
 *
 * Draw path (new — single draw call per shader):
 *   For each unique shader that has visible instances:
 *     1. Bind shader once
 *     2. Bind the shader's shared GeometryPool VAO once
 *     3. ONE glMultiDrawElementsIndirect call covering ALL commands for that
 *        shader — each command points to a different firstIndex/baseVertex
 *        slice inside the shared VBO+EBO.
 *
 * Non-pooled (skinned / EnsurePrivate) groups still behave as before:
 *   one draw call per (mesh, shader) pair with all instances batched.
 *
 * Lifecycle
 * ---------
 * addObject  → RegisterObject  → allocate pool slot; inc ref-count if mesh already pooled
 * endObject  → UnregisterObject → dec ref-count; free pool slot when ref==0
 * replaceMesh→ ResetMesh        → free old pool slot; allocate new slot
 */

#include "MDEI_Renderer.h"
#include "MDEI_Mesh.h"
#include "MDEI_MeshBuilder.h"
#include "MDEI_Shader.h"
#include "MDEI_DrawGroup.h"
#include "MDEI_GeometryPool.h"
#include "MDEI_ObjectProxy.h"
#include "MDEI_SkinDeformer.h"

#include "KX_GameObject.h"
#include "KX_Scene.h"
#include "KX_LightObject.h"

#include "RAS_Rasterizer.h"
#include "RAS_ILightObject.h"

#include "GPU_material.h"
#include "GPU_shader.h"

#include "DNA_object_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"

#include "BKE_material.h"

#include "GPU_glew.h"

#include <cstring>
#include <cstdio>
#include <algorithm>


#ifndef MDEI_DEBUG_LEVEL
#  define MDEI_DEBUG_LEVEL 0
#endif

#define MDEI_MAX_DEBUG_FRAMES 8

#ifndef MDEI_MAX_INSTANCES
#  define MDEI_MAX_INSTANCES 65536
#endif

/* ── GL error helper ───────────────────────────────────────────────────── */
static void mdei_gl_check(const char *label)
{
#if MDEI_DEBUG_LEVEL >= 1
	GLenum e;
	while ((e = glGetError()) != GL_NO_ERROR)
		fprintf(stderr, "[MDEI][GL] 0x%04X after %s\n", e, label);
#else
	(void)label;
#endif
}

/* ── Pretty-print a single DrawElementsIndirectCommand ─────────────────── */
#if MDEI_DEBUG_LEVEL >= 2
static void mdei_print_cmd(int idx, const DrawElementsIndirectCommand &c)
{
	fprintf(stderr,
	        "[MDEI]   cmd[%d]: indices=%u instCnt=%u firstIdx=%u baseVtx=%d baseInst=%u\n",
	        idx, c.count, c.instanceCount, c.firstIndex, c.baseVertex, c.baseInstance);
}
#endif

/* ══════════════════════════════════════════════════════════════════════════ */

MDEI_Renderer::MDEI_Renderer(KX_Scene *scene)
	: m_scene(scene)
	, m_solidFrame(0)
	, m_shadowFrame(MDEI_RING_SEGMENTS / 2)
	, m_debugCallCount(0)
	, m_ensurePrivateCount(0)
{
	m_buffer.Init();
}

MDEI_Renderer::~MDEI_Renderer()
{
	for (auto &p : m_skinDeformers)    delete p.second;
	for (MDEI_DrawGroup *g : m_groups) delete g;
	for (auto &p : m_meshCache)        delete p.second;
	for (auto &p : m_skinnedMeshCache) delete p.second;
	for (auto &p : m_shaderCache)      delete p.second.shader;
	m_buffer.Shutdown();
}

/* ─── Registration ──────────────────────────────────────────────────────── */

void MDEI_Renderer::RegisterObject(KX_GameObject *gameobj,
                                   Object        *blenderObj,
                                   Material      *blenderMat,
                                   Scene         *blenderScene)
{
	if (!gameobj || !blenderObj) return;

	/* ── 1. Mesh ─────────────────────────────────────────────────────── */
	MDEI_Mesh *mesh = nullptr;
	{
		void *key = blenderObj->data;
		auto it   = m_meshCache.find(key);
		if (it != m_meshCache.end()) {
			mesh = it->second;
#if MDEI_DEBUG_LEVEL >= 1
			fprintf(stderr, "[MDEI] Register '%s': reusing cached mesh VAO=%u (%d idx)\n",
			        gameobj->GetName().c_str(),
			        mesh->GetVAO(), (int)mesh->GetIndexCount());
#endif
		}
		else {
			mesh = MDEI_MeshBuilder::Build(blenderObj, blenderScene);
			if (!mesh) {
				fprintf(stderr, "[MDEI] Register '%s': FAILED — mesh build returned null\n",
				        gameobj->GetName().c_str());
				return;
			}
#if MDEI_DEBUG_LEVEL >= 1
			fprintf(stderr, "[MDEI] Register '%s': new mesh  VAO=%u VBO=%u EBO=%u "
			        "indices=%d  AABB(%.2f %.2f %.2f)-(%.2f %.2f %.2f)\n",
			        gameobj->GetName().c_str(),
			        mesh->GetVAO(), mesh->GetVBO(), mesh->GetEBO(),
			        (int)mesh->GetIndexCount(),
			        mesh->m_aabbMin[0], mesh->m_aabbMin[1], mesh->m_aabbMin[2],
			        mesh->m_aabbMax[0], mesh->m_aabbMax[1], mesh->m_aabbMax[2]);
#endif
			m_meshCache[key] = mesh;
		}
	}

	/* ── 2. Shader ───────────────────────────────────────────────────── */
	MDEI_Shader *shader = nullptr;
	GPUMaterial *gpuMat = nullptr;
	{
		void *key = blenderMat;
		auto it   = m_shaderCache.find(key);
		if (it != m_shaderCache.end()) {
			shader = it->second.shader;
			gpuMat = it->second.gpuMat;
#if MDEI_DEBUG_LEVEL >= 1
			fprintf(stderr, "[MDEI] Register '%s': reusing cached shader\n",
			        gameobj->GetName().c_str());
#endif
		}
		else {
			if (!blenderMat) {
				fprintf(stderr, "[MDEI] Register '%s': FAILED — no material\n",
				        gameobj->GetName().c_str());
				return;
			}

			gpuMat = GPU_material_from_blender(blenderScene, blenderMat, false, false);
			if (!gpuMat) {
				fprintf(stderr, "[MDEI] Register '%s': FAILED — GPU_material_from_blender\n",
				        gameobj->GetName().c_str());
				return;
			}

			GPUPass *pass = GPU_material_get_pass(gpuMat);
			if (!pass) {
				fprintf(stderr, "[MDEI] Register '%s': FAILED — GPUPass is null "
				        "(material not compiled yet?)\n",
				        gameobj->GetName().c_str());
				return;
			}

			shader = new MDEI_Shader();
			if (!shader->CompileFromPass(pass, gpuMat, blenderScene, /*variance=*/false)) {
				delete shader;
				fprintf(stderr, "[MDEI] Register '%s': FAILED — shader compile\n",
				        gameobj->GetName().c_str());
				return;
			}

			const bool cullFace = (blenderMat->game.flag & 16) != 0;
			shader->SetCullFace(cullFace);

#if MDEI_DEBUG_LEVEL >= 1
			fprintf(stderr, "[MDEI] Register '%s': compiled shader — "
			        "unfviewmat=%d unfinvviewmat=%d unftime=%d  cullFace=%d\n",
			        gameobj->GetName().c_str(),
			        shader->m_locViewMat, shader->m_locInvViewMat, shader->m_locTime,
			        (int)cullFace);
#endif
			m_shaderCache[key] = { shader, gpuMat, cullFace };
		}
	}

	/* ── 3. DrawGroup (por shader) ───────────────────────────────────── */
	/* A partir daqui grupos são únicos por shader, não por (shader, mesh).
	 * Objetos com shaders iguais mas malhas diferentes compartilham o mesmo
	 * grupo e o GeometryPool interno garante um único draw call. */
	MDEI_DrawGroup *group = GetOrCreatePooledGroup(shader);

	/* ── 4. Aloca slot no GeometryPool ───────────────────────────────── */
	/* Se a mesh já estava no pool (outro objeto já a registrou), apenas
	 * incrementa o ref-count.  Caso contrário, copia a geometria. */
	MDEI_MeshSlot slot = MDEI_SLOT_INVALID;
	{
		/* Verifica se esse MDEI_Mesh já tem um slot no pool deste grupo */
		auto sit = m_meshToSlot.find({ group, mesh });
		if (sit != m_meshToSlot.end()) {
			slot = sit->second;
			group->GetPool()->AddRef(slot);
#if MDEI_DEBUG_LEVEL >= 1
			fprintf(stderr, "[MDEI] Register '%s': mesh already in pool, slot=%d\n",
			        gameobj->GetName().c_str(), slot);
#endif
		}
		else {
			slot = group->GetPool()->Allocate(mesh);
			if (slot == MDEI_SLOT_INVALID) {
				/* Stride incompatível ou packedVerts vazio — usa grupo privado. */
				group = GetOrCreatePrivateGroup(mesh, shader);
			}
			else {
				m_meshToSlot[{ group, mesh }] = slot;
#if MDEI_DEBUG_LEVEL >= 1
				fprintf(stderr, "[MDEI] Register '%s': mesh allocated in pool, slot=%d\n",
				        gameobj->GetName().c_str(), slot);
#endif
			}
		}
	}

	/* ── 5. Proxy ────────────────────────────────────────────────────── */
	MDEI_ObjectProxy *proxy = (slot != MDEI_SLOT_INVALID)
	    ? new MDEI_ObjectProxy(mesh, group, slot)
	    : new MDEI_ObjectProxy(mesh, group);

	gameobj->SetMdeiProxy(proxy);
	gameobj->SetFastRenderFlag(true);

	gameobj->GetCullingNode().GetAabb().Set(mesh->m_aabbMin, mesh->m_aabbMax);

	m_registeredObjects.push_back(gameobj);

#if MDEI_DEBUG_LEVEL >= 1
	fprintf(stderr, "[MDEI] Register '%s': OK — group=%p  slot=%d  total_registered=%d\n",
	        gameobj->GetName().c_str(), (void *)group, slot,
	        (int)m_registeredObjects.size());
#endif
}

void MDEI_Renderer::RegisterReplica(KX_GameObject *replica,
	                                   KX_GameObject *original)
{
	if (!original || !original->HasFastRenderFlag()) return;

	MDEI_ObjectProxy *src = original->GetMdeiProxy();
	if (!src) return;

	/* Increment ref-count for the pool slot we are sharing */
	if (src->meshSlot != MDEI_SLOT_INVALID && src->group && src->group->GetPool())
		src->group->GetPool()->AddRef(src->meshSlot);

	MDEI_ObjectProxy *proxy = new MDEI_ObjectProxy(src->mesh, src->group, src->meshSlot);
	replica->SetMdeiProxy(proxy);
	replica->SetFastRenderFlag(true);

	if (src->mesh)
		replica->GetCullingNode().GetAabb().Set(src->mesh->m_aabbMin, src->mesh->m_aabbMax);

	m_registeredObjects.push_back(replica);

#if MDEI_DEBUG_LEVEL >= 1
	fprintf(stderr,
	        "[MDEI] RegisterReplica '%s' <- original='%s'"
	        "  mesh=%p  group=%p  slot=%d  shader=%p  total_registered=%d\n",
	        replica->GetName().c_str(),
	        original->GetName().c_str(),
	        (void *)src->mesh,
	        (void *)src->group,
	        src->meshSlot,
	        (void *)(src->group ? src->group->GetShader() : nullptr),
	        (int)m_registeredObjects.size());
#endif
}

void MDEI_Renderer::RegisterArmature(KX_GameObject     *gameobj,
                                     Object            *blenderObjNew,
                                     BL_ArmatureObject *arma)
{
	if (!gameobj || !arma) return;

	/* The mesh for this object must already be in m_meshCache. */
	MDEI_Mesh *mesh = nullptr;
	{
		void *staticKey = blenderObjNew->data;
		auto  it        = m_meshCache.find(staticKey);
		if (it == m_meshCache.end()) {
			fprintf(stderr, "[MDEI] RegisterArmature '%s': no base mesh in cache\n",
			        gameobj->GetName().c_str());
			return;
		}

		void *skinnedKey = (void *)blenderObjNew;
		auto  sit        = m_skinnedMeshCache.find(skinnedKey);
		if (sit != m_skinnedMeshCache.end()) {
			mesh = sit->second;
		}
		else {
			mesh = MDEI_MeshBuilder::Build(blenderObjNew,
			                               m_scene->GetBlenderScene(),
			                               /*isSkinned=*/true);
			if (!mesh) {
				fprintf(stderr, "[MDEI] RegisterArmature '%s': mesh rebuild failed\n",
				        gameobj->GetName().c_str());
				return;
			}
			m_skinnedMeshCache[skinnedKey] = mesh;

			/* Swap the proxy's mesh pointer to the skinned mesh.
			 * Skinned objects always use a private (non-pooled) group. */
			MDEI_ObjectProxy *proxy = gameobj->GetMdeiProxy();
			if (proxy) {
				MDEI_Shader *sh = proxy->group ? proxy->group->GetShader() : nullptr;

				/* Release the pool slot the object had (from RegisterObject) */
				if (proxy->meshSlot != MDEI_SLOT_INVALID && proxy->group && proxy->group->GetPool()) {
					proxy->group->GetPool()->Free(proxy->meshSlot);
					/* Remove from meshToSlot map if we were the last user */
					auto mkey = std::make_pair(proxy->group, proxy->mesh);
					auto mit  = m_meshToSlot.find(mkey);
					if (mit != m_meshToSlot.end()) {
						const MDEI_PoolMeshInfo &info = proxy->group->GetPool()->GetInfo(proxy->meshSlot);
						if (info.refCount == 0)
							m_meshToSlot.erase(mit);
					}
				}

				/* Create a private group for this skinned object */
				MDEI_DrawGroup *privateGroup = GetOrCreatePrivateGroup(mesh, sh);
				proxy->mesh     = mesh;
				proxy->group    = privateGroup;
				proxy->meshSlot = MDEI_SLOT_INVALID;
			}
		}
	}

	MDEI_SkinDeformer *deformer = new MDEI_SkinDeformer(
	       gameobj, blenderObjNew, blenderObjNew, mesh, arma);

	m_skinDeformers[gameobj] = deformer;

#if MDEI_DEBUG_LEVEL >= 1
	fprintf(stderr, "[MDEI] RegisterArmature '%s': skinned mesh VAO=%u  deformer=%p  "
	        "total_deformers=%d\n",
	        gameobj->GetName().c_str(), mesh->GetVAO(), (void *)deformer,
	        (int)m_skinDeformers.size());
#endif
}

void MDEI_Renderer::UnregisterObject(KX_GameObject *gameobj)
{
	if (!gameobj || !gameobj->HasFastRenderFlag()) return;

	/* Remove skin deformer if present */
	auto dit = m_skinDeformers.find(gameobj);
	if (dit != m_skinDeformers.end()) {
		delete dit->second;
		m_skinDeformers.erase(dit);
	}

	/* Remove from our own list */
	auto it = std::find(m_registeredObjects.begin(), m_registeredObjects.end(), gameobj);
	if (it != m_registeredObjects.end())
		m_registeredObjects.erase(it);

	MDEI_ObjectProxy *proxy = gameobj->GetMdeiProxy();
	if (proxy) {
		/* Release the pool slot — if ref drops to 0 the geometry is freed */
		if (proxy->meshSlot != MDEI_SLOT_INVALID && proxy->group) {
			MDEI_GeometryPool *pool = proxy->group->GetPool();
			if (pool) {
				pool->Free(proxy->meshSlot);
				/* If nobody else uses this (mesh, group) pair, remove the map entry */
				const auto mkey = std::make_pair(proxy->group, proxy->mesh);
				auto mit = m_meshToSlot.find(mkey);
				if (mit != m_meshToSlot.end()) {
					const MDEI_PoolMeshInfo &info = pool->GetInfo(proxy->meshSlot);
					if (info.refCount == 0)
						m_meshToSlot.erase(mit);
				}
			}
		}

		delete proxy;
		gameobj->SetMdeiProxy(nullptr);
	}
	gameobj->SetFastRenderFlag(false);
}

void MDEI_Renderer::ResetMesh(KX_GameObject *gameobj)
{
	if (!gameobj || !gameobj->HasFastRenderFlag()) return;

	MDEI_ObjectProxy *proxy = gameobj->GetMdeiProxy();
	if (!proxy || !proxy->mesh) return;

	MDEI_Mesh *meshBefore  = proxy->mesh;
	GLuint     vaoBefore   = meshBefore->GetVAO();

	/* ── 1. Garanta mesh e DrawGroup exclusivos ──────────────────────── */
	EnsurePrivateMesh(gameobj);

	/* Após EnsurePrivateMesh proxy->group é privado */
	MDEI_Mesh *privateMesh = proxy->mesh;
	GLuint     vaoPrivate  = privateMesh->GetVAO();

	/* ── 2. Libera geometria GPU e cria mesh vazio ───────────────────── */
	privateMesh->Release();
	delete privateMesh;
	MDEI_Mesh *fresh = new MDEI_Mesh();
	proxy->mesh = fresh;
	if (proxy->group) proxy->group->SetMesh(fresh);
	/* Pool slot já foi desligado por EnsurePrivateMesh */
	proxy->meshSlot = MDEI_SLOT_INVALID;

#if MDEI_DEBUG_LEVEL >= 1
	fprintf(stderr,
	        "[MDEI] ResetMesh '%s':"
	        "  mesh_before=%p (VAO=%u)"
	        "  private_mesh=%p (VAO=%u)"
	        "  fresh_mesh=%p"
	        "  group=%p  shader=%p\n",
	        gameobj->GetName().c_str(),
	        (void *)meshBefore,   vaoBefore,
	        (void *)privateMesh,  vaoPrivate,
	        (void *)fresh,
	        (void *)proxy->group,
	        (void *)(proxy->group ? proxy->group->GetShader() : nullptr));
#endif

	/* ── 3. Remove skin deformer ─────────────────────────────────────── */
	auto dit = m_skinDeformers.find(gameobj);
	if (dit != m_skinDeformers.end()) {
		delete dit->second;
		m_skinDeformers.erase(dit);
	}
}

void MDEI_Renderer::EnsurePrivateMesh(KX_GameObject *gameobj)
{
	if (!gameobj || !gameobj->HasFastRenderFlag()) return;

	MDEI_ObjectProxy *proxy = gameobj->GetMdeiProxy();
	if (!proxy || !proxy->mesh) return;

	/* Se o proxy já usa um grupo privado (não-pooled), nada a fazer */
	if (!proxy->group || !proxy->group->IsPooled()) return;

	MDEI_Mesh   *oldMesh   = proxy->mesh;
	MDEI_Shader *oldShader = proxy->group->GetShader();
	MDEI_DrawGroup *oldGroup = proxy->group;

	/* Libera o slot do pool */
	if (proxy->meshSlot != MDEI_SLOT_INVALID) {
		MDEI_GeometryPool *pool = oldGroup->GetPool();
		if (pool) {
			pool->Free(proxy->meshSlot);
			const auto mkey = std::make_pair(oldGroup, oldMesh);
			auto mit = m_meshToSlot.find(mkey);
			if (mit != m_meshToSlot.end()) {
				const MDEI_PoolMeshInfo &info = pool->GetInfo(proxy->meshSlot);
				if (info.refCount == 0)
					m_meshToSlot.erase(mit);
			}
		}
	}

	/* Cria mesh privado + DrawGroup dedicado */
	MDEI_Mesh *fresh = new MDEI_Mesh();
	proxy->mesh     = fresh;
	proxy->meshSlot = MDEI_SLOT_INVALID;

	MDEI_DrawGroup *dedicated = GetOrCreatePrivateGroup(fresh, oldShader);
	proxy->group = dedicated;

	m_ensurePrivateCount++;

#if MDEI_DEBUG_LEVEL >= 1
	fprintf(stderr,
	        "[MDEI] EnsurePrivateMesh '%s': PRIVATIZED #%d"
	        "  old_mesh=%p (VAO=%u)  old_group=%p"
	        "  new_mesh=%p  new_group=%p  shader=%p"
	        "  total_groups_now=%d\n",
	        gameobj->GetName().c_str(),
	        m_ensurePrivateCount,
	        (void *)oldMesh,
	        oldMesh->GetVAO(),
	        (void *)oldGroup,
	        (void *)fresh,
	        (void *)dedicated,
	        (void *)oldShader,
	        (int)m_groups.size());
#endif
}

void MDEI_Renderer::UpdateDeformerForObject(KX_GameObject *gameobj)
{
	auto it = m_skinDeformers.find(gameobj);
	if (it != m_skinDeformers.end()) {
#if MDEI_DEBUG_LEVEL >= 1
		fprintf(stderr, "[MDEI] UpdateDeformerForObject: '%s'\n",
		        gameobj->GetName().c_str());
#endif
		it->second->Update();
	}
}

void MDEI_Renderer::UpdateDeformers()
{
	for (auto &kv : m_skinDeformers) {
#if MDEI_DEBUG_LEVEL >= 1
		fprintf(stderr, "[MDEI] UpdateDeformers: '%s'\n",
		        kv.first->GetName().c_str());
#endif
		kv.second->Update();
	}
}

/* ─── DrawGroup lookup ──────────────────────────────────────────────────── */

/** Retorna (ou cria) o grupo pooled para um dado shader.
 *  Existe exatamente UM grupo pooled por shader — todos os objetos com esse
 *  shader compartilham o mesmo MDEI_GeometryPool + VAO. */
MDEI_DrawGroup *MDEI_Renderer::GetOrCreatePooledGroup(MDEI_Shader *shader)
{
	for (MDEI_DrawGroup *g : m_groups)
		if (g->IsPooled() && g->GetShader() == shader)
			return g;

	MDEI_DrawGroup *g = new MDEI_DrawGroup(shader);
	m_groups.push_back(g);
	return g;
}

/** Retorna (ou cria) um grupo privado para o par (mesh, shader).
 *  Usado para objetos com armature e para EnsurePrivateMesh. */
MDEI_DrawGroup *MDEI_Renderer::GetOrCreatePrivateGroup(MDEI_Mesh *mesh, MDEI_Shader *shader)
{
	for (MDEI_DrawGroup *g : m_groups)
		if (!g->IsPooled() && g->GetMesh() == mesh && g->GetShader() == shader)
			return g;

	MDEI_DrawGroup *g = new MDEI_DrawGroup(mesh, shader);
	m_groups.push_back(g);
	return g;
}

/* Compatibilidade: usado por código legado que ainda chama GetOrCreateGroup */
MDEI_DrawGroup *MDEI_Renderer::GetOrCreateGroup(MDEI_Mesh *mesh, MDEI_Shader *shader)
{
	return GetOrCreatePrivateGroup(mesh, shader);
}

/* ─── Render ────────────────────────────────────────────────────────────── */

void MDEI_Renderer::ExecuteDraw(bool shadowPass, RAS_Rasterizer *rasty)
{
	if (m_groups.empty() || m_registeredObjects.empty()) return;

	/* ── 0. Restore GL_MODELVIEW to view-only matrix ─────────────────── */
	{
		const mt::mat4& vm = rasty->GetViewMatrix();
		rasty->SetMatrixMode(RAS_Rasterizer::RAS_MODELVIEW);
		rasty->LoadMatrix(&vm.Data()[0][0]);
	}

	int &frameIndex = shadowPass ? m_shadowFrame : m_solidFrame;

	const int  callId   = m_debugCallCount++;
	(void)callId;

#if MDEI_DEBUG_LEVEL >= 1
	const bool doDebug = (callId < MDEI_MAX_DEBUG_FRAMES);
#else
	const bool doDebug = false;
#endif
#if MDEI_DEBUG_LEVEL >= 2
	const bool doDraw  = doDebug;
#else
	const bool doDraw  = false;
#endif
	(void)doDebug; (void)doDraw;

	/* ── 1. Upload geometry pools ────────────────────────────────────── */
	/* Deve acontecer antes do BeginFrame para que os firstIndex/baseVertex
	 * do pool estejam actualizados quando WriteInstancesAndCommand for chamado. */
	for (MDEI_DrawGroup *g : m_groups)
		if (g->IsPooled()) g->UploadPool();

	/* ── 2. Reset groups ─────────────────────────────────────────────── */
	for (MDEI_DrawGroup *g : m_groups) g->BeginFrame();

	/* ── 3. Accumulate visible instances ─────────────────────────────── */
	const int activeLayer = m_scene->GetBlenderScene() ? m_scene->GetBlenderScene()->lay : 0;

	for (KX_GameObject *obj : m_registeredObjects) {
		if (!obj->GetVisible()) continue;
		if (activeLayer != 0 && !(obj->GetLayer() & activeLayer)) continue;

		MDEI_ObjectProxy *proxy = obj->GetMdeiProxy();
		if (!proxy) continue;

		mt::mat4 W = mt::mat4::FromAffineTransform(obj->NodeGetWorldTransform());
		const float mat[16] = {
			W[0],  W[1],  W[2],  W[3],
			W[4],  W[5],  W[6],  W[7],
			W[8],  W[9],  W[10], W[11],
			W[12], W[13], W[14], W[15]
		};

		const float (&col)[4] = obj->GetObjectColor().Data();

		/* Passa o meshSlot para o grupo pooled; grupos privados ignoram-no */
		proxy->group->AddInstance(mat, col, proxy->meshSlot);
	}

	/* ── 4. Snapshot pending counts ─────────────────────────────────── */
	std::vector<unsigned int> counts(m_groups.size());
	unsigned int totalInstances = 0;
	for (size_t i = 0; i < m_groups.size(); i++) {
		counts[i] = m_groups[i]->PendingCount();
		totalInstances += counts[i];
	}

	/* ── 5. Write instance data into ring-buffer segment ────────────── */
	MDEI_Instance *ringBase = m_buffer.BeginFrame(frameIndex);
	if (!ringBase) {
		fprintf(stderr, "[MDEI] #%d [%s]: ringBase NULL — SSBO not mapped!\n",
		        callId, shadowPass ? "SHADOW" : "SOLID");
		m_buffer.EndFrame(frameIndex);
		frameIndex++;
		return;
	}

	std::vector<DrawElementsIndirectCommand> cmds;
	cmds.reserve(m_groups.size());

	/* cmdCounts[i] = number of commands actually appended by group i.
	 * For pooled groups this equals the number of valid-slot instances;
	 * for private groups it is always 0 or 1. */
	std::vector<int> cmdCounts(m_groups.size(), 0);

	unsigned int instanceOffset = 0;
	for (size_t i = 0; i < m_groups.size(); i++) {
		if (counts[i] == 0) continue;
		const int before = (int)cmds.size();
		m_groups[i]->WriteInstancesAndCommand(ringBase + instanceOffset,
		                                      instanceOffset, cmds);
		cmdCounts[i] = (int)cmds.size() - before;
		instanceOffset += counts[i];
	}

	if (cmds.empty()) {
		m_buffer.EndFrame(frameIndex);
		frameIndex++;
		return;
	}

	/* Alerta crítico: overflow do indirect buffer (alocado para 4096 cmds).
	 * Com o design de 1 cmd por (slot, grupo), isso só acontece com > 4096
	 * combinações distintas de (shader, mesh) visíveis — extremamente raro. */
	if ((int)cmds.size() > 4096) {
		fprintf(stderr,
		        "[MDEI] !! INDIRECT OVERFLOW: %d commands > 4096 — cmds truncated!\n",
		        (int)cmds.size());
	}

	m_buffer.UploadCommands(cmds);
	mdei_gl_check("UploadCommands");
	m_buffer.Bind(frameIndex);
	mdei_gl_check("Bind SSBO+IndirectBuf");

	/* ── 6. Emit one glMultiDrawElementsIndirect per shader ──────────── *
	 *
	 * Pooled groups: mesmo shader → mesmo VAO → comandos são contíguos →
	 *   UM único glMultiDrawElementsIndirect cobre TODAS as meshes.
	 * Grupos privados (skinned/EnsurePrivate): um draw por grupo (comportamento legado).
	 * ─────────────────────────────────────────────────────────────────── */
	MDEI_Shader   *curShader = nullptr;
	GLuint         curVAO    = 0;
	int            cmdStart  = 0;
	int            cmdCount  = 0;
	int            cmdCursor = 0;

	auto flushRun = [&]()
	{
		if (cmdCount == 0) return;

		const size_t byteOffset =
		    (size_t)cmdStart * sizeof(DrawElementsIndirectCommand);

		if (doDebug)
			fprintf(stderr,
			        "[MDEI] #%d [%s]: MDEI call — VAO=%u  cmds=%d  byteOff=%zu\n",
			        callId, shadowPass ? "SHADOW" : "SOLID",
			        curVAO, cmdCount, byteOffset);
#if MDEI_DEBUG_LEVEL >= 2
		if (doDraw) {
			for (int c = cmdStart; c < cmdStart + cmdCount; c++)
				mdei_print_cmd(c, cmds[c]);
		}
#endif

		glMultiDrawElementsIndirect(
		    GL_TRIANGLES,
		    GL_UNSIGNED_INT,
		    reinterpret_cast<const void *>(byteOffset),
		    (GLsizei)cmdCount,
		    0  /* stride=0: tightly packed */
		);
		mdei_gl_check("glMultiDrawElementsIndirect");
	};

	for (size_t i = 0; i < m_groups.size(); i++) {
		if (counts[i] == 0) continue;

		MDEI_DrawGroup *g       = m_groups[i];
		MDEI_Shader    *gShader = g->GetShader();
		GLuint          gVAO    = g->GetCurrentVAO();

		/* Verifica se o grupo tem geometria válida para desenhar.
		 * Se não, avança cmdCursor pelos comandos já gerados (para não
		 * desalinhar os offsets dos grupos seguintes) e pula. */
		bool hasValidGeometry = false;
		if (gVAO != 0) {
			if (!g->IsPooled()) {
				MDEI_Mesh *pm = g->GetMesh();
				hasValidGeometry = (pm && pm->GetIndexCount() > 0 && pm->GetCurrentVAO() != 0);
			}
			else {
				hasValidGeometry = (g->GetPool() && g->GetPool()->HasGeometry());
			}
		}

		if (!hasValidGeometry) {
			/* Avança o cursor para manter alinhamento com o buffer de comandos.
			 * Também reseta o run se estava a acumular para este grupo — os
			 * comandos do grupo pulado não foram desenhados, então o próximo
			 * grupo visível começa um run fresco. */
			if (cmdCounts[i] > 0) {
				flushRun();
				cmdCursor += cmdCounts[i];
				cmdStart   = cmdCursor;
				cmdCount   = 0;
			}
			continue;
		}

		bool shaderChanged = (gShader != curShader);
		bool vaoChanged    = (gVAO    != curVAO);

		if (shaderChanged || vaoChanged) {
			flushRun();
			cmdStart = cmdCursor;
			cmdCount = 0;

			if (shaderChanged) {
				if (curShader) {
					if (shadowPass) curShader->UnbindShadow();
					else            curShader->UnbindSolid();
				}
				curShader = gShader;
				if (shadowPass) curShader->BindShadow(rasty);
				else            curShader->BindSolid(rasty);

				if (!shadowPass) {
					const bool needCull = curShader->GetCullFace();
					if (needCull != rasty->GetCullFace())
						rasty->SetCullFace(needCull);
				}
			}

			if (vaoChanged) {
				if (curVAO) glBindVertexArray(0);
				curVAO = gVAO;
				glBindVertexArray(curVAO);

				if (!shadowPass && curShader) {
					GPUVertexAttribs attribs;
					memset(&attribs, 0, sizeof(attribs));
					if (curShader->GetGPUVertexAttribs(attribs)) {
						if (g->IsPooled() && g->GetPool()) {
							/* Qualquer slot activo serve — mesmo stride/layout. */
							MDEI_GeometryPool *pool = g->GetPool();
							for (auto &kv : m_meshToSlot) {
								if (kv.first.first == g) {
									pool->BindUVAttribs(kv.second, attribs);
									break;
								}
							}
						}
						else if (g->GetMesh()) {
							g->GetMesh()->BindUVAttribs(attribs);
						}
					}
				}
			}
		}

		/* Usa a contagem exacta de comandos gerados por este grupo */
		const int groupCmds = cmdCounts[i];
		if (groupCmds > 0) {
			cmdCount  += groupCmds;
			cmdCursor += groupCmds;
		}
	}

	flushRun();

	/* ── 7. Cleanup ─────────────────────────────────────────────────── */
	if (curShader) {
		if (shadowPass) curShader->UnbindShadow();
		else            curShader->UnbindSolid();
	}
	if (curVAO) glBindVertexArray(0);

	if (!shadowPass)
		rasty->SetCullFace(true);

	m_buffer.Unbind();
	m_buffer.EndFrame(frameIndex);
	frameIndex++;
}

/* ─── Public render entry points ────────────────────────────────────────── */

void MDEI_Renderer::RenderSolid(const std::vector<KX_GameObject *>& /*objects*/,
                                RAS_Rasterizer *rasty)
{
	ExecuteDraw(false, rasty);
}

void MDEI_Renderer::RenderShadow(const std::vector<KX_GameObject *>& /*objects*/,
                                 RAS_Rasterizer *rasty)
{
	ExecuteDraw(true, rasty);
}

void MDEI_Renderer::SetAnisotropicFiltering(short /*level*/)
{
	std::vector<MDEI_Shader *> visited;
	for (MDEI_DrawGroup *g : m_groups) {
		MDEI_Shader *sh = g ? g->GetShader() : nullptr;
		if (!sh) continue;
		bool already = false;
		for (MDEI_Shader *v : visited) { if (v == sh) { already = true; break; } }
		if (already) continue;
		visited.push_back(sh);
		sh->RebuildAllSamplerArrays();
	}
}

void MDEI_Renderer::SetMipmapping(bool enabled, int glFilterType)
{
	std::vector<MDEI_Shader *> visited;
	for (MDEI_DrawGroup *g : m_groups) {
		MDEI_Shader *sh = g ? g->GetShader() : nullptr;
		if (!sh) continue;
		bool already = false;
		for (MDEI_Shader *v : visited) { if (v == sh) { already = true; break; } }
		if (already) continue;
		visited.push_back(sh);
		sh->SetMipmapping(enabled, glFilterType);
	}
}
