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
#if MDEI_DEBUG_LEVEL >= 1
			fprintf(stderr, "[MDEI] Register '%s': compiled shader — "
			        "unfviewmat=%d unfinvviewmat=%d unftime=%d\n",
			        gameobj->GetName().c_str(),
			        shader->m_locViewMat, shader->m_locInvViewMat, shader->m_locTime);
#endif
			m_shaderCache[key] = { shader, gpuMat };
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
	fprintf(stderr, "[MDEI] RegisterArmature '%s': skinned mesh VAO=%u  deformer=%p\n",
	        gameobj->GetName().c_str(), mesh->GetVAO(), (void *)deformer);
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

void MDEI_Renderer::UpdateDeformerForObject(KX_GameObject *gameobj)
{
	auto it = m_skinDeformers.find(gameobj);
	if (it != m_skinDeformers.end()) {
		it->second->Update();
	}
}

void MDEI_Renderer::UpdateDeformers()
{
	for (auto &kv : m_skinDeformers) {
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
	const bool doDebug  = (MDEI_DEBUG_LEVEL >= 1) && (callId < MDEI_MAX_DEBUG_FRAMES * 2);
	const bool doDetail = (MDEI_DEBUG_LEVEL >= 2) && (callId < MDEI_MAX_DEBUG_FRAMES * 2);

	/* ── 1. Reset groups ─────────────────────────────────────────────── */
	for (MDEI_DrawGroup *g : m_groups) g->BeginFrame();

	/* ── 2. Accumulate visible instances from our own registered list ──
	 *
	 * We do NOT use the `objects` list passed from RenderBuckets because:
	 *   • Shadow pass: objects are filtered by HasShadowCasterMaterial()
	 *     which is always false for MDEI objects (no RAS_MeshUser).
	 *   • Solid pass: CalculateVisibleMeshes uses DBVT physics broadphase;
	 *     MDEI objects have no physics body so they are invisible to it.
	 *
	 * Instead we iterate m_registeredObjects and apply GetVisible() only.
	 * ------------------------------------------------------------------ */
	int nVisible = 0;
	for (KX_GameObject *obj : m_registeredObjects) {
		if (!obj->GetVisible()) {
			if (doDebug)
				fprintf(stderr, "[MDEI] #%d [%s]: '%s' skipped — GetVisible()=false\n",
				        callId, shadowPass ? "SHADOW" : "SOLID",
				        obj->GetName().c_str());
			continue;
		}

		MDEI_ObjectProxy *proxy = obj->GetMdeiProxy();
		if (!proxy) {
			fprintf(stderr, "[MDEI] #%d [%s]: '%s' has no proxy — skipped\n",
			        callId, shadowPass ? "SHADOW" : "SOLID",
			        obj->GetName().c_str());
			continue;
		}

		/* Column-major world matrix from SG_Node. */
		mt::mat4 W = mt::mat4::FromAffineTransform(obj->NodeGetWorldTransform());
		const float mat[16] = {
			W[0],  W[1],  W[2],  W[3],
			W[4],  W[5],  W[6],  W[7],
			W[8],  W[9],  W[10], W[11],
			W[12], W[13], W[14], W[15]
		};

		if (doDetail)
			fprintf(stderr, "[MDEI] #%d [%s]: '%s' pos(%.3f %.3f %.3f)\n",
			        callId, shadowPass ? "SHADOW" : "SOLID",
			        obj->GetName().c_str(), mat[12], mat[13], mat[14]);

		const float (&col)[4] = obj->GetObjectColor().Data();
		proxy->group->AddInstance(mat, col);
		nVisible++;
	}

	if (doDebug)
		fprintf(stderr,
		        "[MDEI] #%d [%s]: registered=%d  visible=%d  groups=%d  ringSlot=%d\n",
		        callId, shadowPass ? "SHADOW" : "SOLID",
		        (int)m_registeredObjects.size(), nVisible,
		        (int)m_groups.size(), frameIndex % MDEI_RING_SEGMENTS);

	/* ── 3. Snapshot pending counts (before Write clears the lists) ─── */
	std::vector<unsigned int> counts(m_groups.size());
	for (size_t i = 0; i < m_groups.size(); i++)
		counts[i] = m_groups[i]->PendingCount();

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
		if (doDebug)
			fprintf(stderr, "[MDEI] #%d [%s]: 0 commands — nothing to draw.\n",
			        callId, shadowPass ? "SHADOW" : "SOLID");
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
		if (doDetail) {
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

		bool shaderChanged = (gShader != curShader);
		bool meshChanged   = (gMesh   != curMesh);

		if (shaderChanged || meshChanged) {
			flushRun();
			cmdStart = cmdCursor;
			cmdCount = 0;

			if (curShader) {
				if (shadowPass) curShader->UnbindShadow();
				else            curShader->UnbindSolid();
			}
			if (curMesh && meshChanged) {
				glBindVertexArray(0);
				mdei_gl_check("UnbindVAO");
			}

			if (shaderChanged) {
				curShader = gShader;
				if (shadowPass) {
					curShader->BindShadow(rasty);
					mdei_gl_check("BindShadow");
				}
				else {
					curShader->BindSolid(rasty);
					mdei_gl_check("BindSolid");
				}
			}

			if (meshChanged) {
				curMesh = gMesh;
				glBindVertexArray(curMesh->GetCurrentVAO());
				mdei_gl_check("BindVAO");

				if (doDebug)
						fprintf(stderr,
						        "[MDEI] #%d [%s]: VAO=%u  indexCount=%d\n",
						        callId, shadowPass ? "SHADOW" : "SOLID",
						        curMesh->GetCurrentVAO(), (int)curMesh->GetIndexCount());

				/* Bind UV generic attributes into the VAO.
				 * Only needed for solid pass — shadow shader doesn't use UVs. */
				if (!shadowPass && curShader) {
					GPUVertexAttribs attribs;
					memset(&attribs, 0, sizeof(attribs));
					if (curShader->GetGPUVertexAttribs(attribs)) {
						curMesh->BindUVAttribs(attribs);
						mdei_gl_check("BindUVAttribs");
					}
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
	if (curMesh) {
		glBindVertexArray(0);
		mdei_gl_check("FinalUnbindVAO");
	}

	m_buffer.Unbind();
	mdei_gl_check("Unbind SSBO+IndirectBuf");
	m_buffer.EndFrame(frameIndex);

	if (doDebug)
		fprintf(stderr,
		        "[MDEI] #%d [%s]: done — %d cmd(s)  %u instance(s)\n",
		        callId, shadowPass ? "SHADOW" : "SOLID",
		        (int)cmds.size(), instanceOffset);

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
