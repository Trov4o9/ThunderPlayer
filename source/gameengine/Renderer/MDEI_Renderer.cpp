/*
 * MDEI_Renderer.cpp
 *
 * Draw path:
 *   For each unique (shader, mesh) pair that has visible instances:
 *     1. Bind shader once
 *     2. Bind VAO once
 *     3. ONE glMultiDrawElementsIndirect call covering ALL commands for that pair
 *
 * The command buffer is laid out as:
 *   [ group0_cmd | group1_cmd | ... ]
 * Groups are sorted by (shader ptr, mesh ptr) so that contiguous runs share
 * the same VAO+shader — the multi-draw call covers the whole run in one call.
 */

#include "MDEI_Renderer.h"
#include "MDEI_Mesh.h"
#include "MDEI_MeshBuilder.h"
#include "MDEI_Shader.h"
#include "MDEI_DrawGroup.h"
#include "MDEI_ObjectProxy.h"
#include "MDEI_SkinDeformer.h"

#include "KX_GameObject.h"
#include "KX_Scene.h"
#include "KX_LightObject.h"

#include "RAS_Rasterizer.h"
#include "RAS_ILightObject.h"

#include "GPU_material.h"
#include "GPU_shader.h"
/* gpu_codegen.h omitted — GPUPass is used only as an opaque pointer here,
 * forward-declared via GPU_material.h (GPU_material_get_pass). */

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

/* How many frames to keep printing level-1 diagnostics. */
#define MDEI_MAX_DEBUG_FRAMES 8

/* Capacidade máxima do SSBO ring-buffer de instâncias.
 * Deve bater com MDEI_MAX_INSTANCES em MDEI_PersistentBuffer. */
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
	, m_solidFrame(0)                    /* slots 0,1,2,3,4,5,0,... */
	, m_shadowFrame(MDEI_RING_SEGMENTS / 2)  /* starts at 3 — never overlaps solid */
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

			/* Cull face: GEMAT_BACKCULL=16 — se setado, cull face está ON. */
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

	/* ── 3. DrawGroup ────────────────────────────────────────────────── */
	MDEI_DrawGroup *group = GetOrCreateGroup(mesh, shader);

	/* ── 4. Proxy ────────────────────────────────────────────────────── */
	MDEI_ObjectProxy *proxy = new MDEI_ObjectProxy(mesh, group);
	gameobj->SetMdeiProxy(proxy);
	gameobj->SetFastRenderFlag(true);

	/* AABB in local space — sufficient for static objects. */
	gameobj->GetCullingNode().GetAabb().Set(mesh->m_aabbMin, mesh->m_aabbMax);

	/* Track this object in our own list — bypasses RAS culling pipeline. */
	m_registeredObjects.push_back(gameobj);

#if MDEI_DEBUG_LEVEL >= 1
	fprintf(stderr, "[MDEI] Register '%s': OK — group=%p  total_registered=%d\n",
	        gameobj->GetName().c_str(), (void *)group,
	        (int)m_registeredObjects.size());
#endif
}

void MDEI_Renderer::RegisterReplica(KX_GameObject *replica,
	                                   KX_GameObject *original)
{
	if (!original || !original->HasFastRenderFlag()) return;

	MDEI_ObjectProxy *src = original->GetMdeiProxy();
	if (!src) return;

	MDEI_ObjectProxy *proxy = new MDEI_ObjectProxy(src->mesh, src->group);
	replica->SetMdeiProxy(proxy);
	replica->SetFastRenderFlag(true);

	replica->GetCullingNode().GetAabb().Set(src->mesh->m_aabbMin, src->mesh->m_aabbMax);

	m_registeredObjects.push_back(replica);

#if MDEI_DEBUG_LEVEL >= 1
	fprintf(stderr,
	        "[MDEI] RegisterReplica '%s' <- original='%s'"
	        "  mesh=%p  group=%p  shader=%p  VAO=%u"
	        "  total_registered=%d\n",
	        replica->GetName().c_str(),
	        original->GetName().c_str(),
	        (void *)src->mesh,
	        (void *)src->group,
	        (void *)(src->group ? src->group->GetShader() : nullptr),
	        src->mesh ? src->mesh->GetVAO() : 0u,
	        (int)m_registeredObjects.size());
#endif
}

void MDEI_Renderer::RegisterArmature(KX_GameObject     *gameobj,
                                     Object            *blenderObjNew,
                                     BL_ArmatureObject *arma)
{
	if (!gameobj || !arma) return;

	/* The mesh for this object must already be in m_meshCache (registered by
	 * RegisterObject).  We need a *separate* MDEI_Mesh (with GL_STREAM_DRAW)
	 * because each skinned object has its own pose. */
	MDEI_Mesh *mesh = nullptr;
	{
		/* Try to find the existing static mesh for this object's data pointer.
		 * NOTE: blenderObjNew is `ob` from BL_BlenderDataConversion — it is
		 * always valid here.  gameobj->GetBlenderObject() is NOT yet set at
		 * conversion time (RegisterGameObject runs after the switch), so we
		 * must use blenderObjNew everywhere in this function. */
		void *staticKey = blenderObjNew->data;
		auto  it        = m_meshCache.find(staticKey);
		if (it == m_meshCache.end()) {
			fprintf(stderr, "[MDEI] RegisterArmature '%s': no base mesh in cache\n",
			        gameobj->GetName().c_str());
			return;
		}

		/* Check if a skinned copy already exists for this object.
		 * Key = blenderObjNew (stable pointer, unique per object). */
		void *skinnedKey = (void *)blenderObjNew;
		auto  sit        = m_skinnedMeshCache.find(skinnedKey);
		if (sit != m_skinnedMeshCache.end()) {
			mesh = sit->second;
		}
		else {
			/* Build a fresh MDEI_Mesh with origIndexMap populated so Upload()
			 * allocates a persistent ring-VBO for skin deformation.
			 * isSkinned=true → origIndexMap is kept and glBufferStorage is used. */
			mesh = MDEI_MeshBuilder::Build(blenderObjNew,
			                               m_scene->GetBlenderScene(),
			                               /*isSkinned=*/true);
			if (!mesh) {
				fprintf(stderr, "[MDEI] RegisterArmature '%s': mesh rebuild failed\n",
				        gameobj->GetName().c_str());
				return;
			}
			m_skinnedMeshCache[skinnedKey] = mesh;

			/* Swap the proxy's mesh pointer so draw uses the skinned VBO */
			MDEI_ObjectProxy *proxy = gameobj->GetMdeiProxy();
			if (proxy) {
				/* Re-create the draw group for the new (unique) mesh */
				MDEI_DrawGroup *group = GetOrCreateGroup(mesh, proxy->group->GetShader());
				proxy->mesh  = mesh;
				proxy->group = group;
			}
		}
	}

	/* Create skin deformer.
		* Both bmeshobj_old and bmeshobj_new are blenderObjNew — the deformer
		* only needs the Mesh* (from data) and the reference obmat. */
	MDEI_SkinDeformer *deformer = new MDEI_SkinDeformer(
		   gameobj,
		   blenderObjNew,   /* bmeshobj_old: Object that owns the Mesh* */
		   blenderObjNew,   /* bmeshobj_new: provides obmat reference   */
		   mesh,
		   arma);

	m_skinDeformers[gameobj] = deformer;

#if MDEI_DEBUG_LEVEL >= 1
	fprintf(stderr, "[MDEI] RegisterArmature '%s': skinned mesh VAO=%u  deformer=%p  "
	        "total_deformers=%d\n",
	        gameobj->GetName().c_str(), mesh->GetVAO(), (void *)deformer,
	        (int)m_skinDeformers.size());
#else
	/* Always print armature registration so the user knows deformation is wired up. */
	fprintf(stderr, "[MDEI] Armature registered for '%s' (skin deformers: %d)\n",
	        gameobj->GetName().c_str(), (int)m_skinDeformers.size());
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

	MDEI_Mesh *meshBefore = proxy->mesh;
	GLuint vaoBefore = meshBefore->GetVAO();

	/* ── 1. Garanta mesh e DrawGroup exclusivos ──────────────────────── */
	EnsurePrivateMesh(gameobj);

	/* Após EnsurePrivateMesh proxy->mesh pode ter mudado */
	MDEI_Mesh *privateMesh = proxy->mesh;
	GLuint vaoPrivate = privateMesh->GetVAO();

	/* ── 2. Libera geometria GPU e cria mesh vazio ───────────────────── */
	privateMesh->Release();
	delete privateMesh;
	MDEI_Mesh *fresh = new MDEI_Mesh();
	proxy->mesh = fresh;
	if (proxy->group) proxy->group->SetMesh(fresh);

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

	MDEI_Mesh   *oldMesh   = proxy->mesh;
	MDEI_Shader *oldShader = proxy->group ? proxy->group->GetShader() : nullptr;
	MDEI_DrawGroup *oldGroup = proxy->group;

	/* Verifica se o mesh ainda é compartilhado. */
	bool isShared = false;
	void *sharedKey = nullptr;
	for (auto &kv : m_meshCache) {
		if (kv.second == proxy->mesh) {
			isShared  = true;
			sharedKey = kv.first;
			break;
		}
	}
	if (!isShared) {
		for (auto &kv : m_skinnedMeshCache) {
			if (kv.second == proxy->mesh) {
				isShared  = true;
				sharedKey = kv.first;
				break;
			}
		}
	}

	if (!isShared) return; /* já é privado — nada a fazer */

	/* Conta quantos outros objetos registrados compartilham esse mesmo mesh
	 * (inclui o próprio gameobj — resultado esperado ≥ 1). */
	int sharingCount = 0;
	for (KX_GameObject *other : m_registeredObjects) {
		MDEI_ObjectProxy *op = other->GetMdeiProxy();
		if (op && op->mesh == oldMesh) sharingCount++;
	}

	/* Cria mesh privado + DrawGroup dedicado sem liberar o mesh compartilhado. */
	MDEI_Mesh *fresh = new MDEI_Mesh();
	proxy->mesh = fresh;

	MDEI_DrawGroup *dedicated = nullptr;
	if (oldShader) {
		dedicated = new MDEI_DrawGroup(fresh, oldShader);
		m_groups.push_back(dedicated);
		proxy->group = dedicated;
	} else if (proxy->group) {
		proxy->group->SetMesh(fresh);
	}

	m_ensurePrivateCount++;

#if MDEI_DEBUG_LEVEL >= 1
	fprintf(stderr,
	        "[MDEI] EnsurePrivateMesh '%s': PRIVATIZED #%d"
	        "  sharedKey=%p  old_mesh=%p (VAO=%u)  old_group=%p"
	        "  new_mesh=%p  new_group=%p  shader=%p"
	        "  objects_sharing_old_mesh=%d"
	        "  total_groups_now=%d\n",
	        gameobj->GetName().c_str(),
	        m_ensurePrivateCount,
	        sharedKey,
	        (void *)oldMesh,
	        oldMesh->GetVAO(),
	        (void *)oldGroup,
	        (void *)fresh,
	        (void *)dedicated,
	        (void *)oldShader,
	        sharingCount,
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
	else {
#if MDEI_DEBUG_LEVEL >= 1
		/* Object is MDEI but has no skin deformer — static mesh, expected. */
		fprintf(stderr, "[MDEI] UpdateDeformerForObject: '%s' — no deformer (static)\n",
		        gameobj->GetName().c_str());
#endif
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

MDEI_DrawGroup *MDEI_Renderer::GetOrCreateGroup(MDEI_Mesh *mesh, MDEI_Shader *shader)
{
	for (MDEI_DrawGroup *g : m_groups)
		if (g->GetMesh() == mesh && g->GetShader() == shader)
			return g;

	MDEI_DrawGroup *g = new MDEI_DrawGroup(mesh, shader);
	m_groups.push_back(g);
	return g;
}

/* ─── Render ────────────────────────────────────────────────────────────── */

void MDEI_Renderer::ExecuteDraw(bool shadowPass, RAS_Rasterizer *rasty)
{
	if (m_groups.empty() || m_registeredObjects.empty()) return;

	/* ── 0. Restore GL_MODELVIEW to view-only matrix ─────────────────────
	 * The RAS may have left GL_MODELVIEW = viewMatrix * lastObjectMatrix.
	 * Our patched vertex shader applies _inst.modelMatrix itself, then
	 * multiplies by gl_ModelViewMatrix — so gl_ModelViewMatrix must be
	 * the view matrix alone, NOT the product with any RAS object matrix.
	 * --------------------------------------------------------------------- */
	{
		const mt::mat4& vm = rasty->GetViewMatrix();
		rasty->SetMatrixMode(RAS_Rasterizer::RAS_MODELVIEW);
		rasty->LoadMatrix(&vm.Data()[0][0]);
	}

	/* Each pass has its own ring-buffer index so solid and shadow never
	 * share a slot within the same game frame. */
	int &frameIndex = shadowPass ? m_shadowFrame : m_solidFrame;

	const int  callId   = m_debugCallCount++;
	(void)callId; /* evita warning quando todos os prints estão desativados */

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

	/* ── 1. Reset groups ─────────────────────────────────────────────── */
	for (MDEI_DrawGroup *g : m_groups) g->BeginFrame();

	/* ── 2. Accumulate visible instances ─────────────────────────────── */
	/* Layer ativo da cena: filtra objetos que não estão na layer visível.
	 * Se lay == 0 (nenhum layer ativo) não filtramos — renderiza tudo. */
	const int activeLayer = m_scene->GetBlenderScene() ? m_scene->GetBlenderScene()->lay : 0;

	for (KX_GameObject *obj : m_registeredObjects) {
		if (!obj->GetVisible()) continue;
		/* Filtro por layer: objeto deve ter ao menos um bit em comum com o
		 * layer ativo da cena, mesmo comportamento de Renderable() no RAS. */
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
		proxy->group->AddInstance(mat, col);
	}

	/* ── 3. Snapshot pending counts ─────────────────────────────────── */
	std::vector<unsigned int> counts(m_groups.size());
	unsigned int totalInstances = 0;
	for (size_t i = 0; i < m_groups.size(); i++) {
		counts[i] = m_groups[i]->PendingCount();
		totalInstances += counts[i];
	}

	/* Alerta crítico: overflow do indirect buffer (alocado para 4096 cmds) */
	if ((int)m_groups.size() > 4096) {
		fprintf(stderr,
		        "[MDEI] !! INDIRECT OVERFLOW: %d groups > 4096 — cmds truncated!\n",
		        (int)m_groups.size());
	}

	/* ── 4. Write instance data into ring-buffer segment ────────────── */
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

	unsigned int instanceOffset = 0;
	for (size_t i = 0; i < m_groups.size(); i++) {
		if (counts[i] == 0) continue;
		m_groups[i]->WriteInstancesAndCommand(ringBase + instanceOffset,
		                                      instanceOffset, cmds);
		instanceOffset += counts[i];
	}

	if (cmds.empty()) {
		m_buffer.EndFrame(frameIndex);
		frameIndex++;
		return;
	}

	m_buffer.UploadCommands(cmds);
	mdei_gl_check("UploadCommands");
	m_buffer.Bind(frameIndex);
	mdei_gl_check("Bind SSBO+IndirectBuf");

	/* ── 5. Emit one glMultiDrawElementsIndirect per (shader, mesh) run ─
	 *
	 * Groups that share the same (shader, mesh) produce contiguous commands
	 * in the command buffer — one MDEI call covers all of them.
	 * -------------------------------------------------------------------- */
	MDEI_Shader   *curShader = nullptr;
	MDEI_Mesh     *curMesh   = nullptr;
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
			        curMesh ? curMesh->GetCurrentVAO() : 0u,
			        cmdCount, byteOffset);
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
		MDEI_Mesh      *gMesh   = g->GetMesh();

		/* Pula grupos cujo mesh ainda não tem geometria (ex: entre Release e
		 * o próximo mdei_update_mesh).  O cmdCursor NÃO avança pois
		 * WriteInstancesAndCommand também não adicionou comando ao vetor. */
		if (!gMesh || gMesh->GetIndexCount() == 0 || gMesh->GetVAO() == 0)
			continue;

		bool shaderChanged = (gShader != curShader);
		bool meshChanged   = (gMesh   != curMesh);

		if (shaderChanged || meshChanged) {
			flushRun();
			cmdStart = cmdCursor;
			cmdCount = 0;

			/* Só faz unbind/bind do shader quando ele realmente muda.
			 * Se apenas o mesh mudou (mesmo shader, mesh privado diferente)
			 * o shader permanece bindado — apenas o VAO é trocado.
			 * Bug anterior: UnbindSolid() era chamado mesmo sem shaderChanged,
			 * desligando o shader entre grupos com mesmo shader mas VAOs
			 * diferentes — todos os draws após o primeiro rodavam sem shader. */
			if (shaderChanged) {
				if (curShader) {
					if (shadowPass) curShader->UnbindShadow();
					else            curShader->UnbindSolid();
				}
				curShader = gShader;
				if (shadowPass) curShader->BindShadow(rasty);
				else            curShader->BindSolid(rasty);
	
				/* Aplica cull face do material se diferente do estado cached.
				 * O pass de sombra não precisa de cull face por material —
				 * usa o estado default deixado pelo RAS. */
				if (!shadowPass) {
					const bool needCull = curShader->GetCullFace();
					if (needCull != rasty->GetCullFace())
						rasty->SetCullFace(needCull);
				}
			}

			if (meshChanged) {
				if (curMesh) glBindVertexArray(0);
				curMesh = gMesh;
				glBindVertexArray(curMesh->GetCurrentVAO());

				if (!shadowPass && curShader) {
					GPUVertexAttribs attribs;
					memset(&attribs, 0, sizeof(attribs));
					if (curShader->GetGPUVertexAttribs(attribs))
						curMesh->BindUVAttribs(attribs);
				}
			}
		}

		cmdCount++;
		cmdCursor++;
	}

	flushRun();

	/* ── 6. Cleanup ─────────────────────────────────────────────────── */
	if (curShader) {
		if (shadowPass) curShader->UnbindShadow();
		else            curShader->UnbindSolid();
	}
	if (curMesh) glBindVertexArray(0);

	/* Restaura cull face padrão (true) após o draw MDEI. O RAS assume que
	 * GL_CULL_FACE está ligado ao retomar o pipeline normal. */
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
	/* GPU_set_anisotropic() já atualizou GTS.anisotropic antes desta chamada.
	 * Percorremos todos os grupos e forçamos rebuild dos sampler arrays de cada
	 * shader — RebuildAllSamplerArrays() recriará cada GL_TEXTURE_2D_ARRAY com
	 * o novo nível lido via GPU_get_anisotropic(), mantendo o estado mipmap.
	 * Shaders duplicados (mesmo ponteiro) são pulados para não recriar duas vezes. */
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
	/* Percorre shaders únicos e recria todos os sampler arrays com o novo
	 * estado de mipmap.  SetMipmapping em cada shader já força srcIds.clear()
	 * + dirty = true antes de chamar RebuildSamplerArray, portanto as texturas
	 * sempre serão recriadas independente de as fontes terem mudado.
	 * glFilterType == 0 significa "usar GPU_get_mipmap_filter() global". */
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
