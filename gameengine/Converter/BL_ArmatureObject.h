/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file BL_ArmatureObject.h
 *  \ingroup bgeconv
 */

#ifndef __BL_ARMATUREOBJECT_H__
#define __BL_ARMATUREOBJECT_H__

#include "KX_GameObject.h"
#include "BL_ArmatureConstraint.h"
#include "BL_ArmatureChannel.h"
#include "CM_Thread.h"
#include <unordered_map>
#include <array>
#include <string>

struct bArmature;
struct Bone;
struct bPose;
struct bConstraint;
struct Object;
class RAS_DebugDraw;

class BL_ArmatureObject : public KX_GameObject
{
	Py_Header

protected:
	/// List element: BL_ArmatureConstraint.
	EXP_ListValue<BL_ArmatureConstraint> *m_controlledConstraints;
	/// List element: BL_ArmatureChannel.
	EXP_ListValue<BL_ArmatureChannel> *m_poseChannels;
	Object *m_objArma;
	Object *m_origObjArma;
	// Need for BKE_pose_where_is.
	Scene *m_scene;
	double m_lastframe;
	size_t m_constraintNumber;
	size_t m_channelNumber;
	/// Store the original armature object matrix.
	float m_obmat[4][4];
	/// Set to true to allow draw debug info for one frame, reset in DrawDebugArmature.
    bool m_drawDebug;

    double m_lastapplyframe;

    std::unordered_map<std::string, mt::vec3> m_boneLocAdd;
    std::unordered_map<std::string, mt::vec3> m_boneEulAdd;
    std::unordered_map<std::string, mt::vec3> m_boneScaleMul;
    std::unordered_map<std::string, mt::vec4> m_boneQuatMul;


    void ApplyBoneOverrides();

public:
    std::unordered_map<std::string, std::array<float, 3>> m_basePoseHead;
    std::unordered_map<std::string, std::array<float, 3>> m_basePoseTail;
	BL_ArmatureObject(void *sgReplicationInfo,
	                  SG_Callbacks callbacks,
	                  Object *armature,
	                  Scene *scene);
	virtual ~BL_ArmatureObject();

	virtual EXP_Value *GetReplica();
	virtual void ProcessReplica();
	virtual int GetGameObjectType() const;
	virtual void ReParentLogic();
	virtual void Relink(std::map<SCA_IObject *, SCA_IObject *>& obj_map);
	virtual bool UnlinkObject(SCA_IObject *clientobj);

	double GetLastFrame();

	void GetPose(bPose **pose) const;
	/// Never edit this, only for accessing names.
	bPose *GetPose() const;
	void ApplyPose();
	void ApplyPoseLocked();
	void SetPoseByAction(bAction *action, float localtime);
	void BlendInPose(bPose *blend_pose, float weight, short mode);

	bool UpdateTimestep(double curtime);

	Object *GetArmatureObject();
	Object *GetOrigArmatureObject();
	int GetVertDeformType() const;
	bool GetDrawDebug() const;
	void DrawDebug(RAS_DebugDraw& debugDraw);

	// for constraint python API
	void LoadConstraints(BL_SceneConverter& converter);
	size_t GetConstraintNumber() const;
	BL_ArmatureConstraint *GetConstraint(const std::string& posechannel, const std::string& constraint);
	BL_ArmatureConstraint *GetConstraint(const std::string& posechannelconstraint);
	BL_ArmatureConstraint *GetConstraint(int index);

	// for pose channel python API
	void LoadChannels();
	size_t GetChannelNumber() const;
	BL_ArmatureChannel *GetChannel(bPoseChannel *channel);
	BL_ArmatureChannel *GetChannel(const std::string& channel);
	BL_ArmatureChannel *GetChannel(int index);
    bool GetBasePoseHead(const char* name, float out[3]);
    bool GetBasePoseTail(const char* name, float out[3]);

	/// Retrieve the pose transform for the specified bone.
	/// Returns true on success.
	bool GetBoneTransform(Bone *bone, mt::mat3x4& trans);

	/// Returns the bone length.  The end of the bone is in the local y direction.
    float GetBoneLength(Bone *bone) const;

    void SetBoneLocationAdd(const std::string& name, const mt::vec3& loc);
    void SetBoneEulerAdd(const std::string& name, const mt::vec3& eul);
    void SetBoneScaleMul(const std::string& name, const mt::vec3& scale);
    void SetBoneRotationMulQuat(const std::string& name, const mt::vec4& quat);
    void RemoveBoneOverride(const std::string& name);
    void ClearBoneOverrides();
	static PyObject *py_setBoneLocationAdd(PyObject *self, PyObject *args);
	static PyObject *py_setBoneEulerAdd(PyObject *self, PyObject *args);
	static PyObject *py_setBoneScaleMul(PyObject *self, PyObject *args);
	static PyObject *py_setBoneRotationMulQuat(PyObject *self, PyObject *args);
	static PyObject *py_removeBoneOverride(PyObject *self, PyObject *args);
	static PyObject *py_clearBoneOverrides(PyObject *self, PyObject *args);

#ifdef WITH_PYTHON

	// PYTHON
	static PyObject *pyattr_get_constraints(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	static PyObject *pyattr_get_channels(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef);
	EXP_PYMETHOD_DOC_NOARGS(BL_ArmatureObject, update);
	EXP_PYMETHOD_DOC_NOARGS(BL_ArmatureObject, draw);

#endif  /* WITH_PYTHON */

	static CM_ThreadMutex& GetPoseMutex();
};

#endif  /* __BL_ARMATUREOBJECT_H__ */
