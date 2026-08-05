/*
 * MDEI_SkinDeformer.cpp — Armature deformation for MDEI fast-path meshes.
 *
 * The math mirrors BL_SkinDeformer (BGEDeformVerts / BlenderDeformVerts) with
 * all RAS_DisplayArray references replaced by MDEI_Mesh ring-VBO writes.
 *
 * Timing: Update() calls m_armobj->ApplyPoseLocked() itself (under the pose
 * mutex) so that BKE_pose_where_is runs and chan_mat values are current.
 * This is identical to BL_SkinDeformer::UpdateInternal().
 */

/* Eigen3 (same as BL_SkinDeformer) */
#include <Eigen/Core>
#include <Eigen/LU>

#include "MDEI_SkinDeformer.h"
#include "MDEI_Mesh.h"

#include "BL_ArmatureObject.h"

#include "KX_GameObject.h"
#include "DNA_armature_types.h"
#include "DNA_action_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"

#include "BLI_utildefines.h"
#include "BLI_blenlib.h"
#include "BLI_math.h"
#include "BKE_armature.h"
#include "BKE_action.h"

extern "C" {
#  include "BKE_lattice.h"
#  include "BKE_deform.h"
#  include "BKE_mesh.h"
}

#include "CM_Thread.h"

#include <cstring>
#include <cstdio>

/* ── CM_AutoLock (same as the anonymous namespace in BL_SkinDeformer.cpp) ── */
namespace {
class CM_AutoLock
{
	CM_ThreadLock& m_lock;
public:
	CM_AutoLock(CM_ThreadLock& lock) : m_lock(lock) { m_lock.Lock(); }
	~CM_AutoLock() { m_lock.Unlock(); }
};
}

/* ── helpers copied from BL_SkinDeformer ──────────────────────────────────── */

static short mdei_get_deformflags(Object *bmeshobj)
{
	short flags = ARM_DEF_VGROUP;
	for (ModifierData *md = (ModifierData *)bmeshobj->modifiers.first; md; md = md->next) {
		if (md->type == eModifierType_Armature) {
			flags |= ((ArmatureModifierData *)md)->deformflag;
			break;
		}
	}
	return flags;
}

/* ── ctor / dtor ─────────────────────────────────────────────────────────── */

MDEI_SkinDeformer::MDEI_SkinDeformer(KX_GameObject     *gameobj,
                                     Object            *bmeshobj_old,
                                     Object            *bmeshobj_new,
                                     MDEI_Mesh         *mdeiMesh,
                                     BL_ArmatureObject *arma)
	: m_gameobj(gameobj)
	, m_objMesh(bmeshobj_old)
	, m_bmesh(static_cast<Mesh *>(bmeshobj_old->data))
	, m_armobj(arma)
	, m_mdeiMesh(mdeiMesh)
	, m_deformflags(mdei_get_deformflags(bmeshobj_new))
	, m_lastArmaUpdate(-1.0)
	, m_copyNormals(false)
{
	copy_m4_m4(m_obmat, bmeshobj_new->obmat);

	if (m_armobj) {
		m_armobj->RegisterObject(gameobj);
	}
}

MDEI_SkinDeformer::~MDEI_SkinDeformer()
{
	if (m_armobj) {
		m_armobj->UnregisterObject(m_gameobj);
	}
}

/* ── PoseUpdated ─────────────────────────────────────────────────────────── */
/*
 * NOTE: we can't rely solely on m_lastframe != m_lastArmaUpdate because
 * m_lastframe is only advanced by BL_Action/BL_ArmatureActuator when an
 * action is explicitly playing.  When the armature is moved by physics,
 * Python, or manual Transform, m_lastframe stays at its initial value and
 * PoseUpdated() would permanently return false after the first frame.
 *
 * Strategy: always deform if we have an armature — same as BL_SkinDeformer
 * which calls UpdateInternal() unconditionally every frame.  The only guard
 * we keep is against calling Update() before the armature has been processed
 * at least once (m_lastframe == 0 and m_lastapplyframe == 0 both on first
 * frame → ApplyPoseLocked is a no-op, but that's fine: we'd just re-upload
 * rest pose which is what the VBO already contains).
 */
bool MDEI_SkinDeformer::PoseUpdated() const
{
	/* Always return true when an armature is present — ensures deformation
	 * runs every frame regardless of how the armature is being driven. */
	return m_armobj != nullptr;
}

/* ── VerifyStorage ───────────────────────────────────────────────────────── */

void MDEI_SkinDeformer::VerifyStorage()
{
	const unsigned int totvert = (unsigned int)m_bmesh->totvert;
	if (m_transverts.size() != totvert) {
		m_transverts.resize(totvert);
		m_transnors.resize(totvert);
	}
	for (unsigned int v = 0; v < totvert; ++v) {
		copy_v3_v3(m_transverts[v].data, m_bmesh->mvert[v].co);
		normal_short_to_float_v3(m_transnors[v].data, m_bmesh->mvert[v].no);
	}
}

/* ── BlenderDeformVerts ──────────────────────────────────────────────────── */

void MDEI_SkinDeformer::BlenderDeformVerts()
{
	Object *par_arma = m_armobj ? m_armobj->GetArmatureObject() : nullptr;
	if (!par_arma) return;

	float obmat[4][4];
	copy_m4_m4(obmat, m_objMesh->obmat);
	copy_m4_m4(m_objMesh->obmat, m_obmat);

	armature_deform_verts(par_arma, m_objMesh, nullptr,
	                      (float (*)[3])m_transverts.data(), nullptr,
	                      m_bmesh->totvert, m_deformflags, nullptr, nullptr);

	copy_m4_m4(m_objMesh->obmat, obmat);

	RecalcNormals();
}

/* ── BGEDeformVerts ──────────────────────────────────────────────────────── */

void MDEI_SkinDeformer::BGEDeformVerts()
{
	Object *par_arma = m_armobj ? m_armobj->GetArmatureObject() : nullptr;
	if (!par_arma) return;

	MDeformVert *dverts = m_bmesh->dvert;
	if (!dverts) return;

	const unsigned short defbase_tot =
	    (unsigned short)BLI_listbase_count(&m_objMesh->defbase);

	if (m_dfnrToPC.empty()) {
		m_dfnrToPC.resize(defbase_tot);
		int i = 0;
		for (bDeformGroup *dg = (bDeformGroup *)m_objMesh->defbase.first;
		     dg; ++i, dg = dg->next)
		{
			m_dfnrToPC[i] = BKE_pose_channel_find_name(par_arma->pose, dg->name);
			if (m_dfnrToPC[i] && (m_dfnrToPC[i]->bone->flag & BONE_NO_DEFORM))
				m_dfnrToPC[i] = nullptr;
		}
	}

	Eigen::Matrix4f post_mat =
	    Eigen::Matrix4f::Map((float *)m_obmat).inverse()
	    * Eigen::Matrix4f::Map((float *)m_armobj->GetArmatureObject()->obmat);
	Eigen::Matrix4f pre_mat = post_mat.inverse();

	MDeformVert *dv = dverts;
	for (int i = 0; i < m_bmesh->totvert; ++i, dv++) {
		if (!dv->totweight) continue;

		float contrib = 0.0f, weight, max_weight = -1.0f;
		bPoseChannel *pchan = nullptr;
		Eigen::Matrix4f norm_chan_mat = Eigen::Matrix4f::Identity();
		Eigen::Vector4f vec(0.0f, 0.0f, 0.0f, 1.0f);
		Eigen::Vector4f co(m_transverts[i].x,
		                   m_transverts[i].y,
		                   m_transverts[i].z,
		                   1.0f);
		co = pre_mat * co;

		MDeformWeight *dw = dv->dw;
		for (unsigned int j = dv->totweight; j != 0; j--, dw++) {
			const int index = dw->def_nr;
			if (index < defbase_tot && (pchan = m_dfnrToPC[index])) {
				weight = dw->weight;
				if (weight) {
					Eigen::Matrix4f chan_mat =
					    Eigen::Matrix4f::Map((float *)pchan->chan_mat);
					vec.noalias() += (chan_mat * co - co) * weight;
					if (weight > max_weight) {
						max_weight     = weight;
						norm_chan_mat  = chan_mat;
					}
					contrib += weight;
				}
			}
		}

		if (contrib > 0.0f) {
			/* Update normal using most influential bone */
			const Eigen::Vector3f normorg(m_bmesh->mvert[i].no[0],
			                              m_bmesh->mvert[i].no[1],
			                              m_bmesh->mvert[i].no[2]);
			Eigen::Map<Eigen::Vector3f> norm =
			    Eigen::Vector3f::Map(m_transnors[i].data);
			norm = norm_chan_mat.topLeftCorner<3, 3>() * normorg;

			co.noalias() += vec / contrib;
		}
		co[3] = 1.0f;
		co = post_mat * co;
		m_transverts[i] = mt::vec3(co[0], co[1], co[2]);
	}
	m_copyNormals = true;
}

/* ── RecalcNormals ───────────────────────────────────────────────────────── */

void MDEI_SkinDeformer::RecalcNormals()
{
	const int totvert = m_bmesh->totvert;
	MVert *mverts = m_bmesh->mvert;

	/* Save original rest-pose positions so we can restore them after the
	 * normal recalculation without permanently mutating the Blender mesh. */
	struct Vec3f { float x, y, z; };
	std::vector<Vec3f> savedCo(totvert);
	for (int v = 0; v < totvert; ++v) {
		savedCo[v].x = mverts[v].co[0];
		savedCo[v].y = mverts[v].co[1];
		savedCo[v].z = mverts[v].co[2];
	}

	/* Temporarily write the deformed positions into the Blender mesh so
	 * BKE_mesh_calc_normals can work with the deformed topology. */
	for (int v = 0; v < totvert; ++v)
		copy_v3_v3(mverts[v].co, m_transverts[v].data);

	BKE_mesh_calc_normals(m_bmesh);  /* writes short normals into mverts[].no */

	/* Read back the recalculated normals, then restore original positions. */
	if (m_transnors.size() != (size_t)totvert)
		m_transnors.resize(totvert);

	for (int v = 0; v < totvert; ++v) {
		normal_short_to_float_v3(m_transnors[v].data, mverts[v].no);
		mverts[v].co[0] = savedCo[v].x;   /* restore rest-pose */
		mverts[v].co[1] = savedCo[v].y;
		mverts[v].co[2] = savedCo[v].z;
	}
}

/* ── FlushToVBO ──────────────────────────────────────────────────────────── */
/*
 * Write the deformed positions and normals directly into the persistent VBO
 * ring buffer.  Flow:
 *   BeginSkinFrame() → returns ptr to the segment we can write to (GPU fence)
 *   Write pos+nor per VBO vertex using origIndexMap
 *   EndSkinFrame()   → places fence so GPU won't read until done
 *
 * The UV data already sits in the VBO from the initial upload and is NOT
 * touched here — only floats at offsets 0-5 (pos+nor) are overwritten.
 * UV offsets (24+ bytes) are preserved because every segment was pre-filled
 * with the full packed vertex data in Upload().
 */
void MDEI_SkinDeformer::FlushToVBO()
{
	if (!m_mdeiMesh->IsSkinned()) return;

	const int   floatsPerVert = m_mdeiMesh->GetFloatsPerVert();
	const int   vertCount     = (int)m_mdeiMesh->GetVertCount();

	float *segPtr = m_mdeiMesh->BeginSkinFrame();
	if (!segPtr) return;

	const std::vector<int>& origMap = m_mdeiMesh->m_origIndexMap;

	for (int vi = 0; vi < vertCount; ++vi) {
		const int origIdx = origMap[vi];
		float *dst = segPtr + (size_t)vi * (size_t)floatsPerVert;

		/* Positions: offset 0, 1, 2 */
		dst[0] = m_transverts[origIdx].x;
		dst[1] = m_transverts[origIdx].y;
		dst[2] = m_transverts[origIdx].z;

		/* Normals: offset 3, 4, 5 */
		dst[3] = m_transnors[origIdx].x;
		dst[4] = m_transnors[origIdx].y;
		dst[5] = m_transnors[origIdx].z;

		/* UVs at dst[6..] are already correct from the pre-fill in Upload() */
	}

	m_mdeiMesh->EndSkinFrame();
}

/* ── Update ──────────────────────────────────────────────────────────────── */
/*
 * Mirrors BL_SkinDeformer::UpdateInternal() exactly:
 *   1. Lock pose mutex
 *   2. Call ApplyPoseLocked() → BKE_pose_where_is sets chan_mat
 *   3. Run deformation (BGE or Blender path)
 *   4. Write result into the ring VBO
 *
 * The lock is required because multiple mesh children share the same
 * armature and UpdateDeformerForObject() can be called concurrently
 * from the task pool.
 */
bool MDEI_SkinDeformer::Update()
{
	if (!m_armobj) return false;

	/* ── Apply the current pose (calc chan_mat, update obmat) ── */
	{
		CM_AutoLock lock(BL_ArmatureObject::GetPoseMutex());
		m_armobj->ApplyPoseLocked();
	}

	if (!m_armobj) return false;   /* safety — ApplyPoseLocked might clear it */

	/* Restore rest-pose positions into m_transverts */
	VerifyStorage();

#if MDEI_SKIN_DEBUG >= 1
	/* Print armature world position so we can tell if obmat is current. */
	{
		Object *armaObj = m_armobj->GetArmatureObject();
		if (armaObj) {
			fprintf(stderr, "[MDEI][SKIN] Update '%s': arma_pos=(%.3f %.3f %.3f)  lastFrame=%.3f\n",
			        m_gameobj->GetName().c_str(),
			        armaObj->obmat[3][0], armaObj->obmat[3][1], armaObj->obmat[3][2],
			        (float)m_armobj->GetLastFrame());
		}
	}
#endif

	if (m_armobj->GetVertDeformType() == ARM_VDEF_BGE_CPU) {
#if MDEI_SKIN_DEBUG >= 2
		fprintf(stderr, "[MDEI][SKIN]   path=BGE_CPU  totvert=%d  defbase=%d\n",
		        m_bmesh->totvert,
		        (int)BLI_listbase_count(&m_objMesh->defbase));
#endif
		BGEDeformVerts();
	}
	else {
#if MDEI_SKIN_DEBUG >= 2
		fprintf(stderr, "[MDEI][SKIN]   path=BLENDER  totvert=%d\n", m_bmesh->totvert);
#endif
		BlenderDeformVerts();
	}

	m_lastArmaUpdate = m_armobj->GetLastFrame();
	FlushToVBO();

#if MDEI_SKIN_DEBUG >= 2
	/* Print first deformed vert to confirm movement. */
	if (!m_transverts.empty()) {
		fprintf(stderr, "[MDEI][SKIN]   vert[0] deformed=(%.3f %.3f %.3f)\n",
		        m_transverts[0].x, m_transverts[0].y, m_transverts[0].z);
	}
#endif

	return true;
}
