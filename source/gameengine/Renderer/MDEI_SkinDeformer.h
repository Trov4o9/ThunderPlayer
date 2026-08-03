/*
 * MDEI_SkinDeformer.h — Armature-driven vertex deformation for MDEI meshes.
 *
 * Mirrors the math of BL_SkinDeformer (BGEDeformVerts / BlenderDeformVerts)
 * but writes its results directly into the MDEI_Mesh VBO via
 * MDEI_Mesh::UpdatePositionsNormals(), bypassing all RAS structures.
 *
 * Does NOT inherit from RAS_Deformer — it is completely independent of the
 * RAS pipeline and is owned by MDEI_Renderer.
 */

#ifndef __MDEI_SKIN_DEFORMER_H__
#define __MDEI_SKIN_DEFORMER_H__

#include "mathfu.h"   /* mt::vec3_packed */

#include <vector>

struct Object;
struct Mesh;
struct bPoseChannel;
class  KX_GameObject;
class  BL_ArmatureObject;
class  MDEI_Mesh;

class MDEI_SkinDeformer {
public:
	/**
	 * @param gameobj        KX_GameObject that owns this deformer.
	 * @param bmeshobj_old   Blender Object whose data is the Mesh (may differ
	 *                       from bmeshobj_new when the mesh was replaced).
	 * @param bmeshobj_new   Blender Object used as the deformation reference
	 *                       (provides obmat used in armature_deform_verts).
	 * @param mdeiMesh       The MDEI_Mesh whose VBO will be patched every frame.
	 * @param arma           The parent BL_ArmatureObject driving the pose.
	 */
	MDEI_SkinDeformer(KX_GameObject     *gameobj,
	                  Object            *bmeshobj_old,
	                  Object            *bmeshobj_new,
	                  MDEI_Mesh         *mdeiMesh,
	                  BL_ArmatureObject *arma);

	~MDEI_SkinDeformer();

	/** Called every frame by MDEI_Renderer::UpdateDeformers().
	 *  Returns true if the pose changed and the VBO was re-uploaded. */
	bool Update();

	/** Force update on next frame (used after armature is re-linked). */
	void ForceUpdate() { m_lastArmaUpdate = -1.0; }

private:
	/** True if the armature pose changed since last Update(). */
	bool PoseUpdated() const;

	/** Populate m_transverts / m_transnors from rest-pose mvert data. */
	void VerifyStorage();

	/**
	 * Deform using Blender's armature_deform_verts (matches Blender's own
	 * result, heavier but identical to viewport). */
	void BlenderDeformVerts();

	/** BGE's own lighter deformation (linear blend skinning via Eigen). */
	void BGEDeformVerts();

	/** Recalculate smooth vertex normals after position update. */
	void RecalcNormals();

	/** Push deformed m_transverts/m_transnors into the MDEI_Mesh VBO. */
	void FlushToVBO();

	KX_GameObject     *m_gameobj;
	Object            *m_objMesh;       /* Object owning the Mesh* */
	Mesh              *m_bmesh;         /* Blender Mesh */
	BL_ArmatureObject *m_armobj;        /* parent armature */
	MDEI_Mesh         *m_mdeiMesh;      /* target VBO */

	float  m_obmat[4][4];              /* reference object matrix for deform */
	short  m_deformflags;              /* ARM_DEF_* flags from ArmatureModifier */
	double m_lastArmaUpdate;           /* last armature frame we processed */
	bool   m_copyNormals;              /* dirty flag for normal re-copy */

	/** Deformed vertex positions in Blender object space (size = Mesh.totvert) */
	std::vector<mt::vec3_packed> m_transverts;
	/** Deformed vertex normals (size = Mesh.totvert) */
	std::vector<mt::vec3_packed> m_transnors;
	/** Bone channel cache (size = defbase count), built lazily */
	std::vector<bPoseChannel *>  m_dfnrToPC;
};

#endif /* __MDEI_SKIN_DEFORMER_H__ */
