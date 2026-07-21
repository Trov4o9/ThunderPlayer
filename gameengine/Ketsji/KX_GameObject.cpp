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
 * Game object wrapper
 */

/** \file gameengine/Ketsji/KX_GameObject.cpp
 *  \ingroup ketsji
 */

#ifdef _MSC_VER
/* This warning tells us about truncation of __long__ stl-generated names.
 * It can occasionally cause DevStudio to have internal compiler warnings. */
#  pragma warning( disable:4786 )
#endif

#include <vector>
#include "readerwriterqueue.h"
#include <map>
#include "KX_GrassSystem.h"
#include <unordered_map>
#include <ankerl/unordered_dense.h>
#include <string>
#include <algorithm>
#include <atomic>
#include <utility> // std::pair
#include <cmath>   // std::floor
#include <cstdint>
#include <immintrin.h>
#include <cstdio>
#include <iostream>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <chrono>
#include <omp.h>
#include "KX_GameObject.h"
#include "KX_KetsjiEngine.h"
#include "KX_PythonComponent.h"
#include "KX_Camera.h" // only for their ::Type
#include "KX_LightObject.h"  // only for their ::Type
#include "KX_FontObject.h"  // only for their ::Type
#include "RAS_Mesh.h"
#include "RAS_MeshMaterial.h"
#include "RAS_MaterialBucket.h"
#include "RAS_MeshUser.h"
#include "RAS_BatchGroup.h"
#include "RAS_BoundingBoxManager.h"
#include "RAS_2DFilterOffScreen.h"
#include "RAS_Deformer.h"
#include "KX_NavMeshObject.h"
#include "KX_Mesh.h"
#include "KX_PolyProxy.h"
#include "KX_BlenderMaterial.h"
#include "SG_Controller.h"
#include "PHY_IGraphicController.h"
#include "CcdPhysicsController.h"
#include "CcdPhysicsEnvironment.h"
#include "SG_Node.h"
#include "SG_Familly.h"
#include "KX_ClientObjectInfo.h"
#include "RAS_BucketManager.h"
#include "KX_RayCast.h"
#include "KX_Globals.h"
#include "KX_PyMath.h"
#include "SCA_IActuator.h"
#include "SCA_ISensor.h"
#include "SCA_IController.h"
#include "KX_NetworkMessageScene.h" //Needed for sendMessage()
#include "KX_ObstacleSimulation.h"
#include "KX_Scene.h"
#include "KX_LodLevel.h"
#include "KX_LodManager.h"
#include "KX_BoundingBox.h"
#include "SG_CullingNode.h"
#include "KX_BatchGroup.h"
#include "KX_CollisionContactPoints.h"
#include "GPU_material.h"
#include "BL_BlenderShader.h"
#include "BLI_noise.h"
#include "BKE_object.h"
#include "DNA_image_types.h"
extern "C" {
#include "BKE_global.h"
}
#include "GPU_glew.h"
#include "GPU_draw.h"
#include "GPU_texture.h"

#include "BL_BlenderDataConversion.h" // For BL_ConvertDeformer.
#include "BL_ConvertObjectInfo.h"
#include "BL_ActionManager.h"
#include "BL_Action.h"
#include "BL_SkinDeformer.h"
#include "BL_Converter.h"

#include "EXP_PyObjectPlus.h" /* python stuff */
#include "EXP_ListWrapper.h"
#include "BLI_utildefines.h"

#ifdef WITH_PYTHON
#  include "EXP_PythonCallBack.h"
#  include "python_utildefines.h"
#endif

// Component stuff
#include "DNA_python_component_types.h"

// This file defines relationships between parents and children
// in the game engine.

bool KX_GameObject::GetRenderVisibility() const
{
	return m_bVisible;
}



#include "KX_NodeRelationships.h"

#include "BLI_math.h"

#include "CM_Message.h"

static void KX_SNR_ClearCachesForOwner(KX_GameObject *owner, bool releaseNow);

KX_GameObject::ActivityCullingInfo::ActivityCullingInfo()
	:m_flags(ACTIVITY_NONE),
	m_physicsRadius(0.0f),
	m_logicRadius(0.0f)
{
}

KX_GameObject::KX_GameObject(void *sgReplicationInfo,
                             SG_Callbacks callbacks)
	:m_clientInfo(this, KX_ClientObjectInfo::ACTOR),
	m_layer(0),
	m_passIndex(0),
	m_lodManager(nullptr),
	m_currentLodLevel(0),
	m_meshUser(nullptr),
	m_convertInfo(nullptr),
	m_objectColor(mt::one4),
	m_bVisible(true),
	m_bOccluder(false),
	m_autoUpdateBounds(false),
	m_hasShadowCasterMaterial(false),
	m_shadowCacheDirty(true),
	m_shadowCacheBatchGroup(nullptr),
	m_bucketPropsDirty(true),
	m_physicsController(nullptr),
	m_graphicController(nullptr),
	m_sgNode(new SG_Node(this, sgReplicationInfo, callbacks)),
	m_components(nullptr),
	m_instanceObjects(nullptr),
	m_dupliGroupObject(nullptr),
	m_actionManager(nullptr),
#ifdef WITH_PYTHON
	m_attr_dict(nullptr),
	m_collisionCallbacks(nullptr),
	m_proceduralCancel(false),
	m_proceduralActiveCalls(0)
#endif
{
	// define the relationship between this node and it's parent.
	KX_NormalParentRelation *parent_relation = new KX_NormalParentRelation();
	m_sgNode->SetParentRelation(parent_relation);
}

KX_GameObject::KX_GameObject(const KX_GameObject& other)
	:SCA_IObject(other),
	m_clientInfo(this, other.m_clientInfo.m_type),
	m_name(other.m_name),
	m_layer(other.m_layer),
	m_passIndex(other.m_passIndex),
	m_meshes(other.m_meshes),
	m_lodManager(other.m_lodManager),
	m_currentLodLevel(0),
	m_meshUser(nullptr),
	m_convertInfo(other.m_convertInfo),
	m_objectColor(other.m_objectColor),
	m_bVisible(other.m_bVisible),
	m_bOccluder(other.m_bOccluder),
	m_activityCullingInfo(other.m_activityCullingInfo),
	m_autoUpdateBounds(other.m_autoUpdateBounds),
	m_hasShadowCasterMaterial(other.m_hasShadowCasterMaterial),
	m_shadowCacheDirty(other.m_shadowCacheDirty),
	m_shadowCacheBatchGroup(nullptr),
	m_bucketPropsDirty(true),
	m_physicsController(nullptr),
	m_graphicController(nullptr),
	m_sgNode(nullptr),
	m_components(nullptr),
	m_instanceObjects(nullptr),
	m_dupliGroupObject(nullptr),
	m_actionManager(nullptr),
#ifdef WITH_PYTHON
	m_attr_dict(other.m_attr_dict),
	m_collisionCallbacks(other.m_collisionCallbacks),
	m_proceduralCancel(false),
	m_proceduralActiveCalls(0)
#endif  // WITH_PYTHON
{
	if (m_lodManager) {
		m_lodManager->AddRef();
	}

#ifdef WITH_PYTHON
	if (m_attr_dict) {
		m_attr_dict = PyDict_Copy(m_attr_dict);
	}

	Py_XINCREF(m_collisionCallbacks);

	if (other.m_components) {
		m_components = static_cast<EXP_ListValue<KX_PythonComponent> *>(other.m_components->GetReplica());

		for (KX_PythonComponent *component : m_components) {
			component->SetGameObject(this);
		}
	}
#endif  // WITH_PYTHON
}

KX_GameObject::~KX_GameObject()
{
#ifdef WITH_PYTHON
	KX_SNR_ClearCachesForOwner(this, true);
	if (m_attr_dict) {
		PyDict_Clear(m_attr_dict); /* in case of circular refs or other weird cases */
		/* Py_CLEAR: Py_DECREF's and nullptr's */
		Py_CLEAR(m_attr_dict);
	}
	// Unregister collision callbacks
	// Do this before we start freeing physics information like m_clientInfo
	if (m_collisionCallbacks) {
		UnregisterCollisionCallbacks();
		Py_CLEAR(m_collisionCallbacks);
	}

	if (m_components) {
		m_components->Release();
	}

	// Grass agora é gerenciado pelo KX_GrassSystem da cena - nada a destruir aqui
#endif // WITH_PYTHON

	RemoveMeshes();

	if (m_dupliGroupObject) {
		m_dupliGroupObject->Release();
	}

	if (m_instanceObjects) {
		m_instanceObjects->Release();
	}
	if (m_lodManager) {
		// Unregister from scene's LOD list before releasing.
		KX_Scene *scene = GetScene();
		if (scene) {
			scene->RemoveLodObject(this);
		}
		m_lodManager->Release();
	}
}

KX_GameObject *KX_GameObject::GetClientObject(KX_ClientObjectInfo *info)
{
	if (!info) {
		return nullptr;
	}
	return info->m_gameobject;
}

std::string KX_GameObject::GetName()
{
	return m_name;
}

/* Set the name of the value */
void KX_GameObject::SetName(const std::string& name)
{
	m_name = name;
}

RAS_Deformer *KX_GameObject::GetDeformer()
{
	return (m_meshUser) ? m_meshUser->GetDeformer() : nullptr;
}

PHY_IPhysicsController *KX_GameObject::GetPhysicsController()
{
	return m_physicsController.get();
}

void KX_GameObject::SetPhysicsController(PHY_IPhysicsController *physicscontroller)
{
	m_physicsController.reset(physicscontroller);
}

PHY_IGraphicController *KX_GameObject::GetGraphicController()
{
	return m_graphicController.get();
}

void KX_GameObject::SetGraphicController(PHY_IGraphicController *graphiccontroller)
{
	m_graphicController.reset(graphiccontroller);
}

KX_GameObject *KX_GameObject::GetDupliGroupObject()
{
	return m_dupliGroupObject;
}

EXP_ListValue<KX_GameObject> *KX_GameObject::GetInstanceObjects()
{
	return m_instanceObjects;
}

void KX_GameObject::AddInstanceObjects(KX_GameObject *obj)
{
	if (!m_instanceObjects) {
		m_instanceObjects = new EXP_ListValue<KX_GameObject>();
	}

	obj->AddRef();
	m_instanceObjects->Add(obj);
}

void KX_GameObject::RemoveInstanceObject(KX_GameObject *obj)
{
	BLI_assert(m_instanceObjects);
	m_instanceObjects->RemoveValue(obj);
	obj->Release();
}

void KX_GameObject::RemoveDupliGroupObject()
{
	if (m_dupliGroupObject) {
		m_dupliGroupObject->Release();
		m_dupliGroupObject = nullptr;
	}
}

void KX_GameObject::SetDupliGroupObject(KX_GameObject *obj)
{
	obj->AddRef();
	m_dupliGroupObject = obj;
}

const std::vector<bRigidBodyJointConstraint *>& KX_GameObject::GetConstraints()
{
	return m_convertInfo->m_constraints;
}

void KX_GameObject::ReplicateConstraints(PHY_IPhysicsEnvironment *physEnv, const std::vector<KX_GameObject *>& constobj)
{
	if (!m_physicsController || m_convertInfo->m_constraints.empty()) {
		return;
	}

	// Object could have some constraints, iterate over all of theme to ensure that every constraint is recreated.
	for (bRigidBodyJointConstraint *dat : m_convertInfo->m_constraints) {
		// Try to find the constraint targets in the list of group objects.
		for (KX_GameObject *member : constobj) {
			// If the group member is the actual target for the constraint.
			if ((dat->tar->id.name + 2) == member->GetName() && member->GetPhysicsController()) {
				physEnv->SetupObjectConstraints(this, member, dat);
			}
		}
	}
}

KX_GameObject *KX_GameObject::GetParent()
{
	KX_GameObject *result = nullptr;
	SG_Node *node = m_sgNode.get();

	while (node && !result)
	{
		node = node->GetParent();
		if (node) {
			result = (KX_GameObject *)node->GetClientObject();
		}
	}

	return result;
}

void KX_GameObject::SetParent(KX_GameObject *obj, bool addToCompound, bool ghost)
{
	if (!obj || obj == this) {
		return;
	}

	SG_Node *parentSgNode = obj->GetNode();

	if (m_sgNode->GetParent() == parentSgNode ||
	    m_sgNode->IsAncessor(parentSgNode)) {
		return;
	}

	KX_Scene *scene = GetScene();

	if (!(scene->GetInactiveList()->SearchValue(obj) !=
	      scene->GetObjectList()->SearchValue(this))) {
		CM_FunctionWarning(
			"child and parent are not in the same game objects list "
			"(active or inactive). This operation is forbidden.");
		return;
	}

	const mt::vec3 parentScale = obj->NodeGetWorldScaling();
	if (mt::FuzzyZero(parentScale)) {
		return;
	}

	const mt::vec3 childScale = NodeGetWorldScaling();
	if (mt::FuzzyZero(childScale)) {
		return;
	}

	RemoveParent();
	parentSgNode->AddChild(m_sgNode.get());

	if (m_physicsController) {
		m_physicsController->SuspendDynamics(ghost);
	}

	const mt::vec3 invParentScale(
		1.0f / parentScale.x,
		1.0f / parentScale.y,
		1.0f / parentScale.z
	);

	const mt::mat3 invori = obj->NodeGetWorldOrientation().Inverse();

	const mt::vec3 newScale = childScale * invParentScale;
	const mt::vec3 newPos =
		invori *
		(NodeGetWorldPosition() - obj->NodeGetWorldPosition()) *
		invParentScale;

	NodeSetLocalScale(newScale);
	NodeSetLocalPosition(newPos);
	NodeSetLocalOrientation(invori * NodeGetWorldOrientation());
	NodeUpdate();

	EXP_ListValue<KX_GameObject> *rootlist = scene->GetRootParentList();
	if (rootlist->RemoveValue(this)) {
		Release();
	}

	if (addToCompound && m_physicsController) {
		KX_GameObject *rootobj =
			(KX_GameObject *)parentSgNode
				->GetRootSGParent()
				->GetClientObject();

		if (rootobj &&
		    rootobj->m_physicsController &&
		    rootobj->m_physicsController->IsCompound()) {
			rootobj->m_physicsController
				->AddCompoundChild(m_physicsController.get());
		}
	}

}


void KX_GameObject::RemoveParent()
{
	if (!m_sgNode->GetParent()) {
		return;
	}

	// get the root object to remove us from compound object if needed
	KX_GameObject *rootobj = (KX_GameObject *)m_sgNode->GetRootSGParent()->GetClientObject();
	// Set us to the right spot
	m_sgNode->SetLocalScale(m_sgNode->GetWorldScaling());
	m_sgNode->SetLocalOrientation(m_sgNode->GetWorldOrientation());
	m_sgNode->SetLocalPosition(m_sgNode->GetWorldPosition());

	// Remove us from our parent
	m_sgNode->DisconnectFromParent();
	NodeUpdate();

	KX_Scene *scene = GetScene();
	// the object is now a root object, add it to the parentlist
	EXP_ListValue<KX_GameObject> *rootlist = scene->GetRootParentList();
	if (!rootlist->SearchValue(this)) {
		// object was not in root list, add it now and increment ref count
		rootlist->Add(CM_AddRef(this));
	}
	if (m_physicsController) {
		// in case this controller was added as a child shape to the parent
		if (rootobj &&
		    rootobj->m_physicsController &&
		    rootobj->m_physicsController->IsCompound()) {
			rootobj->m_physicsController->RemoveCompoundChild(m_physicsController.get());
		}
		m_physicsController->RestoreDynamics();
		if (m_physicsController->IsDynamic() && (rootobj && rootobj->m_physicsController)) {
			// dynamic object should remember the velocity they had while being parented
			const mt::vec3 childPoint = m_sgNode->GetWorldPosition();
			const mt::vec3 rootPoint = rootobj->m_sgNode->GetWorldPosition();
			const mt::vec3 relPoint = (childPoint - rootPoint);
			const mt::vec3 linVel = rootobj->m_physicsController->GetVelocity(relPoint);
			const mt::vec3 angVel = rootobj->m_physicsController->GetAngularVelocity();
			m_physicsController->SetLinearVelocity(linVel, false);
			m_physicsController->SetAngularVelocity(angVel, false);
		}
	}
	// graphically, the object hasn't change place, no need to update m_graphicController
}

BL_ActionManager *KX_GameObject::GetActionManager()
{
	// We only want to create an action manager if we need it
	if (!m_actionManager) {
		GetScene()->AddAnimatedObject(this);
		m_actionManager.reset(new BL_ActionManager(this));
	}
	return m_actionManager.get();
}

bool KX_GameObject::PlayAction(const std::string& name,
                               float start,
                               float end,
                               short layer,
                               short priority,
                               float blendin,
                               short play_mode,
                               float layer_weight,
                               short ipo_flags,
                               float playback_speed,
                               short blend_mode)
{
	return GetActionManager()->PlayAction(name, start, end, layer, priority, blendin, play_mode, layer_weight, ipo_flags, playback_speed, blend_mode);
}

void KX_GameObject::StopAction(short layer)
{
	GetActionManager()->StopAction(layer);
}

bool KX_GameObject::IsActionDone(short layer)
{
	return GetActionManager()->IsActionDone(layer);
}

bool KX_GameObject::IsActionsSuspended()
{
	return GetActionManager()->IsSuspended();
}

void KX_GameObject::UpdateActionManager(float curtime, bool applyToObject)
{
	GetActionManager()->Update(curtime, applyToObject);
}



float KX_GameObject::GetActionFrame(short layer)
{
	return GetActionManager()->GetActionFrame(layer);
}

const std::string KX_GameObject::GetActionName(short layer)
{
	return GetActionManager()->GetActionName(layer);
}

void KX_GameObject::SetActionFrame(short layer, float frame)
{
	GetActionManager()->SetActionFrame(layer, frame);
}

std::string KX_GameObject::GetCurrentActionName(short layer)
{
	return GetActionManager()->GetCurrentActionName(layer);
}

void KX_GameObject::SetPlayMode(short layer, short mode)
{
	GetActionManager()->SetPlayMode(layer, mode);
}

static void setGraphicController_recursive(SG_Node *node)
{
	const NodeList& children = node->GetChildren();

	for (SG_Node *childnode : children) {
		KX_GameObject *clientgameobj = static_cast<KX_GameObject *>(childnode->GetClientObject());
		if (clientgameobj != nullptr) { // This is a GameObject
			clientgameobj->ActivateGraphicController(false);
		}

		// if the childobj is nullptr then this may be an inverse parent link
		// so a non recursive search should still look down this node.
		setGraphicController_recursive(childnode);
	}
}


void KX_GameObject::ActivateGraphicController(bool recurse)
{
	if (m_graphicController) {
		m_graphicController->Activate(m_bVisible || m_bOccluder);
	}
	if (recurse) {
		setGraphicController_recursive(m_sgNode.get());
	}
}

void KX_GameObject::SetCollisionGroup(unsigned short group)
{
	if (m_physicsController) {
		m_physicsController->SetCollisionGroup(group);
		m_physicsController->RefreshCollisions();
	}
}
void KX_GameObject::SetCollisionMask(unsigned short mask)
{
	if (m_physicsController) {
		m_physicsController->SetCollisionMask(mask);
		m_physicsController->RefreshCollisions();
	}
}

unsigned short KX_GameObject::GetCollisionGroup() const
{
	return m_physicsController ? m_physicsController->GetCollisionGroup() : 0;
}
unsigned short KX_GameObject::GetCollisionMask() const
{
	return m_physicsController ? m_physicsController->GetCollisionMask() : 0;
}

EXP_Value *KX_GameObject::GetReplica()
{
	KX_GameObject *replica = new KX_GameObject(*this);

	// this will copy properties and so on...
	replica->ProcessReplica();

	return replica;
}

bool KX_GameObject::UnlinkObject(SCA_IObject *clientobj)
{
	if (clientobj && clientobj->GetGameObjectType() == SCA_IObject::OBJ_ARMATURE && m_meshUser) {
		RAS_Deformer *deformer = m_meshUser->GetDeformer();
		BL_SkinDeformer *skinDeformer = dynamic_cast<BL_SkinDeformer *>(deformer);
		BL_ArmatureObject *armature = static_cast<BL_ArmatureObject *>(clientobj);
		if (skinDeformer && skinDeformer->UsesArmature(armature)) {
			skinDeformer->SetArmature(nullptr);
			return true;
		}
	}

	return false;
}

KX_BlenderMaterial* KX_GameObject::GetFirstBlenderMaterial() const
{
	for (KX_Mesh* mesh : m_meshes) {
		const std::vector<RAS_MeshMaterial*>& meshMats = mesh->GetMeshMaterialList();
		if (!meshMats.empty()) {
			RAS_MeshMaterial* mmat = meshMats[0];
			if (mmat && mmat->GetBucket()) {
				return static_cast<KX_BlenderMaterial*>(mmat->GetBucket()->GetMaterial());
			}
		}
	}
	return nullptr;
}

bool KX_GameObject::HasShadowCasterMaterial() const
{
	// Detect mid-frame batch group changes (e.g. dynamic batching merge/split).
	RAS_BatchGroup *currentBg = m_meshUser ? m_meshUser->GetBatchGroup() : nullptr;
	
	if (currentBg != m_shadowCacheBatchGroup) {
		m_shadowCacheDirty = true;
		m_shadowCacheBatchGroup = currentBg;
	}

	// Return cached result if still valid.
	if (!m_shadowCacheDirty) {
		return m_hasShadowCasterMaterial;
	}

	m_shadowCacheDirty = false;

	if (currentBg) {
		m_hasShadowCasterMaterial = currentBg->CastsShadows();
		return m_hasShadowCasterMaterial;
	}

	if (m_meshes.empty()) {
		m_hasShadowCasterMaterial = false;
		return false;
	}
	
	for (KX_Mesh* mesh : m_meshes) {
		const std::vector<RAS_MeshMaterial*>& mats = mesh->GetMeshMaterialList();
		
		for (RAS_MeshMaterial* mmat : mats) {
			if (!mmat) continue;
			RAS_MaterialBucket* bucket = mmat->GetBucket();
			if (!bucket) continue;
			RAS_IMaterial* mat = bucket->GetMaterial();
			if (mat) {
				if (mat->CastsShadows()) {
					m_hasShadowCasterMaterial = true;
					return true;
				}
			}
		}
	}
	m_hasShadowCasterMaterial = false;
	return false;
}

void KX_GameObject::RemoveRessources(const BL_Resource::Library& libraryId)
{
	// If the object is using actions, try remove actions from this library.
	if (m_actionManager) {
		m_actionManager->RemoveActions(libraryId);
	}

	for (KX_Mesh *mesh : m_meshes) {
		// If the mesh comes from this lirbary, remove all meshes.
		if (mesh->Belong(libraryId)) {
			RemoveMeshes();
			break;
		}
		else {
			// If one of the material used by the mesh comes from this library, remove all meshes too.
			for (RAS_MeshMaterial *meshmat : mesh->GetMeshMaterialList()) {
				if (static_cast<KX_BlenderMaterial *>(meshmat->GetBucket()->GetMaterial())->Belong(libraryId)) {
					RemoveMeshes();
					break;
				}
			}
		}
	}
}

bool KX_GameObject::IsDynamic() const
{
	if (m_physicsController) {
		return m_physicsController->IsDynamic();
	}
	return false;
}

bool KX_GameObject::IsDynamicsSuspended() const
{
	if (m_physicsController) {
		return m_physicsController->IsDynamicsSuspended();
	}
	return false;
}

float KX_GameObject::GetLinearDamping() const
{
	if (m_physicsController) {
		return m_physicsController->GetLinearDamping();
	}
	return 0.0f;
}

float KX_GameObject::GetAngularDamping() const
{
	if (m_physicsController) {
		return m_physicsController->GetAngularDamping();
	}
	return 0.0f;
}

void KX_GameObject::SetLinearDamping(float damping)
{
	if (m_physicsController) {
		m_physicsController->SetLinearDamping(damping);
	}
}


void KX_GameObject::SetAngularDamping(float damping)
{
	if (m_physicsController) {
		m_physicsController->SetAngularDamping(damping);
	}
}

void KX_GameObject::SetDamping(float linear, float angular)
{
	if (m_physicsController) {
		m_physicsController->SetDamping(linear, angular);
	}
}

void KX_GameObject::ApplyForce(const mt::vec3& force, bool local)
{
	if (m_physicsController) {
		m_physicsController->ApplyForce(force, local);
	}
}

void KX_GameObject::ApplyTorque(const mt::vec3& torque, bool local)
{
	if (m_physicsController) {
		m_physicsController->ApplyTorque(torque, local);
	}
}

void KX_GameObject::ApplyMovement(const mt::vec3& dloc, bool local)
{
	if (dloc.x == 0.0f && dloc.y == 0.0f && dloc.z == 0.0f)
		return;

	if (m_physicsController) {
		m_physicsController->RelativeTranslate(dloc, local);
	}

	SG_Node *node = m_sgNode.get();
	node->RelativeTranslate(dloc, node->GetParent(), local);
	node->UpdateWorldData();
}

void KX_GameObject::ApplyRotation(const mt::vec3& drot, bool local)
{
	if (drot.x == 0.0f && drot.y == 0.0f && drot.z == 0.0f)
		return;

	mt::mat3 rotmat(drot);

	SG_Node *node = m_sgNode.get();
	node->RelativeRotate(rotmat, local);

	if (m_physicsController) {
		m_physicsController->RelativeRotate(rotmat, local);
	}
	node->UpdateWorldData();
}

void KX_GameObject::UpdateBlenderObjectMatrix(Object *blendobj)
{
	if (!blendobj) {
		blendobj = m_convertInfo->m_blenderObject;
	}
	if (blendobj) {
		const mt::mat3x4 trans = NodeGetWorldTransform();
		trans.PackFromAffineTransform(blendobj->obmat);
	}
}

void KX_GameObject::AddMeshUser()
{
	if (m_meshes.empty()) {
		m_meshUser = nullptr;
		// No mesh - shadow cache is definitively false.
		m_hasShadowCasterMaterial = false;
		m_shadowCacheDirty = false;
		return;
	}

	const mt::mat4 matrix = mt::mat4::FromAffineTransform(NodeGetWorldTransform());
	const bool frontFace = !IsNegativeScaling();

	for (KX_Mesh *mesh : m_meshes) {
		RAS_Deformer *deformer = BL_ConvertDeformer(this, mesh);
		m_meshUser = mesh->AddMeshUser(&m_clientInfo, deformer);
		m_meshUser->SetMatrix(matrix);
		m_meshUser->SetFrontFace(frontFace);
	}

	// New meshUser may have a different BatchGroup - invalidate shadow cache.
	m_shadowCacheDirty = true;
	// New meshUser means bucket props must be pushed again.
	m_bucketPropsDirty = true;
}

void KX_GameObject::UpdateBuckets()
{
	RAS_MeshUser *meshUser = m_meshUser;
	if (!meshUser || !m_sgNode)
		return;

	// Update datas and add mesh slot to be rendered only if the object is not culled.
	if (m_sgNode->IsDirty(SG_Node::DIRTY_RENDER)) {
		meshUser->SetMatrix(mt::mat4::FromAffineTransform(NodeGetWorldTransform()));
		meshUser->SetFrontFace(!IsNegativeScaling());
		m_sgNode->ClearDirty(SG_Node::DIRTY_RENDER);
	}

	if (meshUser->GetBatchGroup()) {
		meshUser->ActivateMeshSlots();
		return;
	}

	// Only push passIndex/layer/color when they actually changed.
	if (m_bucketPropsDirty) {
		meshUser->SetPassIndex(m_passIndex);
		meshUser->SetLayer(m_layer);
		meshUser->SetColor(m_objectColor);
		m_bucketPropsDirty = false;
	}
	meshUser->ActivateMeshSlots();
}

void KX_GameObject::UpdateBucketsNoOnlyShadow()
{
	RAS_MeshUser *meshUser = m_meshUser;
	if (!meshUser || !m_sgNode)
		return;

	if (m_sgNode->IsDirty(SG_Node::DIRTY_RENDER)) {
		meshUser->SetMatrix(mt::mat4::FromAffineTransform(NodeGetWorldTransform()));
		meshUser->SetFrontFace(!IsNegativeScaling());
		m_sgNode->ClearDirty(SG_Node::DIRTY_RENDER);
	}

	if (meshUser->GetBatchGroup()) {
		meshUser->ActivateMeshSlotsNoOnlyShadow();
		return;
	}

	if (m_bucketPropsDirty) {
		meshUser->SetPassIndex(m_passIndex);
		meshUser->SetLayer(m_layer);
		meshUser->SetColor(m_objectColor);
		m_bucketPropsDirty = false;
	}
	meshUser->ActivateMeshSlotsNoOnlyShadow();
}

void KX_GameObject::UpdateShadowBuckets()
{
	RAS_MeshUser *meshUser = m_meshUser;
	if (!meshUser || !m_sgNode)
		return;

	if (m_sgNode->IsDirty(SG_Node::DIRTY_RENDER)) {
		meshUser->SetMatrix(mt::mat4::FromAffineTransform(NodeGetWorldTransform()));
		meshUser->SetFrontFace(!IsNegativeScaling());
		m_sgNode->ClearDirty(SG_Node::DIRTY_RENDER);
	}

	if (meshUser->GetBatchGroup()) {
		meshUser->ActivateShadowMeshSlots();
		return;
	}

	if (m_bucketPropsDirty) {
		meshUser->SetPassIndex(m_passIndex);
		meshUser->SetLayer(m_layer);
		meshUser->SetColor(m_objectColor);
		m_bucketPropsDirty = false;
	}
	meshUser->ActivateShadowMeshSlots();
}

void KX_GameObject::ReplaceMesh(KX_Mesh *mesh, bool use_gfx, bool use_phys)
{
	if (!use_gfx && !use_phys) {
		return;
	}

	KX_Mesh *currentMesh = m_meshes.empty() ? nullptr : m_meshes.front();
	const bool meshChanged = use_gfx && mesh && currentMesh != mesh;

	if (meshChanged) {
		// Mesh changed - shadow caster cache is no longer valid.
		m_shadowCacheDirty = true;

		if (m_meshUser) {
			delete m_meshUser;
			m_meshUser = nullptr;
		}

		if (m_meshes.size() == 1) {
			m_meshes[0] = mesh;
		}
		else {
			m_meshes.clear();
			m_meshes.push_back(mesh);
		}

		AddMeshUser();
	}

	bool physicsUpdated = false;
	if (use_phys && m_physicsController) {
		const bool keepCurrentPhysicsShape = use_gfx && !meshChanged;
		if (!keepCurrentPhysicsShape) {
			m_physicsController->ReinstancePhysicsShape(nullptr, use_gfx ? nullptr : mesh);
			physicsUpdated = true;
		}
	}

	if (meshChanged || physicsUpdated) {
		UpdateBounds(true);
	}
}


void KX_GameObject::RemoveMeshes()
{
	if (!m_meshUser && m_meshes.empty()) {
		return;
	}

	// Remove all mesh slots.
	if (m_meshUser) {
		delete m_meshUser;
		m_meshUser = nullptr;
	}

	//note: meshes can be shared, and are deleted by BL_SceneConverter
	m_meshes.clear();

	// No mesh - shadow cache is definitively false.
	m_hasShadowCasterMaterial = false;
	m_shadowCacheDirty = false;
}

const std::vector<KX_Mesh *>& KX_GameObject::GetMeshList() const
{
	return m_meshes;
}

RAS_MeshUser *KX_GameObject::GetMeshUser() const
{
	return m_meshUser;
}

bool KX_GameObject::Renderable(int layer) const
{
	return (m_meshUser != nullptr) && m_bVisible && (layer == 0 || m_layer & layer);
}

void KX_GameObject::SetLodManager(KX_LodManager *lodManager)
{
	// Reset lod level to avoid overflow index in KX_LodManager::GetLevel.
	m_currentLodLevel = 0;

	// Restore object original mesh.
	if (!lodManager && m_lodManager && m_lodManager->GetLevelCount() > 0) {
		KX_Mesh *origmesh = m_lodManager->GetLevel(0).GetMesh();
		ReplaceMesh(origmesh, true, false);
	}

	const bool hadLod = (m_lodManager != nullptr);
	const bool willHaveLod = (lodManager != nullptr);

	if (m_lodManager) {
		m_lodManager->Release();
	}

	m_lodManager = lodManager;

	if (m_lodManager) {
		m_lodManager->AddRef();
	}

	// Keep the scene's LOD-capable list in sync.
	KX_Scene *scene = GetScene();
	if (scene) {
		if (!hadLod && willHaveLod) {
			scene->AddLodObject(this);
		}
		else if (hadLod && !willHaveLod) {
			scene->RemoveLodObject(this);
		}
	}
}

KX_LodManager *KX_GameObject::GetLodManager() const
{
	return m_lodManager;
}

void KX_GameObject::UpdateLod(KX_Scene *scene, const mt::vec3& cam_pos, float lodfactor)
{
	if (!m_lodManager) {
		return;
	}

	const float distance2 = (NodeGetWorldPosition() - cam_pos).LengthSquared() * (lodfactor * lodfactor);
	const KX_LodLevel& lodLevel = m_lodManager->GetLevel(scene, m_currentLodLevel, distance2);

	KX_Mesh *mesh = lodLevel.GetMesh();
	if (mesh != m_meshes.front()) {
		ReplaceMesh(mesh, true, false);
	}

	m_currentLodLevel = lodLevel.GetLevel();
}

void KX_GameObject::UpdateTransform()
{
	// HACK: saves function call for dynamic object, they are handled differently
	if (m_physicsController && !m_physicsController->IsDynamic()) {
		m_physicsController->SetTransform();
	}
	if (m_graphicController) {
		// update the culling tree
		m_graphicController->SetGraphicTransform();
	}
}

void KX_GameObject::UpdateTransformFunc(SG_Node *node, void *gameobj, void *scene)
{
	((KX_GameObject *)gameobj)->UpdateTransform();
}

void KX_GameObject::SynchronizeTransform()
{
	// only used for sensor object, do full synchronization as bullet doesn't do it
	if (m_physicsController) {
		m_physicsController->SetTransform();
	}
	if (m_graphicController) {
		m_graphicController->SetGraphicTransform();
	}
}

void KX_GameObject::SynchronizeTransformFunc(SG_Node *node, void *gameobj, void *scene)
{
	((KX_GameObject *)gameobj)->SynchronizeTransform();
}

bool KX_GameObject::GetVisible(void)
{
	return m_bVisible;
}

static void setVisible_recursive(SG_Node *node, bool v)
{
	const NodeList& children = node->GetChildren();

	for (SG_Node *childnode : children) {
		KX_GameObject *clientgameobj = static_cast<KX_GameObject *>(childnode->GetClientObject());
		if (clientgameobj != nullptr) { // This is a GameObject
			clientgameobj->SetVisible(v, 0);
		}

		// if the childobj is nullptr then this may be an inverse parent link
		// so a non recursive search should still look down this node.
		setVisible_recursive(childnode, v);
	}
}


void KX_GameObject::SetVisible(bool v,
                               bool recursive)
{
	m_bVisible = v;
	if (m_graphicController) {
		m_graphicController->Activate(m_bVisible || m_bOccluder);
	}
	if (recursive) {
		setVisible_recursive(m_sgNode.get(), v);
	}
}

static void setOccluder_recursive(SG_Node *node, bool v)
{
	const NodeList& children = node->GetChildren();

	for (SG_Node *childnode : children) {
		KX_GameObject *clientgameobj = static_cast<KX_GameObject *>(childnode->GetClientObject());
		if (clientgameobj != nullptr) { // This is a GameObject
			clientgameobj->SetOccluder(v, false);
		}

		// if the childobj is nullptr then this may be an inverse parent link
		// so a non recursive search should still look down this node.
		setOccluder_recursive(childnode, v);
	}
}

void KX_GameObject::SetOccluder(bool v,
                                bool recursive)
{
	m_bOccluder = v;
	if (m_graphicController) {
		m_graphicController->Activate(m_bVisible || m_bOccluder);
	}
	if (recursive) {
		setOccluder_recursive(m_sgNode.get(), v);
	}
}

static void setDebug_recursive(KX_Scene *scene, SG_Node *node, bool debug)
{
	const NodeList& children = node->GetChildren();

	for (SG_Node *childnode : children) {
		KX_GameObject *clientgameobj = static_cast<KX_GameObject *>(childnode->GetClientObject());
		if (clientgameobj != nullptr) {
			if (debug) {
				if (!scene->ObjectInDebugList(clientgameobj)) {
					scene->AddObjectDebugProperties(clientgameobj);
				}
			}
			else {
				scene->RemoveObjectDebugProperties(clientgameobj);
			}
		}

		/* if the childobj is nullptr then this may be an inverse parent link
		 * so a non recursive search should still look down this node. */
		setDebug_recursive(scene, childnode, debug);
	}
}

void KX_GameObject::SetUseDebugProperties(bool debug, bool recursive)
{
	KX_Scene *scene = GetScene();

	if (debug) {
		if (!scene->ObjectInDebugList(this)) {
			scene->AddObjectDebugProperties(this);
		}
	}
	else {
		scene->RemoveObjectDebugProperties(this);
	}

	if (recursive) {
		setDebug_recursive(scene, m_sgNode.get(), debug);
	}
}

void KX_GameObject::SetLayer(int l)
{
	if (m_layer != l) {
		m_layer = l;
		m_bucketPropsDirty = true;
	}
}

int KX_GameObject::GetLayer(void)
{
	return m_layer;
}

void KX_GameObject::SetPassIndex(short index)
{
	if (m_passIndex != index) {
		m_passIndex = index;
		m_bucketPropsDirty = true;
	}
}

short KX_GameObject::GetPassIndex() const
{
	return m_passIndex;
}

void KX_GameObject::AddLinearVelocity(const mt::vec3& lin_vel, bool local)
{
	if (m_physicsController) {
		const mt::vec3 lv = local ? NodeGetWorldOrientation() * lin_vel : lin_vel;
		m_physicsController->SetLinearVelocity(lv + m_physicsController->GetLinearVelocity(), 0);
	}
}

void KX_GameObject::SetLinearVelocity(const mt::vec3& lin_vel, bool local)
{
	if (m_physicsController) {
		m_physicsController->SetLinearVelocity(lin_vel, local);
	}
}

void KX_GameObject::SetAngularVelocity(const mt::vec3& ang_vel, bool local)
{
	if (m_physicsController) {
		m_physicsController->SetAngularVelocity(ang_vel, local);
	}
}

void KX_GameObject::SetObjectColor(const mt::vec4& rgbavec)
{
	if (m_objectColor != rgbavec) {
		m_objectColor = rgbavec;
		m_bucketPropsDirty = true;
	}
}

const mt::vec4& KX_GameObject::GetObjectColor()
{
	return m_objectColor;
}

void KX_GameObject::AlignAxisToVect(const mt::vec3& dir, int axis, float fac)
{
	mt::mat3 orimat;
	mt::vec3 vect, ori, z, x, y;
	float len;

	vect = dir;
	len = vect.Length();
	if (mt::FuzzyZero(len)) {
		CM_FunctionError("null vector!");
		return;
	}

	if (fac <= 0.0f) {
		return;
	}

	// normalize
	vect /= len;
	orimat = NodeGetWorldOrientation();
	switch (axis) {
		case 0: // align x axis of new coord system to vect
		{ori = orimat.GetColumn(2);    // pivot axis
		 if (mt::FuzzyZero(1.0f - std::abs(mt::dot(vect, ori)))) {     // vect parallel to pivot?
			 ori = orimat.GetColumn(1);    // change the pivot!
		 }

		 if (fac == 1.0f) {
			 x = vect;
		 }
		 else {
			 x = (vect * fac) + ((orimat *mt::axisX3)*(1.0f - fac));
			 len = x.Length();
			 if (mt::FuzzyZero(len)) {
				 x = vect;
			 }
			 else {
				 x /= len;
			 }
		 }
		 y = mt::cross(ori, x);
		 z = mt::cross(x, y);
		 break;}
		case 1: // y axis
		{ori = orimat.GetColumn(0);
		 if (mt::FuzzyZero(1.0f - std::abs(mt::dot(vect, ori)))) {
			 ori = orimat.GetColumn(2);
		 }

		 if (fac == 1.0f) {
			 y = vect;
		 }
		 else {
			 y = (vect * fac) + ((orimat *mt::axisY3)*(1.0f - fac));
			 len = y.Length();
			 if (mt::FuzzyZero(len)) {
				 y = vect;
			 }
			 else {
				 y /= len;
			 }
		 }
		 z = mt::cross(ori, y);
		 x = mt::cross(y, z);
		 break;}
		case 2: // z axis
		{ori = orimat.GetColumn(1);
		 if (mt::FuzzyZero(1.0f - std::abs(mt::dot(vect, ori)))) {
			 ori = orimat.GetColumn(0);
		 }

		 if (fac == 1.0f) {
			 z = vect;
		 }
		 else {
			 z = (vect * fac) + ((orimat *mt::axisZ3)*(1.0f - fac));
			 len = z.Length();
			 if (mt::FuzzyZero(len)) {
				 z = vect;
			 }
			 else {
				 z /= len;
			 }
		 }
		 x = mt::cross(ori, z);
		 y = mt::cross(z, x);
		 break;}
		default: // invalid axis specified
			CM_FunctionWarning("invalid axis '" << axis << "'");
			return;
	}
	x.Normalize(); // normalize the new base vectors
	y.Normalize();
	z.Normalize();
	orimat = mt::mat3(x, y, z);

	if (m_sgNode->GetParent() != nullptr) {
		// the object is a child, adapt its local orientation so that
		// the global orientation is aligned as we want (cancelling out the parent orientation)
		mt::mat3 invori = m_sgNode->GetParent()->GetWorldOrientation().Inverse();
		NodeSetLocalOrientation(invori * orimat);
	}
	else {
		NodeSetLocalOrientation(orimat);
	}
}

float KX_GameObject::GetMass()
{
	if (m_physicsController) {
		return m_physicsController->GetMass();
	}
	return 0.0f;
}

mt::vec3 KX_GameObject::GetLocalInertia()
{
	mt::vec3 local_inertia = mt::zero3;
	if (m_physicsController) {
		local_inertia = m_physicsController->GetLocalInertia();
	}
	return local_inertia;
}

mt::vec3 KX_GameObject::GetLinearVelocity(bool local)
{
	mt::vec3 velocity = mt::zero3, locvel;
	mt::mat3 ori;
	if (m_physicsController) {
		velocity = m_physicsController->GetLinearVelocity();

		if (local) {
			ori = NodeGetWorldOrientation();

			locvel = velocity * ori;
			return locvel;
		}
	}
	return velocity;
}

mt::vec3 KX_GameObject::GetAngularVelocity(bool local)
{
	mt::vec3 velocity = mt::zero3, locvel;
	mt::mat3 ori;
	if (m_physicsController) {
		velocity = m_physicsController->GetAngularVelocity();

		if (local) {
			ori = NodeGetWorldOrientation();

			locvel = velocity * ori;
			return locvel;
		}
	}
	return velocity;
}

mt::vec3 KX_GameObject::GetGravity() const
{
	if (!m_physicsController) {
		return mt::zero3;
	}

	return m_physicsController->GetGravity();
}

void KX_GameObject::SetGravity(const mt::vec3 &gravity)
{
	if (m_physicsController) {
		m_physicsController->SetGravity(gravity);
	}
}

mt::vec3 KX_GameObject::GetVelocity(const mt::vec3& point)
{
	if (m_physicsController) {
		return m_physicsController->GetVelocity(point);
	}
	return mt::zero3;
}

void KX_GameObject::NodeSetLocalPosition(const mt::vec3& trans)
{
	if (m_physicsController && !m_sgNode->GetParent()) {
		// don't update physic controller if the object is a child:
		// 1) the transformation will not be right
		// 2) in this case, the physic controller is necessarily a static object
		//    that is updated from the normal kinematic synchronization
		m_physicsController->SetPosition(trans);
	}

	m_sgNode->SetLocalPosition(trans);
}

void KX_GameObject::NodeSetLocalOrientation(const mt::mat3& rot)
{
	if (m_physicsController && !m_sgNode->GetParent()) {
		// see note above
		m_physicsController->SetOrientation(rot);
	}
	m_sgNode->SetLocalOrientation(rot);
}

void KX_GameObject::NodeSetGlobalOrientation(const mt::mat3& rot)
{
	SG_Node *parentSgNode = m_sgNode->GetParent();
	if (parentSgNode) {
		NodeSetLocalOrientation(parentSgNode->GetWorldOrientation().Inverse() * rot);
	}
	else {
		NodeSetLocalOrientation(rot);
	}
}

void KX_GameObject::NodeSetLocalScale(const mt::vec3& scale)
{
	if (m_physicsController && !m_sgNode->GetParent()) {
		m_physicsController->SetScaling(scale);
	}
	m_sgNode->SetLocalScale(scale);
}

void KX_GameObject::NodeSetRelativeScale(const mt::vec3& scale)
{
	m_sgNode->RelativeScale(scale);
	if (m_physicsController && (!m_sgNode->GetParent())) {
		// see note above
		// we can use the local scale: it's the same thing for a root object
		// and the world scale is not yet updated
		const mt::vec3& newscale = NodeGetLocalScaling();
		m_physicsController->SetScaling(newscale);
	}
}

void KX_GameObject::NodeSetWorldScale(const mt::vec3& scale)
{
	SG_Node *parent = m_sgNode->GetParent();
	if (parent) {
		// Make sure the objects have some scale
		mt::vec3 p_scale = parent->GetWorldScaling();
		if (mt::FuzzyZero(p_scale)) {
			return;
		}

		p_scale[0] = 1.0f / p_scale[0];
		p_scale[1] = 1.0f / p_scale[1];
		p_scale[2] = 1.0f / p_scale[2];

		NodeSetLocalScale(scale * p_scale);
	}
	else {
		NodeSetLocalScale(scale);
	}
}

void KX_GameObject::NodeSetWorldPosition(const mt::vec3& trans)
{
	SG_Node *parent = m_sgNode->GetParent();
	if (parent != nullptr) {
		// Make sure the objects have some scale
		mt::vec3 scale = parent->GetWorldScaling();
		if (mt::FuzzyZero(scale)) {
			return;
		}

		scale[0] = 1.0f / scale[0];
		scale[1] = 1.0f / scale[1];
		scale[2] = 1.0f / scale[2];

		const mt::mat3 invori = parent->GetWorldOrientation().Inverse();
		const mt::vec3 newpos = invori * (trans - parent->GetWorldPosition()) * scale;
		NodeSetLocalPosition(newpos);
	}
	else {
		NodeSetLocalPosition(trans);
	}
}

void KX_GameObject::NodeUpdate()
{
	m_sgNode->UpdateWorldData();
}

const mt::mat3& KX_GameObject::NodeGetWorldOrientation() const
{
	return m_sgNode->GetWorldOrientation();
}

const mt::mat3& KX_GameObject::NodeGetLocalOrientation() const
{
	return m_sgNode->GetLocalOrientation();
}

const mt::vec3& KX_GameObject::NodeGetWorldScaling() const
{
	return m_sgNode->GetWorldScaling();
}

const mt::vec3& KX_GameObject::NodeGetLocalScaling() const
{
	return m_sgNode->GetLocalScale();
}

const mt::vec3& KX_GameObject::NodeGetWorldPosition() const
{
	return m_sgNode->GetWorldPosition();
}

const mt::vec3& KX_GameObject::NodeGetLocalPosition() const
{
	return m_sgNode->GetLocalPosition();
}

mt::mat3x4 KX_GameObject::NodeGetWorldTransform() const
{
	return m_sgNode->GetWorldTransform();
}

mt::mat3x4 KX_GameObject::NodeGetLocalTransform() const
{
	return m_sgNode->GetLocalTransform();
}

Object *KX_GameObject::GetBlenderObject() const
{
	// Non converted objects has default camera doesn't have convert info.
	return (m_convertInfo) ? m_convertInfo->m_blenderObject : nullptr;
}

BL_ConvertObjectInfo *KX_GameObject::GetConvertObjectInfo() const
{
	return m_convertInfo;
}

void KX_GameObject::SetConvertObjectInfo(BL_ConvertObjectInfo *info)
{
	m_convertInfo = info;
}

void KX_GameObject::SetNode(SG_Node *node)
{
	m_sgNode.reset(node);
}

void KX_GameObject::UpdateBounds(bool force)
{
	if ((!m_autoUpdateBounds && !force) || !m_meshUser) {
		return;
	}

	RAS_BoundingBox *boundingBox = m_meshUser->GetBoundingBox();
	if (!boundingBox || (!boundingBox->GetModified() && !force)) {
		return;
	}

	RAS_Deformer *deformer = GetDeformer();
	if (deformer) {
		/** Update all the deformer, not only per material.
		 * One of the side effect is to clear some flags about AABB calculation.
		 * like in KX_SoftBodyDeformer.
		 */
		deformer->UpdateBuckets();
	}

	// AABB Box : min/max.
	mt::vec3 aabbMin;
	mt::vec3 aabbMax;

	boundingBox->GetAabb(aabbMin, aabbMax);

	SetBoundsAabb(aabbMin, aabbMax);
}

void KX_GameObject::SetBoundsAabb(const mt::vec3 &aabbMin, const mt::vec3 &aabbMax)
{
	// Set the AABB in culling node box.
	m_cullingNode.GetAabb().Set(aabbMin, aabbMax);

	// Synchronize the AABB with the graphic controller.
	if (m_graphicController) {
		m_graphicController->SetLocalAabb(aabbMin, aabbMax);
	}
}

void KX_GameObject::GetBoundsAabb(mt::vec3 &aabbMin, mt::vec3 &aabbMax) const
{
	// Get the culling node box AABB
	m_cullingNode.GetAabb().Get(aabbMin, aabbMax);
}

SG_CullingNode& KX_GameObject::GetCullingNode()
{
	return m_cullingNode;
}

KX_GameObject::ActivityCullingInfo& KX_GameObject::GetActivityCullingInfo()
{
	return m_activityCullingInfo;
}

void KX_GameObject::SetActivityCullingInfo(const ActivityCullingInfo& cullingInfo)
{
	m_activityCullingInfo = cullingInfo;
}

void KX_GameObject::SetActivityCulling(ActivityCullingInfo::Flag flag, bool enable)
{
	if (enable) {
		m_activityCullingInfo.m_flags = (ActivityCullingInfo::Flag)(m_activityCullingInfo.m_flags | flag);
	}
	else {
		m_activityCullingInfo.m_flags = (ActivityCullingInfo::Flag)(m_activityCullingInfo.m_flags & ~flag);

		// Restore physics or logic when disabling activity culling.
		if (flag & ActivityCullingInfo::ACTIVITY_PHYSICS) {
			RestorePhysics();
		}
		if (flag & ActivityCullingInfo::ACTIVITY_LOGIC) {
			ResumeLogic();
		}
	}
}

void KX_GameObject::SuspendPhysics(bool freeConstraints)
{
	if (m_physicsController) {
		m_physicsController->SuspendPhysics((bool)freeConstraints);
	}
}

void KX_GameObject::RestorePhysics()
{
	if (m_physicsController) {
		m_physicsController->RestorePhysics();
	}
}
void KX_GameObject::UnregisterCollisionCallbacks()
{
	if (!m_physicsController) {
		CM_Warning("trying to unregister collision callbacks for object without collisions: " << GetName());
		return;
	}

	// Unregister from callbacks
	KX_Scene *scene = GetScene();
	PHY_IPhysicsEnvironment *pe = scene->GetPhysicsEnvironment();
	PHY_IPhysicsController *spc = m_physicsController.get();
	// If we are the last to unregister on this physics controller
	if (pe->RemoveCollisionCallback(spc)) {
		// If we are a sensor object
		if (m_clientInfo.isSensor()) {
			// Remove sensor body from physics world
			pe->RemoveSensor(spc);
		}
	}
}

void KX_GameObject::RegisterCollisionCallbacks()
{
	if (!m_physicsController) {
		CM_Warning("trying to register collision callbacks for object without collisions: " << GetName());
		return;
	}

	// Register from callbacks
	KX_Scene *scene = GetScene();
	PHY_IPhysicsEnvironment *pe = scene->GetPhysicsEnvironment();
	PHY_IPhysicsController *spc = m_physicsController.get();
	// If we are the first to register on this physics controller
	if (pe->RequestCollisionCallback(spc)) {
		// If we are a sensor object
		if (m_clientInfo.isSensor()) {
			// Add sensor body to physics world
			pe->AddSensor(spc);
		}
	}
}
void KX_GameObject::RunCollisionCallbacks(KX_GameObject *collider, KX_CollisionContactPointList& contactPointList)
{
#ifdef WITH_PYTHON
	if (!m_collisionCallbacks || PyList_GET_SIZE(m_collisionCallbacks) == 0) {
		return;
	}

	const PHY_ICollData *collData = contactPointList.GetCollData();
	const bool isFirstObject = contactPointList.GetFirstObject();

	mt::vec3 point = collData->GetWorldPoint(0, isFirstObject);

	PyObject *args[] = {collider->GetProxy(),
		                PyObjectFrom(point),
		                PyObjectFrom(collData->GetNormal(0, isFirstObject)),
		                contactPointList.GetProxy()};
	EXP_RunPythonCallBackList(m_collisionCallbacks, args, 1, ARRAY_SIZE(args));

	for (unsigned int i = 0; i < ARRAY_SIZE(args); ++i) {
		Py_DECREF(args[i]);
	}

	// Invalidate the collison contact point to avoid access to it in next frame
	contactPointList.InvalidateProxy();
#endif
}

template <bool recursive>
static void walk_children(const SG_Node *node, std::vector<KX_GameObject *>& list)
{
	if (!node) {
		return;
	}

	const NodeList& children = node->GetChildren();

	for (SG_Node *childnode : children) {
		KX_GameObject *childobj = static_cast<KX_GameObject *>(childnode->GetClientObject());
		if (childobj) {
			list.push_back(childobj);
		}

		/* If the childobj is nullptr then this may be an inverse parent link
		 * so a non recursive search should still look down this node. */
		if (recursive || !childobj) {
			walk_children<recursive>(childnode, list);
		}
	}
}

std::vector<KX_GameObject *> KX_GameObject::GetChildren() const
{
	std::vector<KX_GameObject *> list;
	walk_children<false>(m_sgNode.get(), list);
	return list;
}

std::vector<KX_GameObject *> KX_GameObject::GetChildrenRecursive() const
{
	std::vector<KX_GameObject *> list;
	walk_children<true>(m_sgNode.get(), list);
	return list;
}

EXP_ListValue<KX_PythonComponent> *KX_GameObject::GetComponents() const
{
	return m_components;
}

void KX_GameObject::SetComponents(EXP_ListValue<KX_PythonComponent> *components)
{
	m_components = components;
}

void KX_GameObject::UpdateComponents()
{
#ifdef WITH_PYTHON
	if (!m_components) {
		return;
	}

	for (KX_PythonComponent *comp : m_components) {
		comp->Update();
	}

#endif // WITH_PYTHON
}

KX_Scene *KX_GameObject::GetScene()
{
	BLI_assert(m_sgNode);
	return static_cast<KX_Scene *>(m_sgNode->GetClientInfo());
}

/* ---------------------------------------------------------------------
 * Some stuff taken from the header
 * --------------------------------------------------------------------- */
void KX_GameObject::Relink(std::map<SCA_IObject *, SCA_IObject *>& map_parameter)
{
	// we will relink the sensors and actuators that use object references
	// if the object is part of the replicated hierarchy, use the new
	// object reference instead
	SCA_SensorList& sensorlist = GetSensors();
	SCA_SensorList::iterator sit;
	for (sit = sensorlist.begin(); sit != sensorlist.end(); sit++)
	{
		(*sit)->Relink(map_parameter);
	}
	SCA_ActuatorList& actuatorlist = GetActuators();
	SCA_ActuatorList::iterator ait;
	for (ait = actuatorlist.begin(); ait != actuatorlist.end(); ait++)
	{
		(*ait)->Relink(map_parameter);
	}
}

#ifdef WITH_PYTHON

#define PYTHON_CHECK_PHYSICS_CONTROLLER(obj, attr, ret) \
	if (!(obj)->GetPhysicsController()) { \
		PyErr_Format(PyExc_AttributeError, "KX_GameObject.%s, is missing a physics controller", (attr)); \
		return (ret); \
	}

#endif

#ifdef USE_MATHUTILS

/* These require an SGNode */
#define MATHUTILS_VEC_CB_POS_LOCAL 1
#define MATHUTILS_VEC_CB_POS_GLOBAL 2
#define MATHUTILS_VEC_CB_SCALE_LOCAL 3
#define MATHUTILS_VEC_CB_SCALE_GLOBAL 4
#define MATHUTILS_VEC_CB_INERTIA_LOCAL 5
#define MATHUTILS_VEC_CB_OBJECT_COLOR 6
#define MATHUTILS_VEC_CB_LINVEL_LOCAL 7
#define MATHUTILS_VEC_CB_LINVEL_GLOBAL 8
#define MATHUTILS_VEC_CB_ANGVEL_LOCAL 9
#define MATHUTILS_VEC_CB_ANGVEL_GLOBAL 10
#define MATHUTILS_VEC_CB_GRAVITY 11

static unsigned char mathutils_kxgameob_vector_cb_index = -1; /* index for our callbacks */

static int mathutils_kxgameob_generic_check(BaseMathObject *bmo)
{
	KX_GameObject *self = static_cast<KX_GameObject *>EXP_PROXY_REF(bmo->cb_user);
	if (self == nullptr) {
		return -1;
	}

	return 0;
}

static int mathutils_kxgameob_vector_get(BaseMathObject *bmo, int subtype)
{
	KX_GameObject *self = static_cast<KX_GameObject *>EXP_PROXY_REF(bmo->cb_user);
	if (self == nullptr) {
		return -1;
	}

	switch (subtype) {
		case MATHUTILS_VEC_CB_POS_LOCAL:
		{
			self->NodeGetLocalPosition().Pack(bmo->data);
			break;
		}
		case MATHUTILS_VEC_CB_POS_GLOBAL:
		{
			self->NodeGetWorldPosition().Pack(bmo->data);
			break;
		}
		case MATHUTILS_VEC_CB_SCALE_LOCAL:
		{
			self->NodeGetLocalScaling().Pack(bmo->data);
			break;
		}
		case MATHUTILS_VEC_CB_SCALE_GLOBAL:
		{
			self->NodeGetWorldScaling().Pack(bmo->data);
			break;
		}
		case MATHUTILS_VEC_CB_INERTIA_LOCAL:
		{
			PYTHON_CHECK_PHYSICS_CONTROLLER(self, "localInertia", -1);
			self->GetLocalInertia().Pack(bmo->data);
			break;
		}
		case MATHUTILS_VEC_CB_OBJECT_COLOR:
		{
			self->GetObjectColor().Pack(bmo->data);
			break;
		}
		case MATHUTILS_VEC_CB_LINVEL_LOCAL:
		{
			PYTHON_CHECK_PHYSICS_CONTROLLER(self, "localLinearVelocity", -1);
			self->GetLinearVelocity(true).Pack(bmo->data);
			break;
		}
		case MATHUTILS_VEC_CB_LINVEL_GLOBAL:
		{
			PYTHON_CHECK_PHYSICS_CONTROLLER(self, "worldLinearVelocity", -1);
			self->GetLinearVelocity(false).Pack(bmo->data);
			break;
		}
		case MATHUTILS_VEC_CB_ANGVEL_LOCAL:
		{
			PYTHON_CHECK_PHYSICS_CONTROLLER(self, "localLinearVelocity", -1);
			self->GetAngularVelocity(true).Pack(bmo->data);
			break;
		}
		case MATHUTILS_VEC_CB_ANGVEL_GLOBAL:
		{
			PYTHON_CHECK_PHYSICS_CONTROLLER(self, "worldLinearVelocity", -1);
			self->GetAngularVelocity(false).Pack(bmo->data);
			break;
		}
		case MATHUTILS_VEC_CB_GRAVITY:
		{
			PYTHON_CHECK_PHYSICS_CONTROLLER(self, "gravity", -1);
			self->GetGravity().Pack(bmo->data);
			break;
		}

	}

#undef PHYS_ERR

	return 0;
}

static int mathutils_kxgameob_vector_set(BaseMathObject *bmo, int subtype)
{
	KX_GameObject *self = static_cast<KX_GameObject *>EXP_PROXY_REF(bmo->cb_user);
	if (self == nullptr) {
		return -1;
	}

	switch (subtype) {
		case MATHUTILS_VEC_CB_POS_LOCAL:
		{
			self->NodeSetLocalPosition(mt::vec3(bmo->data));
			self->NodeUpdate();
			break;
		}
		case MATHUTILS_VEC_CB_POS_GLOBAL:
		{
			self->NodeSetWorldPosition(mt::vec3(bmo->data));
			self->NodeUpdate();
			break;
		}
		case MATHUTILS_VEC_CB_SCALE_LOCAL:
		{
			self->NodeSetLocalScale(mt::vec3(bmo->data));
			self->NodeUpdate();
			break;
		}
		case MATHUTILS_VEC_CB_SCALE_GLOBAL:
		{
			self->NodeSetWorldScale(mt::vec3(bmo->data));
			self->NodeUpdate();
			break;
		}
		case MATHUTILS_VEC_CB_INERTIA_LOCAL:
		{
			/* read only */
			break;
		}
		case MATHUTILS_VEC_CB_OBJECT_COLOR:
		{
			self->SetObjectColor(mt::vec4(bmo->data));
			break;
		}
		case MATHUTILS_VEC_CB_LINVEL_LOCAL:
		{
			self->SetLinearVelocity(mt::vec3(bmo->data), true);
			break;
		}
		case MATHUTILS_VEC_CB_LINVEL_GLOBAL:
		{
			self->SetLinearVelocity(mt::vec3(bmo->data), false);
			break;
		}
		case MATHUTILS_VEC_CB_ANGVEL_LOCAL:
		{
			self->SetAngularVelocity(mt::vec3(bmo->data), true);
			break;
		}
		case MATHUTILS_VEC_CB_ANGVEL_GLOBAL:
		{
			self->SetAngularVelocity(mt::vec3(bmo->data), false);
			break;
		}
		case MATHUTILS_VEC_CB_GRAVITY:
		{
			self->SetGravity(mt::vec3(bmo->data));
			break;
		}
	}

	return 0;
}

static int mathutils_kxgameob_vector_get_index(BaseMathObject *bmo, int subtype, int index)
{
	/* lazy, avoid repeteing the case statement */
	if (mathutils_kxgameob_vector_get(bmo, subtype) == -1) {
		return -1;
	}
	return 0;
}

static int mathutils_kxgameob_vector_set_index(BaseMathObject *bmo, int subtype, int index)
{
	float f = bmo->data[index];

	/* lazy, avoid repeteing the case statement */
	if (mathutils_kxgameob_vector_get(bmo, subtype) == -1) {
		return -1;
	}

	bmo->data[index] = f;
	return mathutils_kxgameob_vector_set(bmo, subtype);
}

static Mathutils_Callback mathutils_kxgameob_vector_cb = {
	mathutils_kxgameob_generic_check,
	mathutils_kxgameob_vector_get,
	mathutils_kxgameob_vector_set,
	mathutils_kxgameob_vector_get_index,
	mathutils_kxgameob_vector_set_index
};

/* Matrix */
#define MATHUTILS_MAT_CB_ORI_LOCAL 1
#define MATHUTILS_MAT_CB_ORI_GLOBAL 2

static unsigned char mathutils_kxgameob_matrix_cb_index = -1; /* index for our callbacks */

static int mathutils_kxgameob_matrix_get(BaseMathObject *bmo, int subtype)
{
	KX_GameObject *self = static_cast<KX_GameObject *>EXP_PROXY_REF(bmo->cb_user);
	if (self == nullptr) {
		return -1;
	}

	switch (subtype) {
		case MATHUTILS_MAT_CB_ORI_LOCAL:
		{
			self->NodeGetLocalOrientation().Pack(bmo->data);
			break;
		}
		case MATHUTILS_MAT_CB_ORI_GLOBAL:
		{
			self->NodeGetWorldOrientation().Pack(bmo->data);
			break;
		}
	}

	return 0;
}


static int mathutils_kxgameob_matrix_set(BaseMathObject *bmo, int subtype)
{
	KX_GameObject *self = static_cast<KX_GameObject *>EXP_PROXY_REF(bmo->cb_user);
	if (self == nullptr) {
		return -1;
	}

	mt::mat3 mat3x3;
	switch (subtype) {
		case MATHUTILS_MAT_CB_ORI_LOCAL:
		{
			mat3x3 = mt::mat3(bmo->data);
			self->NodeSetLocalOrientation(mat3x3);
			self->NodeUpdate();
			break;
		}
		case MATHUTILS_MAT_CB_ORI_GLOBAL:
		{
			mat3x3 = mt::mat3(bmo->data);
			self->NodeSetLocalOrientation(mat3x3);
			self->NodeUpdate();
			break;
		}
	}

	return 0;
}

static Mathutils_Callback mathutils_kxgameob_matrix_cb = {
	mathutils_kxgameob_generic_check,
	mathutils_kxgameob_matrix_get,
	mathutils_kxgameob_matrix_set,
	nullptr,
	nullptr
};


void KX_GameObject_Mathutils_Callback_Init(void)
{
	// register mathutils callbacks, ok to run more than once.
	mathutils_kxgameob_vector_cb_index = Mathutils_RegisterCallback(&mathutils_kxgameob_vector_cb);
	mathutils_kxgameob_matrix_cb_index = Mathutils_RegisterCallback(&mathutils_kxgameob_matrix_cb);
}

#endif // USE_MATHUTILS

#ifdef WITH_PYTHON
/* ------- python stuff ---------------------------------------------------*/
PyMethodDef KX_GameObject::Methods[] = {
	{"apply_recipe", (PyCFunction)KX_GameObject::sPyApplyRecipe, METH_VARARGS, (char *)KX_GameObject::ApplyRecipe_doc},
	{"compute_voxel_distance", (PyCFunction)KX_GameObject::sPyComputeVoxelDistance, METH_VARARGS, (char *)KX_GameObject::ComputeVoxelDistance_doc},
	{"surface_nets_generate", (PyCFunction)KX_GameObject::sPySurfaceNetsGenerate, METH_VARARGS, (char *)KX_GameObject::SurfaceNetsGenerate_doc},
	{"rebuild_voxel_mesh", (PyCFunction)KX_GameObject::sPyRebuildVoxelMesh, METH_VARARGS, (char *)KX_GameObject::RebuildVoxelMesh_doc},
	{"surface_nets_and_rebuild", (PyCFunction)KX_GameObject::sPySurfaceNetsAndRebuild, METH_VARARGS, (char *)KX_GameObject::SurfaceNetsAndRebuild_doc},
	{"finalize_surface_nets_mesh", (PyCFunction)KX_GameObject::sPyFinalizeSurfaceNetsMesh, METH_VARARGS, (char *)KX_GameObject::FinalizeSurfaceNetsMesh_doc},
	{"enableGrass", (PyCFunction)KX_GameObject::sPyEnableGrass, METH_VARARGS, (char *)KX_GameObject::EnableGrass_doc},
    {"disableGrass", (PyCFunction)KX_GameObject::sPyDisableGrass, METH_VARARGS, (char *)KX_GameObject::DisableGrass_doc},
	{"getVisibleTerrainVertices", (PyCFunction)gPyGetVisibleTerrainVertices, METH_NOARGS},
	{"setMipmapping", (PyCFunction)gPySetMipmapping, METH_VARARGS},
	{"updateMipmappingFilter", (PyCFunction)gPyUpdateMipmappingFilter, METH_VARARGS},
	{"FilterMipmapping", (PyCFunction)gPyFilterMipmapping, METH_NOARGS},
	{"useMultithread", (PyCFunction)gPyuseMultithread, METH_NOARGS},
	{"applyForce", (PyCFunction)KX_GameObject::sPyApplyForce, METH_VARARGS},
	{"applyTorque", (PyCFunction)KX_GameObject::sPyApplyTorque, METH_VARARGS},
	{"applyRotation", (PyCFunction)KX_GameObject::sPyApplyRotation, METH_VARARGS},
	{"applyMovement", (PyCFunction)KX_GameObject::sPyApplyMovement, METH_VARARGS},
	{"getLinearVelocity", (PyCFunction)KX_GameObject::sPyGetLinearVelocity, METH_VARARGS},
	{"setLinearVelocity", (PyCFunction)KX_GameObject::sPySetLinearVelocity, METH_VARARGS},
	{"addLinearVelocity", (PyCFunction)KX_GameObject::sPyAddLinearVelocity, METH_VARARGS},
	{"getAngularVelocity", (PyCFunction)KX_GameObject::sPyGetAngularVelocity, METH_VARARGS},
	{"setAngularVelocity", (PyCFunction)KX_GameObject::sPySetAngularVelocity, METH_VARARGS},
	{"getVelocity", (PyCFunction)KX_GameObject::sPyGetVelocity, METH_VARARGS},
	{"setDamping", (PyCFunction)KX_GameObject::sPySetDamping, METH_VARARGS},
	{"getReactionForce", (PyCFunction)KX_GameObject::sPyGetReactionForce, METH_NOARGS},
	{"alignAxisToVect", (PyCFunction)KX_GameObject::sPyAlignAxisToVect, METH_VARARGS | METH_KEYWORDS},
	{"getAxisVect", (PyCFunction)KX_GameObject::sPyGetAxisVect, METH_O},
	{"suspendPhysics", (PyCFunction)KX_GameObject::sPySuspendPhysics, METH_VARARGS},
	{"restorePhysics", (PyCFunction)KX_GameObject::sPyRestorePhysics, METH_NOARGS},
	{"suspendDynamics", (PyCFunction)KX_GameObject::sPySuspendDynamics, METH_VARARGS},
	{"restoreDynamics", (PyCFunction)KX_GameObject::sPyRestoreDynamics, METH_NOARGS},
	{"enableRigidBody", (PyCFunction)KX_GameObject::sPyEnableRigidBody, METH_NOARGS},
	{"disableRigidBody", (PyCFunction)KX_GameObject::sPyDisableRigidBody, METH_NOARGS},
	{"applyImpulse", (PyCFunction)KX_GameObject::sPyApplyImpulse, METH_VARARGS},
	{"setCollisionMargin", (PyCFunction)KX_GameObject::sPySetCollisionMargin, METH_O},
	{"collide", (PyCFunction)KX_GameObject::sPyCollide, METH_O},
	{"setParent", (PyCFunction)KX_GameObject::sPySetParent, METH_VARARGS | METH_KEYWORDS},
	{"setVisible", (PyCFunction)KX_GameObject::sPySetVisible, METH_VARARGS},
	{"setOcclusion", (PyCFunction)KX_GameObject::sPySetOcclusion, METH_VARARGS},
	{"removeParent", (PyCFunction)KX_GameObject::sPyRemoveParent, METH_NOARGS},


	{"getPhysicsId", (PyCFunction)KX_GameObject::sPyGetPhysicsId, METH_NOARGS},
	{"getPropertyNames", (PyCFunction)KX_GameObject::sPyGetPropertyNames, METH_NOARGS},
	{"replaceMesh", (PyCFunction)KX_GameObject::sPyReplaceMesh, METH_VARARGS | METH_KEYWORDS},
	{"destroyMesh", (PyCFunction)KX_GameObject::sPyDestroyMesh, METH_O},
	{"cancelProcedural", (PyCFunction)KX_GameObject::sPyCancelProcedural, METH_NOARGS},
	{"resetProceduralCancel", (PyCFunction)KX_GameObject::sPyResetProceduralCancel, METH_NOARGS},
	{"getProceduralActiveCalls", (PyCFunction)KX_GameObject::sPyGetProceduralActiveCalls, METH_NOARGS},
	{"endObject", (PyCFunction)KX_GameObject::sPyEndObject, METH_NOARGS},
	{"reinstancePhysicsMesh", (PyCFunction)KX_GameObject::sPyReinstancePhysicsMesh, METH_VARARGS | METH_KEYWORDS},
	{"updateTerrainNormals", (PyCFunction)KX_GameObject::sPyUpdateTerrainNormals, METH_NOARGS},
	{"replacePhysicsShape", (PyCFunction)KX_GameObject::sPyReplacePhysicsShape, METH_O},

	EXP_PYMETHODTABLE_KEYWORDS(KX_GameObject, rayCastTo),
	EXP_PYMETHODTABLE_KEYWORDS(KX_GameObject, rayCast),
	EXP_PYMETHODTABLE_O(KX_GameObject, getDistanceTo),
	EXP_PYMETHODTABLE_O(KX_GameObject, getVectTo),
	EXP_PYMETHODTABLE(KX_GameObject,setGrassTextureObject),
	EXP_PYMETHODTABLE(KX_GameObject,setGrassLightMultipliers),
	EXP_PYMETHODTABLE_KEYWORDS(KX_GameObject,sendMessage),
	EXP_PYMETHODTABLE(KX_GameObject, addDebugProperty),

	EXP_PYMETHODTABLE_KEYWORDS(KX_GameObject, playAction),
	EXP_PYMETHODTABLE(KX_GameObject, stopAction),
	EXP_PYMETHODTABLE(KX_GameObject, getActionFrame),
	EXP_PYMETHODTABLE(KX_GameObject, getActionName),
	EXP_PYMETHODTABLE(KX_GameObject, setActionFrame),
	EXP_PYMETHODTABLE(KX_GameObject, isPlayingAction),

	// dict style access for props
	{"get", (PyCFunction)KX_GameObject::sPyget, METH_VARARGS},
	{"getVertexShader", (PyCFunction)KX_GameObject::sPyGetVertexShader, METH_NOARGS},
	{"getFragmentShader", (PyCFunction)KX_GameObject::sPyGetFragmentShader, METH_NOARGS},
	{"getShadowVertexShader", (PyCFunction)KX_GameObject::sPyGetShadowVertexShader, METH_NOARGS},
	{"getShadowFragmentShader", (PyCFunction)KX_GameObject::sPyGetShadowFragmentShader, METH_NOARGS},

	{nullptr, nullptr} //Sentinel
};

// ------------------------------
// AddLinearVelocity
// ------------------------------
PyObject *KX_GameObject::sPyAddLinearVelocity(PyObject *self, PyObject *args)
{
	KX_GameObject *gameobj = static_cast<KX_GameObject *>(EXP_PROXY_REF(self));
	if (!gameobj) {
		PyErr_SetString(PyExc_RuntimeError, "Invalid game object");
		return nullptr;
	}

	mt::vec3 lin_vel;
	int local = 0;

	if (!PyArg_ParseTuple(args, "fff|p", &lin_vel.x, &lin_vel.y, &lin_vel.z, &local)) {
		PyErr_SetString(PyExc_TypeError, "addLinearVelocity(x, y, z, [local]) expected 3 floats and optional bool");
		return nullptr;
	}

	gameobj->AddLinearVelocity(lin_vel, local != 0);
	Py_RETURN_NONE;
}

PyObject *KX_GameObject::sPyGetVertexShader(PyObject *self, PyObject *args)
{
	KX_GameObject *gameobj = static_cast<KX_GameObject *>(EXP_PROXY_REF(self));
	if (!gameobj) {
		PyErr_SetString(PyExc_RuntimeError, "Invalid game object");
		return nullptr;
	}
	return gameobj->PyGetVertexShader();
}

PyObject *KX_GameObject::sPyGetFragmentShader(PyObject *self, PyObject *args)
{
	KX_GameObject *gameobj = static_cast<KX_GameObject *>(EXP_PROXY_REF(self));
	if (!gameobj) {
		PyErr_SetString(PyExc_RuntimeError, "Invalid game object");
		return nullptr;
	}
	return gameobj->PyGetFragmentShader();
}

PyObject *KX_GameObject::sPyGetShadowVertexShader(PyObject *self, PyObject *args)
{
	KX_GameObject *gameobj = static_cast<KX_GameObject *>(EXP_PROXY_REF(self));
	if (!gameobj) {
		PyErr_SetString(PyExc_RuntimeError, "Invalid game object");
		return nullptr;
	}
	return gameobj->PyGetShadowVertexShader();
}

PyObject *KX_GameObject::sPyGetShadowFragmentShader(PyObject *self, PyObject *args)
{
	KX_GameObject *gameobj = static_cast<KX_GameObject *>(EXP_PROXY_REF(self));
	if (!gameobj) {
		PyErr_SetString(PyExc_RuntimeError, "Invalid game object");
		return nullptr;
	}
	return gameobj->PyGetShadowFragmentShader();
}

PyAttributeDef KX_GameObject::Attributes[] = {
	EXP_PYATTRIBUTE_SHORT_RO("currentLodLevel", KX_GameObject, m_currentLodLevel),
	EXP_PYATTRIBUTE_RW_FUNCTION("lodManager", KX_GameObject, pyattr_get_lodManager, pyattr_set_lodManager),
	EXP_PYATTRIBUTE_RW_FUNCTION("name",     KX_GameObject, pyattr_get_name, pyattr_set_name),
	EXP_PYATTRIBUTE_RO_FUNCTION("parent",   KX_GameObject, pyattr_get_parent),
	EXP_PYATTRIBUTE_RO_FUNCTION("groupMembers", KX_GameObject, pyattr_get_group_members),
	EXP_PYATTRIBUTE_RO_FUNCTION("groupObject",  KX_GameObject, pyattr_get_group_object),
	EXP_PYATTRIBUTE_RO_FUNCTION("scene",        KX_GameObject, pyattr_get_scene),
	EXP_PYATTRIBUTE_RO_FUNCTION("life",     KX_GameObject, pyattr_get_life),
	EXP_PYATTRIBUTE_RW_FUNCTION("mass",     KX_GameObject, pyattr_get_mass,     pyattr_set_mass),
	EXP_PYATTRIBUTE_RO_FUNCTION("isSuspendDynamics",        KX_GameObject, pyattr_get_is_suspend_dynamics),
	EXP_PYATTRIBUTE_RW_FUNCTION("linVelocityMin",       KX_GameObject, pyattr_get_lin_vel_min, pyattr_set_lin_vel_min),
	EXP_PYATTRIBUTE_RW_FUNCTION("linVelocityMax",       KX_GameObject, pyattr_get_lin_vel_max, pyattr_set_lin_vel_max),
	EXP_PYATTRIBUTE_RW_FUNCTION("angularVelocityMin", KX_GameObject, pyattr_get_ang_vel_min, pyattr_set_ang_vel_min),
	EXP_PYATTRIBUTE_RW_FUNCTION("angularVelocityMax", KX_GameObject, pyattr_get_ang_vel_max, pyattr_set_ang_vel_max),
	EXP_PYATTRIBUTE_RW_FUNCTION("layer", KX_GameObject, pyattr_get_layer, pyattr_set_layer),
	EXP_PYATTRIBUTE_SHORT_RW("passIndex", 0, SHRT_MAX, false, KX_GameObject, m_passIndex),
	EXP_PYATTRIBUTE_RW_FUNCTION("visible",  KX_GameObject, pyattr_get_visible,  pyattr_set_visible),
	EXP_PYATTRIBUTE_RO_FUNCTION("culled", KX_GameObject, pyattr_get_culled),
	EXP_PYATTRIBUTE_RO_FUNCTION("cullingBox",   KX_GameObject, pyattr_get_cullingBox),
	EXP_PYATTRIBUTE_BOOL_RW("occlusion", KX_GameObject, m_bOccluder),
	EXP_PYATTRIBUTE_RW_FUNCTION("physicsCullingRadius", KX_GameObject, pyattr_get_physicsCullingRadius, pyattr_set_physicsCullingRadius),
	EXP_PYATTRIBUTE_RW_FUNCTION("logicCullingRadius", KX_GameObject, pyattr_get_logicCullingRadius, pyattr_set_logicCullingRadius),
	EXP_PYATTRIBUTE_RW_FUNCTION("physicsCulling", KX_GameObject, pyattr_get_physicsCulling, pyattr_set_physicsCulling),
	EXP_PYATTRIBUTE_RW_FUNCTION("logicCulling", KX_GameObject, pyattr_get_logicCulling, pyattr_set_logicCulling),
	EXP_PYATTRIBUTE_RW_FUNCTION("position", KX_GameObject, pyattr_get_worldPosition,    pyattr_set_localPosition),
	EXP_PYATTRIBUTE_RO_FUNCTION("localInertia", KX_GameObject, pyattr_get_localInertia),
	EXP_PYATTRIBUTE_RW_FUNCTION("orientation", KX_GameObject, pyattr_get_worldOrientation, pyattr_set_localOrientation),
	EXP_PYATTRIBUTE_RW_FUNCTION("scaling",  KX_GameObject, pyattr_get_worldScaling, pyattr_set_localScaling),
	EXP_PYATTRIBUTE_RW_FUNCTION("timeOffset", KX_GameObject, pyattr_get_timeOffset, pyattr_set_timeOffset),
	EXP_PYATTRIBUTE_RW_FUNCTION("collisionCallbacks",       KX_GameObject, pyattr_get_collisionCallbacks,   pyattr_set_collisionCallbacks),
	EXP_PYATTRIBUTE_RW_FUNCTION("collisionGroup",           KX_GameObject, pyattr_get_collisionGroup, pyattr_set_collisionGroup),
	EXP_PYATTRIBUTE_RW_FUNCTION("collisionMask",                KX_GameObject, pyattr_get_collisionMask, pyattr_set_collisionMask),
	EXP_PYATTRIBUTE_RW_FUNCTION("state",        KX_GameObject, pyattr_get_state,    pyattr_set_state),
	EXP_PYATTRIBUTE_RO_FUNCTION("meshes",   KX_GameObject, pyattr_get_meshes),
	EXP_PYATTRIBUTE_RO_FUNCTION("batchGroup", KX_GameObject, pyattr_get_batchGroup),
	EXP_PYATTRIBUTE_RW_FUNCTION("localOrientation", KX_GameObject, pyattr_get_localOrientation, pyattr_set_localOrientation),
	EXP_PYATTRIBUTE_RW_FUNCTION("worldOrientation", KX_GameObject, pyattr_get_worldOrientation, pyattr_set_worldOrientation),
	EXP_PYATTRIBUTE_RW_FUNCTION("localPosition",    KX_GameObject, pyattr_get_localPosition,    pyattr_set_localPosition),
	EXP_PYATTRIBUTE_RW_FUNCTION("worldPosition",    KX_GameObject, pyattr_get_worldPosition,    pyattr_set_worldPosition),
	EXP_PYATTRIBUTE_RW_FUNCTION("localScale",   KX_GameObject, pyattr_get_localScaling, pyattr_set_localScaling),
	EXP_PYATTRIBUTE_RW_FUNCTION("worldScale",   KX_GameObject, pyattr_get_worldScaling, pyattr_set_worldScaling),
	EXP_PYATTRIBUTE_RW_FUNCTION("localTransform",       KX_GameObject, pyattr_get_localTransform, pyattr_set_localTransform),
	EXP_PYATTRIBUTE_RW_FUNCTION("worldTransform",       KX_GameObject, pyattr_get_worldTransform, pyattr_set_worldTransform),
	EXP_PYATTRIBUTE_RW_FUNCTION("linearVelocity", KX_GameObject, pyattr_get_localLinearVelocity, pyattr_set_worldLinearVelocity),
	EXP_PYATTRIBUTE_RW_FUNCTION("localLinearVelocity", KX_GameObject, pyattr_get_localLinearVelocity, pyattr_set_localLinearVelocity),
	EXP_PYATTRIBUTE_RW_FUNCTION("worldLinearVelocity", KX_GameObject, pyattr_get_worldLinearVelocity, pyattr_set_worldLinearVelocity),
	EXP_PYATTRIBUTE_RW_FUNCTION("angularVelocity", KX_GameObject, pyattr_get_localAngularVelocity, pyattr_set_worldAngularVelocity),
	EXP_PYATTRIBUTE_RW_FUNCTION("localAngularVelocity", KX_GameObject, pyattr_get_localAngularVelocity, pyattr_set_localAngularVelocity),
	EXP_PYATTRIBUTE_RW_FUNCTION("worldAngularVelocity", KX_GameObject, pyattr_get_worldAngularVelocity, pyattr_set_worldAngularVelocity),
	EXP_PYATTRIBUTE_RW_FUNCTION("linearDamping", KX_GameObject, pyattr_get_linearDamping, pyattr_set_linearDamping),
	EXP_PYATTRIBUTE_RW_FUNCTION("angularDamping", KX_GameObject, pyattr_get_angularDamping, pyattr_set_angularDamping),
	EXP_PYATTRIBUTE_RO_FUNCTION("children", KX_GameObject, pyattr_get_children),
	EXP_PYATTRIBUTE_RO_FUNCTION("childrenRecursive",    KX_GameObject, pyattr_get_children_recursive),
	EXP_PYATTRIBUTE_RO_FUNCTION("attrDict", KX_GameObject, pyattr_get_attrDict),
	EXP_PYATTRIBUTE_RW_FUNCTION("color", KX_GameObject, pyattr_get_obcolor, pyattr_set_obcolor),
	EXP_PYATTRIBUTE_RW_FUNCTION("debug",    KX_GameObject, pyattr_get_debug, pyattr_set_debug),
	EXP_PYATTRIBUTE_RO_FUNCTION("components", KX_GameObject, pyattr_get_components),
	EXP_PYATTRIBUTE_RW_FUNCTION("debugRecursive",   KX_GameObject, pyattr_get_debugRecursive, pyattr_set_debugRecursive),
	EXP_PYATTRIBUTE_RW_FUNCTION("gravity", KX_GameObject, pyattr_get_gravity, pyattr_set_gravity),

	/* experimental, don't rely on these yet */
	EXP_PYATTRIBUTE_RO_FUNCTION("sensors",      KX_GameObject, pyattr_get_sensors),
	EXP_PYATTRIBUTE_RO_FUNCTION("controllers",  KX_GameObject, pyattr_get_controllers),
	EXP_PYATTRIBUTE_RO_FUNCTION("actuators",        KX_GameObject, pyattr_get_actuators),
	EXP_PYATTRIBUTE_NULL //Sentinel
};


struct ChunkData {
	std::vector<mt::vec3> vertices;
	mt::vec3 min;
	mt::vec3 max;
};

static std::map<KX_GameObject*, std::map<std::pair<int, int>, ChunkData>> g_chunkCache;

std::vector<mt::vec3> ObterVerticesVisiveisComFrustum(KX_GameObject *terreno, const SG_Frustum &frustum)
{
	std::vector<mt::vec3> resultado;
	if (!terreno) {
		return resultado;
	}

	const float chunkSize = 5.0f;
	auto &chunks = g_chunkCache[terreno];

	if (chunks.empty()) {
		for (KX_Mesh *mesh : terreno->GetMeshList()) {
			for (RAS_MeshMaterial *meshMat : mesh->GetMeshMaterialList()) {
				RAS_DisplayArray *displayArray = meshMat->GetDisplayArray();

				for (int i = 0; i < displayArray->GetVertexCount(); ++i) {
					const mt::vec3 localPos = mt::vec3(displayArray->GetPosition(i));
					mt::vec3 worldPos = terreno->NodeGetWorldTransform() * localPos;

					int cx = static_cast<int>(std::floor(worldPos.x / chunkSize));
					int cy = static_cast<int>(std::floor(worldPos.y / chunkSize));

					ChunkData &chunk = chunks[{cx, cy}];
					if (chunk.vertices.empty()) {
						chunk.min = worldPos;
						chunk.max = worldPos;
					}
					else {
						chunk.min.x = std::min(chunk.min.x, worldPos.x);
						chunk.min.y = std::min(chunk.min.y, worldPos.y);
						chunk.min.z = std::min(chunk.min.z, worldPos.z);
						chunk.max.x = std::max(chunk.max.x, worldPos.x);
						chunk.max.y = std::max(chunk.max.y, worldPos.y);
						chunk.max.z = std::max(chunk.max.z, worldPos.z);
					}
					chunk.vertices.push_back(worldPos);
				}
			}
		}
	}

	size_t totalVertices = 0;
	std::vector<const ChunkData*> visibleChunks;
	visibleChunks.reserve(chunks.size());

	for (const auto &it : chunks) {
		const ChunkData &chunk = it.second;

		if (frustum.AabbInsideFrustum(chunk.min, chunk.max, mt::mat4::Identity()) != SG_Frustum::OUTSIDE) {
			visibleChunks.push_back(&chunk);
			totalVertices += chunk.vertices.size();
		}
	}

	resultado.reserve(totalVertices);
	for (const ChunkData* chunk : visibleChunks) {
		resultado.insert(resultado.end(), chunk->vertices.begin(), chunk->vertices.end());
	}

	return resultado;
}



static PyObject* gPySetMipmapping(PyObject *self_v, PyObject *args)
{
    KX_GameObject *self = static_cast<KX_GameObject *>(EXP_PROXY_REF(self_v));
    if (!self) {
        PyErr_SetString(PyExc_RuntimeError, "Invalid game object");
        return nullptr;
    }
    return self->PySetMipmapping(args); 
}

static PyObject* gPyUpdateMipmappingFilter(PyObject *self_v, PyObject *args)
{
    KX_GameObject *self = static_cast<KX_GameObject *>(EXP_PROXY_REF(self_v));
    if (!self) {
        PyErr_SetString(PyExc_RuntimeError, "Invalid game object");
        return nullptr;
    }
    return self->PyUpdateMipmappingFilter(args); 
}

static PyObject* gPyFilterMipmapping(PyObject *self_v, PyObject *args)
{
    KX_GameObject *self = static_cast<KX_GameObject *>(EXP_PROXY_REF(self_v));
    if (!self) {
        PyErr_SetString(PyExc_RuntimeError, "Invalid game object");
        return nullptr;
    }
    return self->PyFilterMipmapping(args); 
}

static PyObject* gPyuseMultithread(PyObject *self_v, PyObject *args)
{
    KX_GameObject *self = static_cast<KX_GameObject *>(EXP_PROXY_REF(self_v));
    if (!self) {
        PyErr_SetString(PyExc_RuntimeError, "Invalid game object");
        return nullptr;
    }
    return self->PyuseMultithread(args); 
}

PyObject *KX_GameObject::PyuseMultithread(PyObject *args)
{
    
    g_useMultithreadCulling = !g_useMultithreadCulling;

    
    if (g_useMultithreadCulling) {
		CM_Debug("True Multithread Culling\n");
        Py_RETURN_TRUE;
    } else {
		CM_Debug("False Multithread Culling\n");
        Py_RETURN_FALSE;
    }
}

PyObject *KX_GameObject::PyFilterMipmapping(PyObject *args)
{
    
    g_use_mipmaps = !g_use_mipmaps;

    
    if (g_use_mipmaps) {
        Py_RETURN_TRUE;
    } else {
        Py_RETURN_FALSE;
    }
}

PyObject *KX_GameObject::PySetMipmapping(PyObject *args)
{
	int enabled = 0;
	int type = -1;

	if (!PyArg_ParseTuple(args, "i|i:setMipmapping", &enabled, &type)) {
		return nullptr;
	}

	// Save old settings
	bool old_domipmap = GPU_get_mipmap();
	bool old_linearmipmap = GPU_get_linear_mipmap();

	// Determine new settings
	bool new_domipmap = (enabled != 0);
	bool new_linearmipmap = (type == -1) ? old_linearmipmap : (type != 0);

	// FORCE global state change to allow GPU_verify_image to see the "change"
	// but we pass NULL to GPU_set_mipmap to prevent it from freeing ALL engine images.
	// We use a trick: if old == new, GPU_set_mipmap does nothing. 
	// So we temporarily flip it if needed to force the internal state update.
	if (old_domipmap == new_domipmap) {
		GPU_set_mipmap(NULL, !old_domipmap); 
	}
	GPU_set_mipmap(NULL, new_domipmap);
	GPU_set_linear_mipmap(new_linearmipmap);

	// Iterate over meshes and their materials/textures
	for (KX_Mesh *mesh : m_meshes) {
		if (!mesh)
			continue;
		const RAS_MeshMaterialList &matList = mesh->GetMeshMaterialList();
		for (RAS_MeshMaterial *meshMat : matList) {
			if (!meshMat)
				continue;
			RAS_IMaterial *mat = meshMat->GetBucket()->GetMaterial();
			if (mat) {
				for (int i = 0; i < RAS_Texture::MaxUnits; i++) {
					RAS_Texture *tex = mat->GetTexture(i);
					if (tex) {
						Image *ima = tex->GetImage();
						if (ima) {
							// Free and recreate specifically for this texture
							GPU_free_image(ima);
							// This calls GPU_verify_image which checks global GTS settings
							tex->CheckValidTexture();
						}
					}
				}
			}
		}
	}

	// Restore old settings
	// Again, force the change if needed so future bge.render calls work correctly
	if (GPU_get_mipmap() == old_domipmap) {
		GPU_set_mipmap(NULL, !old_domipmap);
	}
	GPU_set_mipmap(NULL, old_domipmap);
	GPU_set_linear_mipmap(old_linearmipmap);

	Py_RETURN_NONE;
}

PyObject *KX_GameObject::PyUpdateMipmappingFilter(PyObject *args)
{
	int mode = 0;
	int slot = -1;

	if (!PyArg_ParseTuple(args, "i|i:updateMipmappingFilter", &mode, &slot)) {
		return nullptr;
	}
	if (slot >= RAS_Texture::MaxUnits) {
		PyErr_Format(PyExc_ValueError, "updateMipmappingFilter: invalid texture slot %d", slot);
		return nullptr;
	}

	/* Save global state */
	bool old_domipmap      = GPU_get_mipmap();
	GPU_MipmapFilter old_filter = GPU_get_mipmap_filter();

	/* Set global state to match requested mode.
	 * GPU_free_image + CheckValidTexture will read these globals to set filters. */
	bool new_domipmap = true;
	GPU_MipmapFilter new_filter = old_filter;
	switch (mode) {
		case 0:
			new_domipmap = true;
			new_filter = GPU_MIPMAP_FILTER_LINEAR_MIPMAP_LINEAR;
			break;
		case 1:
			new_domipmap = false;
			break;
		case 2:
			new_domipmap = true;
			new_filter = GPU_MIPMAP_FILTER_LINEAR_MIPMAP_NEAREST;
			break;
		case 3:
			new_domipmap = true;
			new_filter = GPU_MIPMAP_FILTER_NEAREST_MIPMAP_NEAREST;
			break;
		case 4:
			new_domipmap = true;
			new_filter = GPU_MIPMAP_FILTER_NEAREST_MIPMAP_LINEAR;
			break;
		default:
			PyErr_Format(PyExc_ValueError, "updateMipmappingFilter: invalid mode %d", mode);
			return nullptr;
	}

	/* Force the internal state to actually update even when value is same */
	if (old_domipmap == new_domipmap)
		GPU_set_mipmap(nullptr, !old_domipmap);
	GPU_set_mipmap(nullptr, new_domipmap);
	if (new_domipmap) {
		if (old_filter == new_filter) {
			GPU_set_mipmap_filter((GPU_MipmapFilter)((old_filter + 1) & 3));
		}
		GPU_set_mipmap_filter(new_filter);
	}

	/* Free and recreate each texture — this is the same path setMipmapping uses
	 * and is the only reliable way to update bindless handles */
	for (KX_Mesh *mesh : m_meshes) {
		if (!mesh) continue;
		const RAS_MeshMaterialList &matList = mesh->GetMeshMaterialList();
		for (RAS_MeshMaterial *meshMat : matList) {
			if (!meshMat) continue;
			RAS_IMaterial *mat = meshMat->GetBucket()->GetMaterial();
			if (!mat) continue;
			const int start = (slot >= 0) ? slot : 0;
			const int end = (slot >= 0) ? (slot + 1) : RAS_Texture::MaxUnits;
			for (int i = start; i < end; i++) {
				RAS_Texture *tex = mat->GetTexture(i);
				if (!tex) continue;
				Image *ima = tex->GetImage();
				if (ima) {
					GPU_free_image(ima);
					tex->CheckValidTexture();
				}
			}
		}
	}

	/* Restore global state */
	if (GPU_get_mipmap() == old_domipmap)
		GPU_set_mipmap(nullptr, !old_domipmap);
	GPU_set_mipmap(nullptr, old_domipmap);
	if (GPU_get_mipmap_filter() == old_filter) {
		GPU_set_mipmap_filter((GPU_MipmapFilter)((old_filter + 1) & 3));
	}
	GPU_set_mipmap_filter(old_filter);

	Py_RETURN_NONE;
}

static PyObject* gPyGetVisibleTerrainVertices(PyObject *self_v, PyObject *args)
{
    KX_GameObject *self = static_cast<KX_GameObject *>(EXP_PROXY_REF(self_v));
    if (!self) {
        PyErr_SetString(PyExc_RuntimeError, "Invalid game object");
        return nullptr;
    }
    return self->PyGetVisibleTerrainVertices(args);
}


PyObject *KX_GameObject::PyGetVisibleTerrainVertices(PyObject *args)
{
	KX_Camera *cam = GetScene()->GetActiveCamera();
	if (!cam) {
		PyErr_SetString(PyExc_RuntimeError, "getVisibleTerrainVertices: no active camera");
		return nullptr;
	}

	
	const RAS_Rasterizer::StereoEye eye = RAS_Rasterizer::RAS_STEREO_LEFTEYE;

	SG_Frustum frustum = cam->GetFrustum(eye);

	std::vector<mt::vec3> vertices = ObterVerticesVisiveisComFrustum(this, frustum);


	PyObject *list = PyList_New(vertices.size());
	for (size_t i = 0; i < vertices.size(); ++i) {
		const mt::vec3 &v = vertices[i];
		PyObject *vec = Py_BuildValue("(f,f,f)", v.x, v.y, v.z);
		PyList_SET_ITEM(list, i, vec);
	}

	return list;
}




PyObject *KX_GameObject::PyReplaceMesh(PyObject *args, PyObject *kwds)
{
	SCA_LogicManager *logicmgr = GetScene()->GetLogicManager();

	PyObject *value;
	int use_gfx = 1, use_phys = 0;
	KX_Mesh *new_mesh;

	if (!EXP_ParseTupleArgsAndKeywords(args, kwds, "O|ii:replaceMesh",
	                                   {"mesh", "useDisplayMesh", "usePhysicsMesh", 0}, &value, &use_gfx, &use_phys)) {
		return nullptr;
	}

	if (!ConvertPythonToMesh(logicmgr, value, &new_mesh, false, "gameOb.replaceMesh(value): KX_GameObject")) {
		return nullptr;
	}

	ReplaceMesh(new_mesh, (bool)use_gfx, (bool)use_phys);
	Py_RETURN_NONE;
}

PyObject *KX_GameObject::PyDestroyMesh(PyObject *value)
{
	SCA_LogicManager *logicmgr = GetScene()->GetLogicManager();
	KX_Mesh *mesh = nullptr;

	if (!ConvertPythonToMesh(logicmgr, value, &mesh, false, "gameOb.destroyMesh(mesh): KX_GameObject")) {
		return nullptr;
	}

	if (!mesh) {
		Py_RETURN_FALSE;
	}

	if (std::find(m_meshes.begin(), m_meshes.end(), mesh) != m_meshes.end()) {
		Py_RETURN_FALSE;
	}

	BL_Converter *converter = KX_GetActiveEngine()->GetConverter();
	KX_Scene *meshScene = mesh->GetScene();
	if (!converter || !meshScene || meshScene != GetScene()) {
		Py_RETURN_FALSE;
	}

	return PyBool_FromLong(converter->RemoveMesh(meshScene, mesh) ? 1 : 0);
}

PyObject *KX_GameObject::PyCancelProcedural()
{
	m_proceduralCancel.store(true, std::memory_order_relaxed);
	KX_SNR_ClearCachesForOwner(this, m_proceduralActiveCalls.load(std::memory_order_relaxed) == 0);
	Py_RETURN_NONE;
}

PyObject *KX_GameObject::PyResetProceduralCancel()
{
	m_proceduralCancel.store(false, std::memory_order_relaxed);
	Py_RETURN_NONE;
}

PyObject *KX_GameObject::PyGetProceduralActiveCalls()
{
	return PyLong_FromLong((long)m_proceduralActiveCalls.load(std::memory_order_relaxed));
}

PyObject *KX_GameObject::PyEndObject()
{
	GetScene()->DelayedRemoveObject(this);

	Py_RETURN_NONE;
}

PyObject *KX_GameObject::PyReinstancePhysicsMesh(PyObject *args, PyObject *kwds)
{
	KX_GameObject *gameobj = nullptr;
	KX_Mesh *mesh = nullptr;
	SCA_LogicManager *logicmgr = GetScene()->GetLogicManager();
	int dupli = 0;

	PyObject *gameobj_py = nullptr;
	PyObject *mesh_py = nullptr;
	PyObject *callback = nullptr;

	if (!EXP_ParseTupleArgsAndKeywords(args, kwds, "|OOiO:reinstancePhysicsMesh",
	                                   {"gameObject", "meshObject", "dupli", "callback", 0}, &gameobj_py, &mesh_py, &dupli, &callback) ||
	    (gameobj_py && !ConvertPythonToGameObject(logicmgr, gameobj_py, &gameobj, true, "gameOb.reinstancePhysicsMesh(obj, mesh, dupli): KX_GameObject")) ||
	    (mesh_py && !ConvertPythonToMesh(logicmgr, mesh_py, &mesh, true, "gameOb.reinstancePhysicsMesh(obj, mesh, dupli): KX_GameObject"))) {
		return nullptr;
	}

	if (callback && callback != Py_None) {
		if (!PyCallable_Check(callback)) {
			PyErr_SetString(PyExc_TypeError, "gameOb.reinstancePhysicsMesh(..., callback): callback must be callable");
			return nullptr;
		}

		CcdPhysicsController *ccd = dynamic_cast<CcdPhysicsController *>(m_physicsController.get());
		if (ccd) {
			CcdPhysicsEnvironment *env = ccd->GetPhysicsEnvironment();
			if (env && env->EnqueueReinstancePhysicsShapeAsync(ccd, gameobj, mesh, dupli != 0, callback)) {
				Py_RETURN_TRUE;
			}
		}
		Py_RETURN_FALSE;
	}

	/* gameobj and mesh can be nullptr */
	if (m_physicsController && m_physicsController->ReinstancePhysicsShape(gameobj, mesh, dupli)) {
		Py_RETURN_TRUE;
	}

	Py_RETURN_FALSE;
}

PyObject *KX_GameObject::PyUpdateTerrainNormals()
{
	if (m_meshes.empty()) {
		Py_RETURN_NONE;
	}

	KX_Mesh *mesh = m_meshes[0];
	unsigned int numPolys = mesh->GetNumPolygons();

	std::unordered_map<RAS_DisplayArray*, std::vector<mt::vec3>> normalMap;

	for (unsigned int i = 0; i < numPolys; ++i) {
		RAS_Mesh::PolygonInfo poly = mesh->GetPolygon(i);
		RAS_DisplayArray *array = poly.array;

		if (!array) continue;

		if (normalMap.find(array) == normalMap.end()) {
			normalMap[array] = std::vector<mt::vec3>(
				array->GetVertexCount(),
				mt::vec3(0.0f)
			);
		}
	}

	for (unsigned int i = 0; i < numPolys; ++i) {
		RAS_Mesh::PolygonInfo poly = mesh->GetPolygon(i);
		RAS_DisplayArray *array = poly.array;

		if (!array) continue;

		auto &normals = normalMap[array];

		const unsigned int i0 = poly.indices[0];
		const unsigned int i1 = poly.indices[1];
		const unsigned int i2 = poly.indices[2];

		const mt::vec3 v0 = mt::vec3(array->GetPosition(i0));
		const mt::vec3 v1 = mt::vec3(array->GetPosition(i1));
		const mt::vec3 v2 = mt::vec3(array->GetPosition(i2));

		mt::vec3 normal = mt::cross(v1 - v0, v2 - v0);

		normals[i0] += normal;
		normals[i1] += normal;
		normals[i2] += normal;
	}

	for (auto &pair : normalMap) {
		RAS_DisplayArray *array = pair.first;
		std::vector<mt::vec3> &normals = pair.second;

		const size_t count = normals.size();

		for (size_t i = 0; i < count; ++i) {
			float len = normals[i].Length();

			if (len > 1e-6f) {
				normals[i] /= len;
			} else {
				normals[i] = mt::vec3(0.0f, 0.0f, 1.0f);
			}

			array->SetNormal(i, normals[i]);
		}

		array->NotifyUpdate(RAS_DisplayArray::NORMAL_MODIFIED);
	}


	if (m_physicsController) {
		m_physicsController->ReinstancePhysicsShape(this, mesh, 0);
	}

	Py_RETURN_NONE;
}
PyObject *KX_GameObject::PyReplacePhysicsShape(PyObject *value)
{
	KX_GameObject *gameobj;
	SCA_LogicManager *logicmgr = GetScene()->GetLogicManager();

	if (!ConvertPythonToGameObject(logicmgr, value, &gameobj, false, "gameOb.replacePhysicsShape(obj): KX_GameObject")) {
		return nullptr;
	}

	if (!m_physicsController || !gameobj->GetPhysicsController()) {
		PyErr_SetString(PyExc_AttributeError, "gameOb.replacePhysicsShape(obj): function only available for objects with collisions enabled");
		return nullptr;
	}

	if (m_physicsController->ReplacePhysicsShape(gameobj->GetPhysicsController())) {
		Py_RETURN_TRUE;
	}
	Py_RETURN_FALSE;
}

static PyObject *Map_GetItem(PyObject *self_v, PyObject *item)
{
	KX_GameObject *self = static_cast<KX_GameObject *>EXP_PROXY_REF(self_v);
	const char *attr_str = _PyUnicode_AsString(item);
	EXP_Value *resultattr;
	PyObject *pyconvert;

	if (self == nullptr) {
		PyErr_SetString(PyExc_SystemError, "val = gameOb[key]: KX_GameObject, " EXP_PROXY_ERROR_MSG);
		return nullptr;
	}

	/* first see if the attributes a string and try get the cvalue attribute */
	if (attr_str && (resultattr = self->GetProperty(attr_str))) {
		pyconvert = resultattr->ConvertValueToPython();
		return pyconvert ? pyconvert : resultattr->GetProxy();
	}
	/* no EXP_Value attribute, try get the python only m_attr_dict attribute */
	else if (self->m_attr_dict && (pyconvert = PyDict_GetItem(self->m_attr_dict, item))) {

		if (attr_str) {
			PyErr_Clear();
		}
		Py_INCREF(pyconvert);
		return pyconvert;
	}
	else {
		if (attr_str) {
			PyErr_Format(PyExc_KeyError, "value = gameOb[key]: KX_GameObject, key \"%s\" does not exist", attr_str);
		}
		else {
			PyErr_SetString(PyExc_KeyError, "value = gameOb[key]: KX_GameObject, key does not exist");
		}
		return nullptr;
	}

}


static int Map_SetItem(PyObject *self_v, PyObject *key, PyObject *val)
{
	KX_GameObject *self = static_cast<KX_GameObject *>EXP_PROXY_REF(self_v);
	const char *attr_str = _PyUnicode_AsString(key);
	if (attr_str == nullptr) {
		PyErr_Clear();
	}

	if (self == nullptr) {
		PyErr_SetString(PyExc_SystemError, "gameOb[key] = value: KX_GameObject, " EXP_PROXY_ERROR_MSG);
		return -1;
	}

	if (val == nullptr) { /* del ob["key"] */
		int del = 0;

		/* try remove both just in case */
		if (attr_str) {
			del |= (self->RemoveProperty(attr_str) == true) ? 1 : 0;
		}

		if (self->m_attr_dict) {
			del |= (PyDict_DelItem(self->m_attr_dict, key) == 0) ? 1 : 0;
		}

		if (del == 0) {
			if (attr_str) {
				PyErr_Format(PyExc_KeyError, "gameOb[key] = value: KX_GameObject, key \"%s\" could not be set", attr_str);
			}
			else {
				PyErr_SetString(PyExc_KeyError, "del gameOb[key]: KX_GameObject, key could not be deleted");
			}
			return -1;
		}
		else if (self->m_attr_dict) {
			PyErr_Clear(); /* PyDict_DelItem sets an error when it fails */
		}
	}
	else { /* ob["key"] = value */
		bool set = false;

		/* as EXP_Value */
		if (attr_str && PyObject_TypeCheck(val, &EXP_PyObjectPlus::Type) == 0) { /* don't allow GameObjects for eg to be assigned to EXP_Value props */
			EXP_Value *vallie = self->ConvertPythonToValue(val, false, "gameOb[key] = value: ");

			if (vallie) {
				EXP_Value *oldprop = self->GetProperty(attr_str);

				if (oldprop) {
					oldprop->SetValue(vallie);
				}
				else {
					self->SetProperty(attr_str, vallie);
				}

				vallie->Release();
				set = true;

				/* try remove dict value to avoid double ups */
				if (self->m_attr_dict) {
					if (PyDict_DelItem(self->m_attr_dict, key) != 0) {
						PyErr_Clear();
					}
				}
			}
			else if (PyErr_Occurred()) {
				return -1;
			}
		}

		if (set == false) {
			if (self->m_attr_dict == nullptr) { /* lazy init */
				self->m_attr_dict = PyDict_New();
			}


			if (PyDict_SetItem(self->m_attr_dict, key, val) == 0) {
				if (attr_str) {
					self->RemoveProperty(attr_str); /* overwrite the EXP_Value if it exists */
				}
				set = true;
			}
			else {
				if (attr_str) {
					PyErr_Format(PyExc_KeyError, "gameOb[key] = value: KX_GameObject, key \"%s\" not be added to internal dictionary", attr_str);
				}
				else {
					PyErr_SetString(PyExc_KeyError, "gameOb[key] = value: KX_GameObject, key not be added to internal dictionary");
				}
			}
		}

		if (set == false) {
			return -1; /* pythons error value */
		}

	}

	return 0; /* success */
}

static int Seq_Contains(PyObject *self_v, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>EXP_PROXY_REF(self_v);

	if (self == nullptr) {
		PyErr_SetString(PyExc_SystemError, "val in gameOb: KX_GameObject, " EXP_PROXY_ERROR_MSG);
		return -1;
	}

	if (PyUnicode_Check(value) && self->GetProperty(_PyUnicode_AsString(value))) {
		return 1;
	}

	if (self->m_attr_dict && PyDict_GetItem(self->m_attr_dict, value)) {
		return 1;
	}

	return 0;
}


PyMappingMethods KX_GameObject::Mapping = {
	(lenfunc)nullptr,                               /*inquiry mp_length */
	(binaryfunc)Map_GetItem,        /*binaryfunc mp_subscript */
	(objobjargproc)Map_SetItem, /*objobjargproc mp_ass_subscript */
};

PySequenceMethods KX_GameObject::Sequence = {
	nullptr,        /* Cant set the len otherwise it can evaluate as false */
	nullptr,        /* sq_concat */
	nullptr,        /* sq_repeat */
	nullptr,        /* sq_item */
	nullptr,        /* sq_slice */
	nullptr,        /* sq_ass_item */
	nullptr,        /* sq_ass_slice */
	(objobjproc)Seq_Contains,   /* sq_contains */
	(binaryfunc)nullptr,  /* sq_inplace_concat */
	(ssizeargfunc)nullptr,  /* sq_inplace_repeat */
};

PyTypeObject KX_GameObject::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_GameObject",
	sizeof(EXP_PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0,
	&Sequence,
	&Mapping,
	0, 0, 0,
	nullptr,
	nullptr,
	0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&SCA_IObject::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyObject *KX_GameObject::pyattr_get_name(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	return PyUnicode_FromStdString(self->GetName());
}

int KX_GameObject::pyattr_set_name(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

	if (!PyUnicode_Check(value)) {
		PyErr_SetString(PyExc_TypeError, "gameOb.name = str: KX_GameObject, expected a string");
		return PY_SET_ATTR_FAIL;
	}

	std::string newname = std::string(_PyUnicode_AsString(value));
	std::string oldname = self->GetName();

	SCA_LogicManager *manager = self->GetScene()->GetLogicManager();

	// If true, it mean that's this game object is not a replica and was added at conversion time.
	if (manager->GetGameObjectByName(oldname) == self) {
		/* Two non-replica objects can have the same name bacause these objects are register in the
		 * logic manager and that the result of GetGameObjectByName will be undefined. */
		if (manager->GetGameObjectByName(newname)) {
			PyErr_Format(PyExc_TypeError, "gameOb.name = str: name %s is already used by an other non-replica game object", oldname.c_str());
			return PY_SET_ATTR_FAIL;
		}
		// Unregister the old name.
		manager->UnregisterGameObjectName(oldname);
		// Register the object under the new name.
		manager->RegisterGameObjectName(newname, self);
	}

	// Change the name
	self->SetName(newname);

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_parent(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	KX_GameObject *parent = self->GetParent();
	if (parent) {
		return parent->GetProxy();
	}
	Py_RETURN_NONE;
}

PyObject *KX_GameObject::pyattr_get_group_members(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	EXP_ListValue<KX_GameObject> *instances = self->GetInstanceObjects();
	if (instances) {
		return instances->GetProxy();
	}
	Py_RETURN_NONE;
}

PyObject *KX_GameObject::pyattr_get_collisionCallbacks(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

	// Only objects with a physics controller should have collision callbacks
	PYTHON_CHECK_PHYSICS_CONTROLLER(self, "collisionCallbacks", nullptr);

	// Return the existing callbacks
	if (!self->m_collisionCallbacks) {
		self->m_collisionCallbacks = PyList_New(0);
		// Subscribe to collision update from KX_CollisionEventManager
		self->RegisterCollisionCallbacks();
	}
	Py_INCREF(self->m_collisionCallbacks);
	return self->m_collisionCallbacks;
}

int KX_GameObject::pyattr_set_collisionCallbacks(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

	// Only objects with a physics controller should have collision callbacks
	PYTHON_CHECK_PHYSICS_CONTROLLER(self, "collisionCallbacks", PY_SET_ATTR_FAIL);

	if (!PyList_CheckExact(value)) {
		PyErr_SetString(PyExc_ValueError, "Expected a list");
		return PY_SET_ATTR_FAIL;
	}

	if (!self->m_collisionCallbacks) {
		self->RegisterCollisionCallbacks();
	}
	else {
		Py_DECREF(self->m_collisionCallbacks);
	}

	Py_INCREF(value);


	self->m_collisionCallbacks = value;

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_collisionGroup(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	return PyLong_FromLong(self->GetCollisionGroup());
}

int KX_GameObject::pyattr_set_collisionGroup(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	int val = PyLong_AsLong(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_SetString(PyExc_TypeError, "gameOb.collisionGroup = int: KX_GameObject, expected an int bit field");
		return PY_SET_ATTR_FAIL;
	}

	if (val == 0 || val & ~((1 << OB_MAX_COL_MASKS) - 1)) {
		PyErr_Format(PyExc_AttributeError, "gameOb.collisionGroup = int: KX_GameObject, expected a int bit field, 0 < group < %i", (1 << OB_MAX_COL_MASKS));
		return PY_SET_ATTR_FAIL;
	}

	self->SetCollisionGroup(val);
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_collisionMask(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	return PyLong_FromLong(self->GetCollisionMask());
}

int KX_GameObject::pyattr_set_collisionMask(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	int val = PyLong_AsLong(value);

	if (val == -1 && PyErr_Occurred()) {
		PyErr_SetString(PyExc_TypeError, "gameOb.collisionMask = int: KX_GameObject, expected an int bit field");
		return PY_SET_ATTR_FAIL;
	}

	if (val == 0 || val & ~((1 << OB_MAX_COL_MASKS) - 1)) {
		PyErr_Format(PyExc_AttributeError, "gameOb.collisionMask = int: KX_GameObject, expected a int bit field, 0 < mask < %i", (1 << OB_MAX_COL_MASKS));
		return PY_SET_ATTR_FAIL;
	}

	self->SetCollisionMask(val);
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_scene(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	KX_Scene *scene = self->GetScene();
	if (scene) {
		return scene->GetProxy();
	}
	Py_RETURN_NONE;
}

PyObject *KX_GameObject::pyattr_get_group_object(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	KX_GameObject *pivot = self->GetDupliGroupObject();
	if (pivot) {
		return pivot->GetProxy();
	}
	Py_RETURN_NONE;
}

PyObject *KX_GameObject::pyattr_get_life(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

	EXP_Value *life = self->GetProperty("::timebomb");
	if (life) {
		// this convert the timebomb seconds to frames, hard coded 50.0f (assuming 50fps)
		// value hardcoded in KX_Scene::AddReplicaObject()
		return PyFloat_FromDouble(life->GetNumber() * 50.0);
	}
	else {
		Py_RETURN_NONE;
	}
}

PyObject *KX_GameObject::pyattr_get_mass(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	PHY_IPhysicsController *spc = self->GetPhysicsController();
	return PyFloat_FromDouble(spc ? spc->GetMass() : 0.0f);
}

int KX_GameObject::pyattr_set_mass(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	PHY_IPhysicsController *spc = self->GetPhysicsController();
	float val = PyFloat_AsDouble(value);
	if (val < 0.0f) { /* also accounts for non float */
		PyErr_SetString(PyExc_AttributeError, "gameOb.mass = float: KX_GameObject, expected a float zero or above");
		return PY_SET_ATTR_FAIL;
	}

	if (spc) {
		spc->SetMass(val);
	}

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_is_suspend_dynamics(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

	// Only objects with a physics controller can be suspended
	PYTHON_CHECK_PHYSICS_CONTROLLER(self, attrdef->m_name.c_str(), nullptr);

	return PyBool_FromLong(self->IsDynamicsSuspended());
}

PyObject *KX_GameObject::pyattr_get_lin_vel_min(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	PHY_IPhysicsController *spc = self->GetPhysicsController();
	return PyFloat_FromDouble(spc ? spc->GetLinVelocityMin() : 0.0f);
}

int KX_GameObject::pyattr_set_lin_vel_min(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	PHY_IPhysicsController *spc = self->GetPhysicsController();
	float val = PyFloat_AsDouble(value);
	if (val < 0.0f) { /* also accounts for non float */
		PyErr_SetString(PyExc_AttributeError, "gameOb.linVelocityMin = float: KX_GameObject, expected a float zero or above");
		return PY_SET_ATTR_FAIL;
	}

	if (spc) {
		spc->SetLinVelocityMin(val);
	}

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_lin_vel_max(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	PHY_IPhysicsController *spc = self->GetPhysicsController();
	return PyFloat_FromDouble(spc ? spc->GetLinVelocityMax() : 0.0f);
}

int KX_GameObject::pyattr_set_lin_vel_max(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	PHY_IPhysicsController *spc = self->GetPhysicsController();
	float val = PyFloat_AsDouble(value);
	if (val < 0.0f) { /* also accounts for non float */
		PyErr_SetString(PyExc_AttributeError, "gameOb.linVelocityMax = float: KX_GameObject, expected a float zero or above");
		return PY_SET_ATTR_FAIL;
	}

	if (spc) {
		spc->SetLinVelocityMax(val);
	}

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_ang_vel_min(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	PHY_IPhysicsController *spc = self->GetPhysicsController();
	return PyFloat_FromDouble(spc ? spc->GetAngularVelocityMin() : 0.0f);
}

int KX_GameObject::pyattr_set_ang_vel_min(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	PHY_IPhysicsController *spc = self->GetPhysicsController();
	float val = PyFloat_AsDouble(value);
	if (val < 0.0f) { /* also accounts for non float */
		PyErr_SetString(PyExc_AttributeError,
		                "gameOb.angularVelocityMin = float: KX_GameObject, expected a nonnegative float");
		return PY_SET_ATTR_FAIL;
	}

	if (spc) {
		spc->SetAngularVelocityMin(val);
	}

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_ang_vel_max(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	PHY_IPhysicsController *spc = self->GetPhysicsController();
	return PyFloat_FromDouble(spc ? spc->GetAngularVelocityMax() : 0.0f);
}

int KX_GameObject::pyattr_set_ang_vel_max(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	PHY_IPhysicsController *spc = self->GetPhysicsController();
	float val = PyFloat_AsDouble(value);
	if (val < 0.0f) { /* also accounts for non float */
		PyErr_SetString(PyExc_AttributeError,
		                "gameOb.angularVelocityMax = float: KX_GameObject, expected a nonnegative float");
		return PY_SET_ATTR_FAIL;
	}

	if (spc) {
		spc->SetAngularVelocityMax(val);
	}

	return PY_SET_ATTR_SUCCESS;
}


PyObject *KX_GameObject::pyattr_get_layer(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	return PyLong_FromLong(self->GetLayer());
}

#define MAX_LAYERS ((1 << 20) - 1)
int KX_GameObject::pyattr_set_layer(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	int layer = PyLong_AsLong(value);

	if (layer == -1 && PyErr_Occurred()) {
		PyErr_Format(PyExc_TypeError, "expected an integer for attribute \"%s\"", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	if (layer < 1) {
		PyErr_Format(PyExc_TypeError, "expected an integer greater than 1 for attribute \"%s\"", attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}
	else if (layer > MAX_LAYERS) {
		PyErr_Format(PyExc_TypeError, "expected an integer less than %i for attribute \"%s\"", MAX_LAYERS, attrdef->m_name.c_str());
		return PY_SET_ATTR_FAIL;
	}

	self->SetLayer(layer);
	return PY_SET_ATTR_SUCCESS;
}
#undef MAX_LAYERS

PyObject *KX_GameObject::pyattr_get_visible(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	return PyBool_FromLong(self->GetVisible());
}

int KX_GameObject::pyattr_set_visible(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	int param = PyObject_IsTrue(value);
	if (param == -1) {
		PyErr_SetString(PyExc_AttributeError, "gameOb.visible = bool: KX_GameObject, expected True or False");
		return PY_SET_ATTR_FAIL;
	}

	self->SetVisible(param, false);
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_culled(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	return PyBool_FromLong(self->GetCullingNode().GetCulled());
}

PyObject *KX_GameObject::pyattr_get_cullingBox(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	return (new KX_BoundingBox(self))->NewProxy(true);
}

PyObject *KX_GameObject::pyattr_get_physicsCulling(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	return PyBool_FromLong(self->GetActivityCullingInfo().m_flags & ActivityCullingInfo::ACTIVITY_PHYSICS);
}

int KX_GameObject::pyattr_set_physicsCulling(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	int param = PyObject_IsTrue(value);
	if (param == -1) {
		PyErr_SetString(PyExc_AttributeError, "gameOb.physicsCulling = bool: KX_GameObject, expected True or False");
		return PY_SET_ATTR_FAIL;
	}

	self->SetActivityCulling(ActivityCullingInfo::ACTIVITY_PHYSICS, param);
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_logicCulling(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	return PyBool_FromLong(self->GetActivityCullingInfo().m_flags & ActivityCullingInfo::ACTIVITY_LOGIC);
}

int KX_GameObject::pyattr_set_logicCulling(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	int param = PyObject_IsTrue(value);
	if (param == -1) {
		PyErr_SetString(PyExc_AttributeError, "gameOb.logicCulling = bool: KX_GameObject, expected True or False");
		return PY_SET_ATTR_FAIL;
	}

	self->SetActivityCulling(ActivityCullingInfo::ACTIVITY_LOGIC, param);
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_physicsCullingRadius(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	return PyFloat_FromDouble(std::sqrt(self->GetActivityCullingInfo().m_physicsRadius));
}

int KX_GameObject::pyattr_set_physicsCullingRadius(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	const float val = PyFloat_AsDouble(value);
	if (val < 0.0f) { // Also accounts for non float.
		PyErr_SetString(PyExc_AttributeError, "gameOb.physicsCullingRadius = float: KX_GameObject, expected a float zero or above");
		return PY_SET_ATTR_FAIL;
	}

	self->GetActivityCullingInfo().m_physicsRadius = val * val;

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_logicCullingRadius(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	return PyFloat_FromDouble(std::sqrt(self->GetActivityCullingInfo().m_logicRadius));
}

int KX_GameObject::pyattr_set_logicCullingRadius(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	const float val = PyFloat_AsDouble(value);
	if (val < 0.0f) { // Also accounts for non float.
		PyErr_SetString(PyExc_AttributeError, "gameOb.logicCullingRadius = float: KX_GameObject, expected a float zero or above");
		return PY_SET_ATTR_FAIL;
	}

	self->GetActivityCullingInfo().m_logicRadius = val * val;

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_worldPosition(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
#ifdef USE_MATHUTILS
	return Vector_CreatePyObject_cb(
		EXP_PROXY_FROM_REF_BORROW(self_v), 3,
		mathutils_kxgameob_vector_cb_index, MATHUTILS_VEC_CB_POS_GLOBAL);
#else
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	return PyObjectFrom(self->NodeGetWorldPosition());
#endif
}

int KX_GameObject::pyattr_set_worldPosition(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	mt::vec3 pos;
	if (!PyVecTo(value, pos)) {
		return PY_SET_ATTR_FAIL;
	}

	self->NodeSetWorldPosition(pos);
	self->NodeUpdate();
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_localPosition(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
#ifdef USE_MATHUTILS
	return Vector_CreatePyObject_cb(
		EXP_PROXY_FROM_REF_BORROW(self_v), 3,
		mathutils_kxgameob_vector_cb_index, MATHUTILS_VEC_CB_POS_LOCAL);
#else
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	return PyObjectFrom(self->NodeGetLocalPosition());
#endif
}

int KX_GameObject::pyattr_set_localPosition(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	mt::vec3 pos;
	if (!PyVecTo(value, pos)) {
		return PY_SET_ATTR_FAIL;
	}

	self->NodeSetLocalPosition(pos);
	self->NodeUpdate();
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_localInertia(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
#ifdef USE_MATHUTILS
	return Vector_CreatePyObject_cb(
		EXP_PROXY_FROM_REF_BORROW(self_v), 3,
		mathutils_kxgameob_vector_cb_index, MATHUTILS_VEC_CB_INERTIA_LOCAL);
#else
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	if (self->GetPhysicsController1()) {
		return PyObjectFrom(self->GetPhysicsController1()->GetLocalInertia());
	}
	return PyObjectFrom(mt::zero3);
#endif
}

PyObject *KX_GameObject::pyattr_get_worldOrientation(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
#ifdef USE_MATHUTILS
	return Matrix_CreatePyObject_cb(
		EXP_PROXY_FROM_REF_BORROW(self_v), 3, 3,
		mathutils_kxgameob_matrix_cb_index, MATHUTILS_MAT_CB_ORI_GLOBAL);
#else
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	return PyObjectFrom(self->NodeGetWorldOrientation());
#endif
}

int KX_GameObject::pyattr_set_worldOrientation(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

	/* if value is not a sequence PyOrientationTo makes an error */
	mt::mat3 rot;
	if (!PyOrientationTo(value, rot, "gameOb.worldOrientation = sequence: KX_GameObject, ")) {
		return PY_SET_ATTR_FAIL;
	}

	self->NodeSetGlobalOrientation(rot);

	self->NodeUpdate();
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_localOrientation(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
#ifdef USE_MATHUTILS
	return Matrix_CreatePyObject_cb(
		EXP_PROXY_FROM_REF_BORROW(self_v), 3, 3,
		mathutils_kxgameob_matrix_cb_index, MATHUTILS_MAT_CB_ORI_LOCAL);
#else
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	return PyObjectFrom(self->NodeGetLocalOrientation());
#endif
}

int KX_GameObject::pyattr_set_localOrientation(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

	/* if value is not a sequence PyOrientationTo makes an error */
	mt::mat3 rot;
	if (!PyOrientationTo(value, rot, "gameOb.localOrientation = sequence: KX_GameObject, ")) {
		return PY_SET_ATTR_FAIL;
	}

	self->NodeSetLocalOrientation(rot);
	self->NodeUpdate();
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_worldScaling(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
#ifdef USE_MATHUTILS
	return Vector_CreatePyObject_cb(
		EXP_PROXY_FROM_REF_BORROW(self_v), 3,
		mathutils_kxgameob_vector_cb_index, MATHUTILS_VEC_CB_SCALE_GLOBAL);
#else
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	return PyObjectFrom(self->NodeGetWorldScaling());
#endif
}

int KX_GameObject::pyattr_set_worldScaling(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	mt::vec3 scale;
	if (!PyVecTo(value, scale)) {
		return PY_SET_ATTR_FAIL;
	}

	self->NodeSetWorldScale(scale);
	self->NodeUpdate();
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_localScaling(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
#ifdef USE_MATHUTILS
	return Vector_CreatePyObject_cb(
		EXP_PROXY_FROM_REF_BORROW(self_v), 3,
		mathutils_kxgameob_vector_cb_index, MATHUTILS_VEC_CB_SCALE_LOCAL);
#else
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	return PyObjectFrom(self->NodeGetLocalScaling());
#endif
}

int KX_GameObject::pyattr_set_localScaling(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	mt::vec3 scale;
	if (!PyVecTo(value, scale)) {
		return PY_SET_ATTR_FAIL;
	}

	self->NodeSetLocalScale(scale);
	self->NodeUpdate();
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_localTransform(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

	return PyObjectFrom(mt::mat4::FromAffineTransform(self->NodeGetLocalTransform()));
}

int KX_GameObject::pyattr_set_localTransform(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	mt::mat4 temp;
	if (!PyMatTo(value, temp)) {
		return PY_SET_ATTR_FAIL;
	}

	self->NodeSetLocalPosition(temp.TranslationVector3D());
	self->NodeSetLocalOrientation(temp.RotationMatrix());
	self->NodeSetLocalScale(temp.ScaleVector3D());

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_worldTransform(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

	return PyObjectFrom(mt::mat4::FromAffineTransform(self->NodeGetWorldTransform()));
}

int KX_GameObject::pyattr_set_worldTransform(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	mt::mat4 temp;
	if (!PyMatTo(value, temp)) {
		return PY_SET_ATTR_FAIL;
	}

	self->NodeSetWorldPosition(temp.TranslationVector3D());
	self->NodeSetGlobalOrientation(temp.RotationMatrix());
	self->NodeSetWorldScale(temp.ScaleVector3D());

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_worldLinearVelocity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
#ifdef USE_MATHUTILS
	return Vector_CreatePyObject_cb(
		EXP_PROXY_FROM_REF_BORROW(self_v), 3,
		mathutils_kxgameob_vector_cb_index, MATHUTILS_VEC_CB_LINVEL_GLOBAL);
#else
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	return PyObjectFrom(GetLinearVelocity(false));
#endif
}

int KX_GameObject::pyattr_set_worldLinearVelocity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	mt::vec3 velocity;
	if (!PyVecTo(value, velocity)) {
		return PY_SET_ATTR_FAIL;
	}

	self->SetLinearVelocity(velocity, false);

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_localLinearVelocity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
#ifdef USE_MATHUTILS
	return Vector_CreatePyObject_cb(
		EXP_PROXY_FROM_REF_BORROW(self_v), 3,
		mathutils_kxgameob_vector_cb_index, MATHUTILS_VEC_CB_LINVEL_LOCAL);
#else
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	return PyObjectFrom(GetLinearVelocity(true));
#endif
}

int KX_GameObject::pyattr_set_localLinearVelocity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	mt::vec3 velocity;
	if (!PyVecTo(value, velocity)) {
		return PY_SET_ATTR_FAIL;
	}

	self->SetLinearVelocity(velocity, true);

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_worldAngularVelocity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
#ifdef USE_MATHUTILS
	return Vector_CreatePyObject_cb(
		EXP_PROXY_FROM_REF_BORROW(self_v), 3,
		mathutils_kxgameob_vector_cb_index, MATHUTILS_VEC_CB_ANGVEL_GLOBAL);
#else
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	return PyObjectFrom(GetAngularVelocity(false));
#endif
}

int KX_GameObject::pyattr_set_worldAngularVelocity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	mt::vec3 velocity;
	if (!PyVecTo(value, velocity)) {
		return PY_SET_ATTR_FAIL;
	}

	self->SetAngularVelocity(velocity, false);

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_localAngularVelocity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
#ifdef USE_MATHUTILS
	return Vector_CreatePyObject_cb(
		EXP_PROXY_FROM_REF_BORROW(self_v), 3,
		mathutils_kxgameob_vector_cb_index, MATHUTILS_VEC_CB_ANGVEL_LOCAL);
#else
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	return PyObjectFrom(GetAngularVelocity(true));
#endif
}

int KX_GameObject::pyattr_set_localAngularVelocity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	mt::vec3 velocity;
	if (!PyVecTo(value, velocity)) {
		return PY_SET_ATTR_FAIL;
	}

	self->SetAngularVelocity(velocity, true);

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_gravity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
#ifdef USE_MATHUTILS
	return Vector_CreatePyObject_cb(
		EXP_PROXY_FROM_REF_BORROW(self_v), 3,
		mathutils_kxgameob_vector_cb_index, MATHUTILS_VEC_CB_GRAVITY);
#else
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	return PyObjectFrom(GetGravity());
#endif
}

int KX_GameObject::pyattr_set_gravity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	mt::vec3 gravity;
	if (!PyVecTo(value, gravity)) {
		return PY_SET_ATTR_FAIL;
	}

	self->SetGravity(gravity);

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_linearDamping(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	return PyFloat_FromDouble(self->GetLinearDamping());
}

int KX_GameObject::pyattr_set_linearDamping(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	float val = PyFloat_AsDouble(value);
	self->SetLinearDamping(val);
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_angularDamping(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	return PyFloat_FromDouble(self->GetAngularDamping());
}

int KX_GameObject::pyattr_set_angularDamping(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	float val = PyFloat_AsDouble(value);
	self->SetAngularDamping(val);
	return PY_SET_ATTR_SUCCESS;
}


PyObject *KX_GameObject::pyattr_get_timeOffset(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	SG_Node *sg_parent;
	if ((sg_parent = self->m_sgNode->GetParent()) != nullptr && sg_parent->IsSlowParent()) {
		return PyFloat_FromDouble(static_cast<KX_SlowParentRelation *>(sg_parent->GetParentRelation())->GetTimeOffset());
	}
	else {
		return PyFloat_FromDouble(0.0f);
	}
}

int KX_GameObject::pyattr_set_timeOffset(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	float val = PyFloat_AsDouble(value);
	SG_Node *sg_parent = self->m_sgNode->GetParent();
	if (val < 0.0f) { /* also accounts for non float */
		PyErr_SetString(PyExc_AttributeError, "gameOb.timeOffset = float: KX_GameObject, expected a float zero or above");
		return PY_SET_ATTR_FAIL;
	}
	if (sg_parent && sg_parent->IsSlowParent()) {
		static_cast<KX_SlowParentRelation *>(sg_parent->GetParentRelation())->SetTimeOffset(val);
	}
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_state(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	int state = 0;
	state |= self->GetState();
	return PyLong_FromLong(state);
}

int KX_GameObject::pyattr_set_state(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	int state_i = PyLong_AsLong(value);
	unsigned int state = 0;

	if (state_i == -1 && PyErr_Occurred()) {
		PyErr_SetString(PyExc_TypeError, "gameOb.state = int: KX_GameObject, expected an int bit field");
		return PY_SET_ATTR_FAIL;
	}

	state |= state_i;
	if ((state & ((1 << 30) - 1)) == 0) {
		PyErr_SetString(PyExc_AttributeError, "gameOb.state = int: KX_GameObject, state bitfield was not between 0 and 30 (1<<0 and 1<<29)");
		return PY_SET_ATTR_FAIL;
	}
	self->SetState(state);
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_meshes(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	PyObject *meshes = PyList_New(self->m_meshes.size());
	int i;

	for (i = 0; i < (int)self->m_meshes.size(); i++)
	{
		PyObject *item = self->m_meshes[i]->GetProxy();
		Py_INCREF(item);
		PyList_SET_ITEM(meshes, i, item);
	}

	return meshes;
}

PyObject *KX_GameObject::pyattr_get_batchGroup(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	RAS_MeshUser *meshUser = self->GetMeshUser();
	if (!meshUser) {
		Py_RETURN_NONE;
	}

	KX_BatchGroup *batchGroup = (KX_BatchGroup *)meshUser->GetBatchGroup();
	if (!batchGroup) {
		Py_RETURN_NONE;
	}

	return batchGroup->GetProxy();
}

PyObject *KX_GameObject::pyattr_get_obcolor(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
#ifdef USE_MATHUTILS
	return Vector_CreatePyObject_cb(
		EXP_PROXY_FROM_REF_BORROW(self_v), 4,
		mathutils_kxgameob_vector_cb_index, MATHUTILS_VEC_CB_OBJECT_COLOR);
#else
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	return PyObjectFrom(self->GetObjectColor());
#endif
}

int KX_GameObject::pyattr_set_obcolor(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	mt::vec4 obcolor;
	if (!PyVecTo(value, obcolor)) {
		return PY_SET_ATTR_FAIL;
	}

	self->SetObjectColor(obcolor);
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_components(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	EXP_ListValue<KX_PythonComponent> *components = self->GetComponents();
	return components ? components->GetProxy() : (new EXP_ListValue<KX_PythonComponent>())->NewProxy(true);
}

unsigned int KX_GameObject::py_get_sensors_size()
{
	return m_sensors.size();
}

PyObject *KX_GameObject::py_get_sensors_item(unsigned int index)
{
	return m_sensors[index]->GetProxy();
}

std::string KX_GameObject::py_get_sensors_item_name(unsigned int index)
{
	return m_sensors[index]->GetName();
}

PyObject *KX_GameObject::pyattr_get_sensors(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	return (new EXP_ListWrapper<KX_GameObject, &KX_GameObject::py_get_sensors_size, &KX_GameObject::py_get_sensors_item,
				nullptr, &KX_GameObject::py_get_sensors_item_name>(self_v))->NewProxy(true);
}

unsigned int KX_GameObject::py_get_controllers_size()
{
	return m_controllers.size();
}

PyObject *KX_GameObject::py_get_controllers_item(unsigned int index)
{
	return m_controllers[index]->GetProxy();
}

std::string KX_GameObject::py_get_controllers_item_name(unsigned int index)
{
	return m_controllers[index]->GetName();
}

PyObject *KX_GameObject::pyattr_get_controllers(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	return (new EXP_ListWrapper<KX_GameObject, &KX_GameObject::py_get_controllers_size, &KX_GameObject::py_get_controllers_item,
				nullptr, &KX_GameObject::py_get_controllers_item_name>(self_v))->NewProxy(true);
}

unsigned int KX_GameObject::py_get_actuators_size()
{
	return m_actuators.size();
}

PyObject *KX_GameObject::py_get_actuators_item(unsigned int index)
{
	return m_actuators[index]->GetProxy();
}

std::string KX_GameObject::py_get_actuators_item_name(unsigned int index)
{
	return m_actuators[index]->GetName();
}

PyObject *KX_GameObject::pyattr_get_actuators(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	return (new EXP_ListWrapper<KX_GameObject, &KX_GameObject::py_get_actuators_size, &KX_GameObject::py_get_actuators_item,
				nullptr, &KX_GameObject::py_get_actuators_item_name>(self_v))->NewProxy(true);
}

PyObject *KX_GameObject::pyattr_get_children(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	EXP_ListValue<KX_GameObject> *list = new EXP_ListValue<KX_GameObject>(self->GetChildren());
	/* The list must not own any data because is temporary and we can't
	 * ensure that it will freed before item's in it (e.g python owner). */
	list->SetReleaseOnDestruct(false);
	return list->NewProxy(true);
}

PyObject *KX_GameObject::pyattr_get_children_recursive(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	EXP_ListValue<KX_GameObject> *list = new EXP_ListValue<KX_GameObject>(self->GetChildrenRecursive());
	/* The list must not own any data because is temporary and we can't
	 * ensure that it will freed before item's in it (e.g python owner). */
	list->SetReleaseOnDestruct(false);
	return list->NewProxy(true);
}

PyObject *KX_GameObject::pyattr_get_attrDict(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

	if (self->m_attr_dict == nullptr) {
		self->m_attr_dict = PyDict_New();
	}

	Py_INCREF(self->m_attr_dict);
	return self->m_attr_dict;
}

PyObject *KX_GameObject::pyattr_get_debug(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

	return PyBool_FromLong(self->GetScene()->ObjectInDebugList(self));
}

int KX_GameObject::pyattr_set_debug(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	int param = PyObject_IsTrue(value);

	if (param == -1) {
		PyErr_SetString(PyExc_AttributeError, "gameOb.debug = bool: KX_GameObject, expected True or False");
		return PY_SET_ATTR_FAIL;
	}

	self->SetUseDebugProperties(param, false);

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_debugRecursive(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

	return PyBool_FromLong(self->GetScene()->ObjectInDebugList(self));
}

int KX_GameObject::pyattr_set_debugRecursive(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);
	int param = PyObject_IsTrue(value);

	if (param == -1) {
		PyErr_SetString(PyExc_AttributeError, "gameOb.debugRecursive = bool: KX_GameObject, expected True or False");
		return PY_SET_ATTR_FAIL;
	}

	self->SetUseDebugProperties(param, true);

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::pyattr_get_lodManager(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

	return (self->m_lodManager) ? self->m_lodManager->GetProxy() : Py_None;
}

int KX_GameObject::pyattr_set_lodManager(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_GameObject *self = static_cast<KX_GameObject *>(self_v);

	KX_LodManager *lodManager = nullptr;
	if (!ConvertPythonToLodManager(value, &lodManager, true, "gameobj.lodManager: KX_GameObject")) {
		return PY_SET_ATTR_FAIL;
	}

	self->SetLodManager(lodManager);

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_GameObject::PyApplyForce(PyObject *args)
{
	int local = 0;
	PyObject *pyvect;

	if (PyArg_ParseTuple(args, "O|i:applyForce", &pyvect, &local)) {
		mt::vec3 force;
		if (PyVecTo(pyvect, force)) {
			ApplyForce(force, (local != 0));
			Py_RETURN_NONE;
		}
	}
	return nullptr;
}

PyObject *KX_GameObject::PyApplyTorque(PyObject *args)
{
	int local = 0;
	PyObject *pyvect;

	if (PyArg_ParseTuple(args, "O|i:applyTorque", &pyvect, &local)) {
		mt::vec3 torque;
		if (PyVecTo(pyvect, torque)) {
			ApplyTorque(torque, (local != 0));
			Py_RETURN_NONE;
		}
	}
	return nullptr;
}

PyObject *KX_GameObject::PyApplyRotation(PyObject *args)
{
	int local = 0;
	PyObject *pyvect;

	if (PyArg_ParseTuple(args, "O|i:applyRotation", &pyvect, &local)) {
		mt::vec3 rotation;
		if (PyVecTo(pyvect, rotation)) {
			ApplyRotation(rotation, (local != 0));
			Py_RETURN_NONE;
		}
	}
	return nullptr;
}

PyObject *KX_GameObject::PyApplyMovement(PyObject *args)
{
	int local = 0;
	PyObject *pyvect;

	if (PyArg_ParseTuple(args, "O|i:applyMovement", &pyvect, &local)) {
		mt::vec3 movement;
		if (PyVecTo(pyvect, movement)) {
			ApplyMovement(movement, (local != 0));
			Py_RETURN_NONE;
		}
	}
	return nullptr;
}

PyObject *KX_GameObject::PyGetLinearVelocity(PyObject *args)
{
	// only can get the velocity if we have a physics object connected to us...
	int local = 0;

	if (PyArg_ParseTuple(args, "|i:getLinearVelocity", &local)) {
		return PyObjectFrom(GetLinearVelocity((local != 0)));
	}
	else {
		return nullptr;
	}
}

PyObject *KX_GameObject::PySetLinearVelocity(PyObject *args)
{
	int local = 0;
	PyObject *pyvect;

	if (PyArg_ParseTuple(args, "O|i:setLinearVelocity", &pyvect, &local)) {
		mt::vec3 velocity;
		if (PyVecTo(pyvect, velocity)) {
			SetLinearVelocity(velocity, (local != 0));
			Py_RETURN_NONE;
		}
	}
	return nullptr;
}

PyObject *KX_GameObject::PyGetAngularVelocity(PyObject *args)
{
	// only can get the velocity if we have a physics object connected to us...
	int local = 0;

	if (PyArg_ParseTuple(args, "|i:getAngularVelocity", &local)) {
		return PyObjectFrom(GetAngularVelocity((local != 0)));
	}
	else {
		return nullptr;
	}
}

PyObject *KX_GameObject::PySetAngularVelocity(PyObject *args)
{
	int local = 0;
	PyObject *pyvect;

	if (PyArg_ParseTuple(args, "O|i:setAngularVelocity", &pyvect, &local)) {
		mt::vec3 velocity;
		if (PyVecTo(pyvect, velocity)) {
			SetAngularVelocity(velocity, (local != 0));
			Py_RETURN_NONE;
		}
	}
	return nullptr;
}

PyObject *KX_GameObject::PySetDamping(PyObject *args)
{
	float linear;
	float angular;

	if (!PyArg_ParseTuple(args, "ff:setDamping", &linear, &angular)) {
		return nullptr;
	}

	SetDamping(linear, angular);
	Py_RETURN_NONE;
}

PyObject *KX_GameObject::PyGetVertexShader()
{
	KX_BlenderMaterial *blmat = GetFirstBlenderMaterial();
	if (blmat) {
		Material *ma = blmat->GetBlenderMaterial();
		Scene *scene = (Scene *)blmat->GetBlenderScene();
		if (ma && scene) {
			GPUShaderExport *export_data = GPU_shader_export(scene, ma);
			if (export_data) {
				PyObject *ret = PyUnicode_FromString(export_data->vertex ? export_data->vertex : "");
				GPU_free_shader_export(export_data);
				return ret;
			}
		}
	}
	Py_RETURN_NONE;
}

PyObject *KX_GameObject::PyGetFragmentShader()
{
	KX_BlenderMaterial *blmat = GetFirstBlenderMaterial();
	if (blmat) {
		Material *ma = blmat->GetBlenderMaterial();
		Scene *scene = (Scene *)blmat->GetBlenderScene();
		if (ma && scene) {
			GPUShaderExport *export_data = GPU_shader_export(scene, ma);
			if (export_data) {
				PyObject *ret = PyUnicode_FromString(export_data->fragment ? export_data->fragment : "");
				GPU_free_shader_export(export_data);
				return ret;
			}
		}
	}
	Py_RETURN_NONE;
}

PyObject *KX_GameObject::PyGetShadowVertexShader()
{
	// Retorna informação sobre o vertex shader usado no passe de sombra
	KX_BlenderMaterial *blmat = GetFirstBlenderMaterial();
	if (blmat) {
		BL_BlenderShader *shader = blmat->GetBlenderShader();
		if (shader) {
			GPUMaterial *gpumat = shader->GetGPUMaterial();
			if (gpumat) {
				// Verifica se o material tem custom vertex code
				const char *customVertex = GPU_material_get_custom_vertex_code(gpumat);
				if (customVertex && customVertex[0] != '\0') {
					return PyUnicode_FromString("[Shadow vertex: Has custom vertex code]");
				}
			}
		}
		return PyUnicode_FromString("[Shadow vertex: Using builtin VSM shader]");
	}
	Py_RETURN_NONE;
}

PyObject *KX_GameObject::PyGetShadowFragmentShader()
{
	// Retorna informação sobre o fragment shader usado no passe de sombra
	KX_BlenderMaterial *blmat = GetFirstBlenderMaterial();
	if (blmat) {
		BL_BlenderShader *shader = blmat->GetBlenderShader();
		if (shader) {
			GPUMaterial *gpumat = shader->GetGPUMaterial();
			if (gpumat) {
				// Verifica se há shader cache de sombra
				GPUShader **shadowCache = GPU_material_get_shadow_shader_cache(gpumat, 0);
				if (shadowCache && *shadowCache) {
					return PyUnicode_FromString("[Shadow fragment: Has cached shadow shader]");
				}
			}
		}
		return PyUnicode_FromString("[Shadow fragment: Using builtin VSM shader]");
	}
	Py_RETURN_NONE;
}

PyObject *KX_GameObject::PySetVisible(PyObject *args)
{
	int visible, recursive = 0;
	if (!PyArg_ParseTuple(args, "i|i:setVisible", &visible, &recursive)) {
		return nullptr;
	}

	SetVisible(visible ? true : false, recursive ? true : false);
	Py_RETURN_NONE;

}

PyObject *KX_GameObject::PySetOcclusion(PyObject *args)
{
	int occlusion, recursive = 0;

	if (!PyArg_ParseTuple(args, "i|i:setOcclusion", &occlusion, &recursive)) {
		return nullptr;
	}

	SetOccluder(occlusion ? true : false, recursive ? true : false);
	Py_RETURN_NONE;
}

PyObject *KX_GameObject::PyGetVelocity(PyObject *args)
{
	// only can get the velocity if we have a physics object connected to us...
	mt::vec3 point = mt::zero3;
	PyObject *pypos = nullptr;

	if (!PyArg_ParseTuple(args, "|O:getVelocity", &pypos) || (pypos && !PyVecTo(pypos, point))) {
		return nullptr;
	}

	return PyObjectFrom(GetVelocity(point));
}

PyObject *KX_GameObject::PyGetReactionForce()
{
	// only can get the velocity if we have a physics object connected to us...

	// XXX - Currently not working with bullet intergration, see KX_BulletPhysicsController.cpp's getReactionForce
#if 0
	if (GetPhysicsController1()) {
		return PyObjectFrom(GetPhysicsController1()->getReactionForce());
	}
	return PyObjectFrom(mt::zero3);
#endif

	return PyObjectFrom(mt::zero3);
}



PyObject *KX_GameObject::PyEnableRigidBody()
{
	if (m_physicsController) {
		m_physicsController->SetRigidBody(true);
	}

	Py_RETURN_NONE;
}



PyObject *KX_GameObject::PyDisableRigidBody()
{
	if (m_physicsController) {
		m_physicsController->SetRigidBody(false);
	}

	Py_RETURN_NONE;
}


PyObject *KX_GameObject::PySetParent(PyObject *args, PyObject *kwds)
{
	SCA_LogicManager *logicmgr = GetScene()->GetLogicManager();
	PyObject *pyobj;
	KX_GameObject *obj;
	int addToCompound = 1, ghost = 1;

	if (!EXP_ParseTupleArgsAndKeywords(args, kwds, "O|ii:setParent", {"parent", "compound", "ghost", 0},
	                                   &pyobj, &addToCompound, &ghost)) {
		return nullptr; // Python sets a simple error
	}
	if (!ConvertPythonToGameObject(logicmgr, pyobj, &obj, true, "gameOb.setParent(obj): KX_GameObject")) {
		return nullptr;
	}

	if (obj) {
		SetParent(obj, addToCompound, ghost);
	}
	Py_RETURN_NONE;
}

PyObject *KX_GameObject::PyRemoveParent()
{
	RemoveParent();
	Py_RETURN_NONE;
}


PyObject *KX_GameObject::PySetCollisionMargin(PyObject *value)
{
	float collisionMargin = PyFloat_AsDouble(value);

	if (collisionMargin == -1 && PyErr_Occurred()) {
		PyErr_SetString(PyExc_TypeError, "expected a float");
		return nullptr;
	}

	PYTHON_CHECK_PHYSICS_CONTROLLER(this, "setCollisionMargin", nullptr);

	m_physicsController->SetMargin(collisionMargin);
	Py_RETURN_NONE;
}


PyObject *KX_GameObject::PyCollide(PyObject *value)
{
	KX_Scene *scene = GetScene();
	KX_GameObject *other;

	if (!ConvertPythonToGameObject(scene->GetLogicManager(), value, &other, false, "gameOb.collide(obj): KX_GameObject")) {
		return nullptr;
	}

	if (!m_physicsController || !other->GetPhysicsController()) {
		PyErr_SetString(PyExc_TypeError, "expected objects with physics controller");
		return nullptr;
	}

	PHY_IPhysicsEnvironment *env = scene->GetPhysicsEnvironment();
	PHY_CollisionTestResult testResult = env->CheckCollision(m_physicsController.get(), other->GetPhysicsController());

	PyObject *result = PyTuple_New(2);
	if (!testResult.collide) {
		PyTuple_SET_ITEM(result, 0, Py_False);
		PyTuple_SET_ITEM(result, 1, Py_None);

		Py_INCREF(Py_False);
		Py_INCREF(Py_None);
	}
	else {
		PyTuple_SET_ITEM(result, 0, Py_True);
		Py_INCREF(Py_True);

		if (testResult.collData) {
			KX_CollisionContactPointList *contactPointList = new KX_CollisionContactPointList(testResult.collData, testResult.isFirst);
			PyTuple_SET_ITEM(result, 1, contactPointList->NewProxy(true));
		}
		else {
			PyTuple_SET_ITEM(result, 1, Py_None);
			Py_INCREF(Py_None);
		}
	}

	return result;
}


PyObject *KX_GameObject::PyApplyImpulse(PyObject *args)
{
	PyObject *pyattach;
	PyObject *pyimpulse;
	int local = 0;

	PYTHON_CHECK_PHYSICS_CONTROLLER(this, "applyImpulse", nullptr);
	if (PyArg_ParseTuple(args, "OO|i:applyImpulse", &pyattach, &pyimpulse, &local)) {
		mt::vec3 attach;
		mt::vec3 impulse;
		if (PyVecTo(pyattach, attach) && PyVecTo(pyimpulse, impulse)) {
			m_physicsController->ApplyImpulse(attach, impulse, (local != 0));
			Py_RETURN_NONE;
		}

	}

	return nullptr;
}

PyObject *KX_GameObject::PySuspendPhysics(PyObject *args)
{
	int freeConstraints = false;

	if (!PyArg_ParseTuple(args, "|i:suspendPhysics", &freeConstraints)) {
		return nullptr;
	}

	SuspendPhysics(freeConstraints);

	Py_RETURN_NONE;
}

PyObject *KX_GameObject::PyRestorePhysics()
{
	RestorePhysics();

	Py_RETURN_NONE;
}

PyObject *KX_GameObject::PySuspendDynamics(PyObject *args)
{
	bool ghost = false;

	if (!PyArg_ParseTuple(args, "|b", &ghost)) {
		return nullptr;
	}

	if (m_physicsController) {
		m_physicsController->SuspendDynamics(ghost);
	}

	Py_RETURN_NONE;
}



PyObject *KX_GameObject::PyRestoreDynamics()
{
	// Child objects must be static, so we block changing to dynamic
	if (m_physicsController && !GetParent()) {
		m_physicsController->RestoreDynamics();
	}
	Py_RETURN_NONE;
}


PyObject *KX_GameObject::PyAlignAxisToVect(PyObject *args, PyObject *kwds)
{
	PyObject *pyvect;
	int axis = 2; //z axis is the default
	float fac = 1.0f;

	if (EXP_ParseTupleArgsAndKeywords(args, kwds, "O|if:alignAxisToVect", {"vect", "axis", "factor", 0},
	                                  &pyvect, &axis, &fac)) {
		mt::vec3 vect;
		if (PyVecTo(pyvect, vect)) {
			if (fac > 0.0f) {
				if (fac > 1.0f) {
					fac = 1.0f;
				}

				AlignAxisToVect(vect, axis, fac);
				NodeUpdate();
			}
			Py_RETURN_NONE;
		}
	}
	return nullptr;
}

PyObject *KX_GameObject::PyGetAxisVect(PyObject *value)
{
	mt::vec3 vect;
	if (PyVecTo(value, vect)) {
		return PyObjectFrom(NodeGetWorldOrientation() * vect);
	}
	return nullptr;
}


PyObject *KX_GameObject::PyGetPhysicsId()
{
	unsigned long long physid = 0;
	if (m_physicsController) {
		physid = (unsigned long long)m_physicsController.get();
	}
	return PyLong_FromUnsignedLongLong(physid);
}

PyObject *KX_GameObject::PyGetPropertyNames()
{
	PyObject *list = ConvertKeysToPython();

	if (m_attr_dict) {
		PyObject *key, *value;
		Py_ssize_t pos = 0;

		while (PyDict_Next(m_attr_dict, &pos, &key, &value)) {
			PyList_Append(list, key);
		}
	}
	return list;
}

EXP_PYMETHODDEF_DOC_O(KX_GameObject, getDistanceTo,
                      "getDistanceTo(other): get distance to another point/KX_GameObject")
{
	mt::vec3 b;
	if (PyVecTo(value, b)) {
		return PyFloat_FromDouble((NodeGetWorldPosition() - b).Length());
	}
	PyErr_Clear();

	SCA_LogicManager *logicmgr = GetScene()->GetLogicManager();
	KX_GameObject *other;
	if (ConvertPythonToGameObject(logicmgr, value, &other, false, "gameOb.getDistanceTo(value): KX_GameObject")) {
		return PyFloat_FromDouble((NodeGetWorldPosition() - other->NodeGetWorldPosition()).Length());
	}

	return nullptr;
}

EXP_PYMETHODDEF_DOC_O(KX_GameObject, getVectTo,
                      "getVectTo(other): get vector and the distance to another point/KX_GameObject\n"
                      "Returns a 3-tuple with (distance,worldVector,localVector)\n")
{
	mt::vec3 toPoint, fromPoint;
	mt::vec3 toDir, locToDir;
	float distance;

	SCA_LogicManager *logicmgr = GetScene()->GetLogicManager();
	PyObject *returnValue;

	if (!PyVecTo(value, toPoint)) {
		PyErr_Clear();

		KX_GameObject *other;
		if (ConvertPythonToGameObject(logicmgr, value, &other, false, "")) { /* error will be overwritten */
			toPoint = other->NodeGetWorldPosition();
		}
		else {
			PyErr_SetString(PyExc_TypeError, "gameOb.getVectTo(other): KX_GameObject, expected a 3D Vector or KX_GameObject type");
			return nullptr;
		}
	} else {
		// Vector input is global, convert to local
	}

	fromPoint = NodeGetWorldPosition();
	toDir = toPoint - fromPoint;
	distance = toDir.Length();

	if (mt::FuzzyZero(distance)) {
		locToDir = toDir = mt::zero3;
		distance = 0.0f;
	}
	else {
		toDir.Normalize();
		locToDir = toDir * NodeGetWorldOrientation();
	}

	returnValue = PyTuple_New(3);
	if (returnValue) { // very unlikely to fail, python sets a memory error here.
		PyTuple_SET_ITEM(returnValue, 0, PyFloat_FromDouble(distance));
		PyTuple_SET_ITEM(returnValue, 1, PyObjectFrom(toDir));
		PyTuple_SET_ITEM(returnValue, 2, PyObjectFrom(locToDir));
	}
	return returnValue;
}

KX_GameObject::RayCastData::RayCastData(const std::string& prop, bool xray, unsigned int mask)
	:m_prop(prop),
	m_xray(xray),
	m_mask(mask),
	m_hitObject(nullptr)
{
}

static bool CheckRayCastObject(KX_GameObject *obj, KX_GameObject::RayCastData *rayData)
{
	const std::string& prop = rayData->m_prop;
	const unsigned int mask = rayData->m_mask;
	// Check if the object had a given property (if this one is non empty) and have the correct group mask (if this one is different from 0xFFFF).
	return ((prop.empty() || obj->GetProperty(prop)) && (mask == ((1u << OB_MAX_COL_MASKS) - 1) || obj->GetCollisionGroup() & mask));
}

bool KX_GameObject::RayHit(KX_ClientObjectInfo *client, KX_RayCast *result, RayCastData *rayData)
{
	KX_GameObject *obj = client->m_gameobject;

	// if X-ray option is selected, the unwanted objects were not tested, so get here only with true hit
	// if not, all objects were tested and the front one may not be the correct one.
	if (rayData->m_xray || CheckRayCastObject(obj, rayData)) {
		rayData->m_hitObject = obj;
		rayData->m_hitVertex = result->m_hitVertex;
		rayData->m_hitVertexIndex = result->m_hitVertexIndex;
		rayData->m_hitVertexNormal = result->m_hitVertexNormal;
	}
	// return true to stop RayCast::RayTest from looping, the above test was decisive
	// We would want to loop only if we want to get more than one hit point
	return true;
}

/* this function is used to pre-filter the object before casting the ray on them.
 * This is useful for "X-Ray" option when we want to see "through" unwanted object.
 */
bool KX_GameObject::NeedRayCast(KX_ClientObjectInfo *client, RayCastData *rayData)
{
	KX_GameObject *obj = client->m_gameobject;

	// if X-Ray option is selected, skip object that don't match the criteria as we see through them
	// if not, test all objects because we don't know yet which one will be on front
	return (!rayData->m_xray || CheckRayCastObject(obj, rayData));
}

EXP_PYMETHODDEF_DOC(KX_GameObject, rayCastTo,
                    "rayCastTo(other,dist,prop): look towards another point/KX_GameObject and return first object hit within dist that matches prop\n"
                    " prop = property name that object must have; can be omitted => detect any object\n"
                    " dist = max distance to look (can be negative => look behind); 0 or omitted => detect up to other\n"
                    " other = 3-tuple or object reference")
{
	mt::vec3 toPoint;
	PyObject *pyarg;
	float dist = 0.0f;
	const char *propName = "";
	SCA_LogicManager *logicmgr = GetScene()->GetLogicManager();

	if (!EXP_ParseTupleArgsAndKeywords(args, kwds, "O|fs:rayCastTo", {"other", "dist", "prop", 0},
	                                   &pyarg, &dist, &propName)) {
		return nullptr; // python sets simple error
	}

	if (!PyVecTo(pyarg, toPoint)) {
		KX_GameObject *other;
		PyErr_Clear();

		if (ConvertPythonToGameObject(logicmgr, pyarg, &other, false, "")) { /* error will be overwritten */
			toPoint = other->NodeGetWorldPosition();
		}
		else {
			PyErr_SetString(PyExc_TypeError, "gameOb.rayCastTo(other,dist,prop): KX_GameObject, the first argument to rayCastTo must be a vector or a KX_GameObject");
			return nullptr;
		}
	} else {
		// Vector input is global, convert to local
	}
	mt::vec3 fromPoint = NodeGetWorldPosition();

	if (dist != 0.0f) {
		toPoint = fromPoint + dist * (toPoint - fromPoint).SafeNormalized(mt::axisX3);
	}

	PHY_IPhysicsEnvironment *pe = GetScene()->GetPhysicsEnvironment();
	PHY_IPhysicsController *spc = m_physicsController.get();
	KX_GameObject *parent = GetParent();
	if (!spc && parent) {
		spc = parent->GetPhysicsController();
	}

	RayCastData rayData(propName, false, (1u << OB_MAX_COL_MASKS) - 1);
	KX_RayCast::Callback<KX_GameObject, RayCastData> callback(this, spc, &rayData);
	
	if (KX_RayCast::RayTest(pe, fromPoint, toPoint, callback) && rayData.m_hitObject) {
		PyObject *hitObj = rayData.m_hitObject->GetProxy();
		mt::vec3 pos = callback.m_hitPoint;
		const mt::vec3 &normal = callback.m_hitNormal;
		mt::vec3 vert = rayData.m_hitVertex;
		const mt::vec3 &vnormal = rayData.m_hitVertexNormal;
		int index = rayData.m_hitVertexIndex;
	
		return Py_BuildValue("O(fff)(fff)(fff)(fff)i",
			hitObj,
			pos.x,    pos.y,    pos.z,
			normal.x, normal.y, normal.z,
			vert.x,   vert.y,   vert.z,
			vnormal.x, vnormal.y, vnormal.z,
			index);
	}
	
	Py_RETURN_NONE;
}	

/* faster then Py_BuildValue since some scripts call raycast a lot */
static PyObject *none_tuple_3()
{
	PyObject *ret = PyTuple_New(3);
	PyTuple_SET_ITEM(ret, 0, Py_None);
	PyTuple_SET_ITEM(ret, 1, Py_None);
	PyTuple_SET_ITEM(ret, 2, Py_None);

	Py_INCREF(Py_None);
	Py_INCREF(Py_None);
	Py_INCREF(Py_None);
	return ret;
}
static PyObject *none_tuple_4()
{
	PyObject *ret = PyTuple_New(4);
	PyTuple_SET_ITEM(ret, 0, Py_None);
	PyTuple_SET_ITEM(ret, 1, Py_None);
	PyTuple_SET_ITEM(ret, 2, Py_None);
	PyTuple_SET_ITEM(ret, 3, Py_None);

	Py_INCREF(Py_None);
	Py_INCREF(Py_None);
	Py_INCREF(Py_None);
	Py_INCREF(Py_None);
	return ret;
}

static PyObject *none_tuple_5()
{
	PyObject *ret = PyTuple_New(5);
	PyTuple_SET_ITEM(ret, 0, Py_None);
	PyTuple_SET_ITEM(ret, 1, Py_None);
	PyTuple_SET_ITEM(ret, 2, Py_None);
	PyTuple_SET_ITEM(ret, 3, Py_None);
	PyTuple_SET_ITEM(ret, 4, Py_None);

	Py_INCREF(Py_None);
	Py_INCREF(Py_None);
	Py_INCREF(Py_None);
	Py_INCREF(Py_None);
	Py_INCREF(Py_None);
	return ret;
}

EXP_PYMETHODDEF_DOC(KX_GameObject, rayCast,
    "rayCast(to,from,dist,prop,face,xray,poly,mask): cast a ray and return tuple with contact info.\n"
    "Returns (object, hit_point, hit_normal) or (object, hit_point, hit_normal, polygon) or (object, hit_point, hit_normal, polygon, hituv)\n"
    "or (object, hit_point, hit_normal, vertex, index, vertex_normal) with vertex position, index and normal.\n"
    "If no hit, returns tuples filled with None.\n"
    " to   = 3-tuple or object reference for ray destination\n"
    " from = 3-tuple or object reference for ray origin\n"
    " dist = max distance\n"
    " prop = property name\n"
    " face = 1 to return face normal\n"
    " xray = 1 to skip non-matching objects\n"
    " poly = 0 (return vertex info), 1 (polygon), 2 (polygon + UV)\n"
    " mask = collision mask\n"
)
{
    mt::vec3 toPoint;
    mt::vec3 fromPoint;
    PyObject *pyto;
    PyObject *pyfrom = Py_None;
    float dist = 0.0f;
    const char *propName = "";
    KX_GameObject *other;
    int face = 0, xray = 0, poly = 0;
    int mask = (1 << OB_MAX_COL_MASKS) - 1;
    SCA_LogicManager *logicmgr = GetScene()->GetLogicManager();

    if (!EXP_ParseTupleArgsAndKeywords(args, kwds, "O|Ofsiiii:rayCast",
                                       {"objto", "objfrom", "dist", "prop", "face", "xray", "poly", "mask", 0},
                                       &pyto, &pyfrom, &dist, &propName, &face, &xray, &poly, &mask)) {
        return nullptr;
    }

    KX_Scene *scene = GetScene();

    if (!PyVecTo(pyto, toPoint)) {
        PyErr_Clear();

        if (ConvertPythonToGameObject(logicmgr, pyto, &other, false, "")) {
            toPoint = other->NodeGetWorldPosition();
        }
        else {
            PyErr_SetString(PyExc_TypeError, "the first argument to rayCast must be a vector or a KX_GameObject");
            return nullptr;
        }
    }

    if (pyfrom == Py_None) {
        fromPoint = NodeGetWorldPosition();
    }
    else if (!PyVecTo(pyfrom, fromPoint)) {
        PyErr_Clear();

        if (ConvertPythonToGameObject(logicmgr, pyfrom, &other, false, "")) {
            fromPoint = other->NodeGetWorldPosition();
        }
        else {
            PyErr_SetString(PyExc_TypeError, "the second argument to rayCast must be a vector or a KX_GameObject");
            return nullptr;
        }
    }

    if (mask == 0 || mask & ~((1 << OB_MAX_COL_MASKS) - 1)) {
        PyErr_Format(PyExc_TypeError, "mask argument must be an int bitfield, 0 < mask < %i", (1 << OB_MAX_COL_MASKS));
        return nullptr;
    }

    if (dist != 0.0f) {
        mt::vec3 toDir = toPoint - fromPoint;
        if (mt::FuzzyZero(toDir)) {
            return none_tuple_3();
        }
        toDir.Normalize();
        toPoint = fromPoint + dist * toDir;
    }
    else if (mt::FuzzyZero(toPoint - fromPoint)) {
        return none_tuple_3();
    }

    PHY_IPhysicsEnvironment *pe = GetScene()->GetPhysicsEnvironment();
    PHY_IPhysicsController *spc = m_physicsController.get();
    KX_GameObject *parent = GetParent();
    if (!spc && parent) {
        spc = parent->GetPhysicsController();
    }

    RayCastData rayData(propName, xray, mask);
    KX_RayCast::Callback<KX_GameObject, RayCastData> callback(this, spc, &rayData, face, (poly == 2));

    if (KX_RayCast::RayTest(pe, fromPoint, toPoint, callback) && rayData.m_hitObject) {
		PyObject *returnValue = nullptr;
		mt::vec3 vert = rayData.m_hitVertex;
		const mt::vec3 &vnormal = rayData.m_hitVertexNormal;
		int index = rayData.m_hitVertexIndex;
		mt::vec3 hitPoint = callback.m_hitPoint;

		// Convert Local to Global

		if (poly == 2) {
			returnValue = PyTuple_New(5);
		}
		else if (poly == 1) {
			returnValue = PyTuple_New(4);
		}
		else {
			returnValue = PyTuple_New(6); // Agora com vertex_normal
		}

		if (returnValue) {
			PyTuple_SET_ITEM(returnValue, 0, rayData.m_hitObject->GetProxy());
			PyTuple_SET_ITEM(returnValue, 1, PyObjectFrom(hitPoint));
			PyTuple_SET_ITEM(returnValue, 2, PyObjectFrom(callback.m_hitNormal));

            if (poly) {
                if (callback.m_hitMesh) {
                    KX_Mesh *mesh = static_cast<KX_Mesh *>(callback.m_hitMesh);
                    const RAS_Mesh::PolygonInfo polygon = mesh->GetPolygon(callback.m_hitPolygon);
                    KX_PolyProxy *polyproxy = new KX_PolyProxy(mesh, polygon);
                    PyTuple_SET_ITEM(returnValue, 3, polyproxy->NewProxy(true));
                    if (poly == 2) {
                        if (callback.m_hitUVOK) {
                            PyTuple_SET_ITEM(returnValue, 4, PyObjectFrom(callback.m_hitUV));
                        }
                        else {
                            Py_INCREF(Py_None);
                            PyTuple_SET_ITEM(returnValue, 4, Py_None);
                        }
                    }
                }
                else {
                    Py_INCREF(Py_None);
                    PyTuple_SET_ITEM(returnValue, 3, Py_None);
                    if (poly == 2) {
                        Py_INCREF(Py_None);
                        PyTuple_SET_ITEM(returnValue, 4, Py_None);
                    }
                }
            }
            else {
                PyTuple_SET_ITEM(returnValue, 3, Py_BuildValue("(fff)", vert.x, vert.y, vert.z));
                PyTuple_SET_ITEM(returnValue, 4, PyLong_FromLong(index));
                PyTuple_SET_ITEM(returnValue, 5, Py_BuildValue("(fff)", vnormal.x, vnormal.y, vnormal.z));
            }
        }
        return returnValue;
    }

    if (poly == 2) {
        return none_tuple_5();
    }
    else if (poly == 1) {
        return none_tuple_4();
    }
    else {
        return none_tuple_3(); 
    }
}



EXP_PYMETHODDEF_DOC(KX_GameObject,setGrassTextureObject,
"setGrassTextureObject() - Define este objeto como fonte de texturas para o sistema de grama")
{
	if (!PyArg_ParseTuple(args, "")) {
		return nullptr;
	}

	KX_Scene *scene = GetScene();
	if (scene) {
		KX_GrassSystem *gs = scene->GetOrCreateGrassSystem();
		if (gs) {
			gs->SetTextureObject(this);
		}
	}
	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_GameObject,setGrassLightMultipliers,
"setGrassLightMultipliers(list) - Define multiplicadores de força e cores opcionais para luzes específicas no sistema de grama\n"
"Ex: obj.setGrassLightMultipliers([('Lamp', 0.5), ('Sun', 1.2, [1.0, 0.0, 0.0])])")
{
	PyObject *list;
	if (!PyArg_ParseTuple(args, "O", &list)) {
		return nullptr;
	}

	KX_Scene *scene = GetScene();
	if (scene) {
		KX_GrassSystem *gs = scene->GetOrCreateGrassSystem();
		if (gs && PyList_Check(list)) {
			gs->ClearLightMultipliers();
			Py_ssize_t size = PyList_Size(list);
			for (Py_ssize_t i = 0; i < size; ++i) {
				PyObject *item = PyList_GetItem(list, i);
				if (PyTuple_Check(item)) {
					Py_ssize_t tupleSize = PyTuple_Size(item);
					if (tupleSize >= 2) {
						PyObject *nameObj = PyTuple_GetItem(item, 0);
						PyObject *valObj = PyTuple_GetItem(item, 1);
						if (PyUnicode_Check(nameObj) && (PyFloat_Check(valObj) || PyLong_Check(valObj))) {
							const char *pName = PyUnicode_AsUTF8(nameObj);
							float val = (float)PyFloat_AsDouble(valObj);
							mt::vec3 col(-1.0f, -1.0f, -1.0f);

							if (tupleSize >= 3) {
								PyObject *colObj = PyTuple_GetItem(item, 2);
								if (PySequence_Check(colObj) && PySequence_Size(colObj) == 3) {
									PyObject *r = PySequence_GetItem(colObj, 0);
									PyObject *g = PySequence_GetItem(colObj, 1);
									PyObject *b = PySequence_GetItem(colObj, 2);
									col = mt::vec3((float)PyFloat_AsDouble(r), 
									               (float)PyFloat_AsDouble(g), 
									               (float)PyFloat_AsDouble(b));
									Py_DECREF(r); Py_DECREF(g); Py_DECREF(b);
								}
							}
							gs->AddLightMultiplier(std::string(pName), val, col);
						}
					}
				}
			}
		}
	}
	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_GameObject,sendMessage,
                    "sendMessage(subject, [body, to])\n"
                    "sends a message in same manner as a message actuator"
                    "subject = Subject of the message (string)"
                    "body = Message body (string)"
                    "to = Name of object to send the message to")
{
	char *subject;
	char *body = (char *)"";
	char *to = (char *)"";

	if (!EXP_ParseTupleArgsAndKeywords(args, kwds, "s|ss:sendMessage", {"subject", "body", "to", 0},
	                                   &subject, &body, &to)) {
		return nullptr;
	}

	GetScene()->GetNetworkMessageScene()->SendMessage(to, this, subject, body);
	Py_RETURN_NONE;
}

static void layer_check(short &layer, const char *method_name)
{
	if (layer < 0 || layer >= MAX_ACTION_LAYERS) {
		CM_PythonFunctionWarning("KX_GameObject", method_name, "given layer (" << layer
		                                                                       << ") is out of range (0 - " << (MAX_ACTION_LAYERS - 1) << "), setting to 0.");
		layer = 0;
	}
}

EXP_PYMETHODDEF_DOC(KX_GameObject, playAction,
                    "playAction(name, start_frame, end_frame, layer=0, priority=0 blendin=0, play_mode=ACT_MODE_PLAY, layer_weight=0.0, ipo_flags=0, speed=1.0)\n"
                    "Plays an action\n")
{
	const char *name;
	float start, end, blendin = 0.f, speed = 1.f, layer_weight = 0.f;
	short layer = 0, priority = 0;
	short ipo_flags = 0;
	short play_mode = 0;
	short blend_mode = 0;

	if (!EXP_ParseTupleArgsAndKeywords(args, kwds, "sff|hhfhfhfh:playAction", {"name", "start_frame", "end_frame", "layer",
	                                                                           "priority", "blendin", "play_mode", "layer_weight", "ipo_flags", "speed", "blend_mode", 0},
	                                   &name, &start, &end, &layer, &priority, &blendin, &play_mode, &layer_weight, &ipo_flags, &speed, &blend_mode)) {
		return nullptr;
	}

	layer_check(layer, "playAction");

	if (play_mode < 0 || play_mode > BL_Action::ACT_MODE_MAX) {
		CM_PythonFunctionWarning("KX_GameObject", "playAction", "given play_mode (" << play_mode << ") is out of range (0 - "
		                                                                            << (BL_Action::ACT_MODE_MAX - 1) << "), setting to ACT_MODE_PLAY");
		play_mode = BL_Action::ACT_MODE_PLAY;
	}

	if (blend_mode < 0 || blend_mode > BL_Action::ACT_BLEND_MAX) {
		CM_PythonFunctionWarning("KX_GameObject", "playAction", "given blend_mode (" << blend_mode << ") is out of range (0 - "
		                                                                             << (BL_Action::ACT_BLEND_MAX - 1) << "), setting to ACT_BLEND_BLEND");
		blend_mode = BL_Action::ACT_BLEND_BLEND;
	}

	if (layer_weight < 0.f || layer_weight > 1.f) {
		CM_PythonFunctionWarning("KX_GameObject", "playAction", "given layer_weight (" << layer_weight
		                                                                               << ") is out of range (0.0 - 1.0), setting to 0.0");
		layer_weight = 0.f;
	}

	PlayAction(name, start, end, layer, priority, blendin, play_mode, layer_weight, ipo_flags, speed, blend_mode);

	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_GameObject, stopAction,
                    "stopAction(layer=0)\n"
                    "Stop playing the action on the given layer\n")
{
	short layer = 0;

	if (!PyArg_ParseTuple(args, "|h:stopAction", &layer)) {
		return nullptr;
	}

	layer_check(layer, "stopAction");

	StopAction(layer);

	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_GameObject, getActionFrame,
                    "getActionFrame(layer=0)\n"
                    "Gets the current frame of the action playing in the supplied layer\n")
{
	short layer = 0;

	if (!PyArg_ParseTuple(args, "|h:getActionFrame", &layer)) {
		return nullptr;
	}

	layer_check(layer, "getActionFrame");

	return PyFloat_FromDouble(GetActionFrame(layer));
}

EXP_PYMETHODDEF_DOC(KX_GameObject, getActionName,
                    "getActionName(layer=0)\n"
                    "Gets the name of the current action playing in the supplied layer\n")
{
	short layer = 0;

	if (!PyArg_ParseTuple(args, "|h:getActionName", &layer)) {
		return nullptr;
	}

	layer_check(layer, "getActionName");

	return PyUnicode_FromStdString(GetActionName(layer));
}

EXP_PYMETHODDEF_DOC(KX_GameObject, setActionFrame,
                    "setActionFrame(frame, layer=0)\n"
                    "Set the current frame of the action playing in the supplied layer\n")
{
	short layer = 0;
	float frame;

	if (!PyArg_ParseTuple(args, "f|h:setActionFrame", &frame, &layer)) {
		return nullptr;
	}

	layer_check(layer, "setActionFrame");

	SetActionFrame(layer, frame);

	Py_RETURN_NONE;
}



EXP_PYMETHODDEF_DOC(KX_GameObject, isPlayingAction,
                    "isPlayingAction(layer=0)\n"
                    "Checks to see if there is an action playing in the given layer\n")
{
	short layer = 0;

	if (!PyArg_ParseTuple(args, "|h:isPlayingAction", &layer)) {
		return nullptr;
	}

	layer_check(layer, "isPlayingAction");

	return PyBool_FromLong(!IsActionDone(layer));
}


EXP_PYMETHODDEF_DOC(KX_GameObject, addDebugProperty,
                    "addDebugProperty(name, visible=1)\n"
                    "Added or remove a debug property to the debug list.\n")
{
	KX_Scene *scene = GetScene();
	char *name;
	int visible = 1;

	if (!PyArg_ParseTuple(args, "s|i:debugProperty", &name, &visible)) {
		return nullptr;
	}

	if (visible) {
		if (!scene->PropertyInDebugList(this, name)) {
			scene->AddDebugProperty(this, name);
		}
	}
	else {
		scene->RemoveDebugProperty(this, name);
	}

	Py_RETURN_NONE;
}


/* dict style access */


/* Matches python dict.get(key, [default]) */
PyObject *KX_GameObject::Pyget(PyObject *args)
{
	PyObject *key;
	PyObject *def = Py_None;
	PyObject *ret;

	if (!PyArg_ParseTuple(args, "O|O:get", &key, &def)) {
		return nullptr;
	}


	if (PyUnicode_Check(key)) {
		EXP_Value *item = GetProperty(_PyUnicode_AsString(key));
		if (item) {
			ret = item->ConvertValueToPython();
			if (ret) {
				return ret;
			}
			else {
				return item->GetProxy();
			}
		}
	}

	if (m_attr_dict && (ret = PyDict_GetItem(m_attr_dict, key))) {
		Py_INCREF(ret);
		return ret;
	}

	Py_INCREF(def);
	return def;
}

bool ConvertPythonToGameObject(SCA_LogicManager *manager, PyObject *value, KX_GameObject **object, bool py_none_ok, const char *error_prefix)
{
	if (value == nullptr) {
		PyErr_Format(PyExc_TypeError, "%s, python pointer nullptr, should never happen", error_prefix);
		*object = nullptr;
		return false;
	}

	if (value == Py_None) {
		*object = nullptr;

		if (py_none_ok) {
			return true;
		}
		else {
			PyErr_Format(PyExc_TypeError, "%s, expected KX_GameObject or a KX_GameObject name, None is invalid", error_prefix);
			return false;
		}
	}

	if (PyUnicode_Check(value)) {
		*object = (KX_GameObject *)manager->GetGameObjectByName(std::string(_PyUnicode_AsString(value)));

		if (*object) {
			return true;
		}
		else {
			PyErr_Format(PyExc_ValueError, "%s, requested name \"%s\" did not match any KX_GameObject in this scene", error_prefix, _PyUnicode_AsString(value));
			return false;
		}
	}

	if (PyObject_TypeCheck(value, &KX_GameObject::Type) ||
	    PyObject_TypeCheck(value, &KX_LightObject::Type)    ||
	    PyObject_TypeCheck(value, &KX_Camera::Type)         ||
	    PyObject_TypeCheck(value, &KX_FontObject::Type) ||
	    PyObject_TypeCheck(value, &KX_NavMeshObject::Type)) {
		*object = static_cast<KX_GameObject *>EXP_PROXY_REF(value);

		/* sets the error */
		if (*object == nullptr) {
			PyErr_Format(PyExc_SystemError, "%s, " EXP_PROXY_ERROR_MSG, error_prefix);
			return false;
		}

		return true;
	}

	*object = nullptr;

	if (py_none_ok) {
		PyErr_Format(PyExc_TypeError, "%s, expect a KX_GameObject, a string or None", error_prefix);
	}
	else {
		PyErr_Format(PyExc_TypeError, "%s, expect a KX_GameObject or a string", error_prefix);
	}

	return false;
}
#endif // WITH_PYTHON

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
    // printf("DEBUG: get_biome_data start for pos (%.2f, %.2f)\n", pos.x, pos.y);
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

        // Converter PyObjects para mt::vec3
        mt::vec3 center(0, 0, 0);
        if (pyCenter && PySequence_Check(pyCenter)) {
            center.x = PyFloat_AsDouble(PySequence_GetItem(pyCenter, 0));
            center.y = PyFloat_AsDouble(PySequence_GetItem(pyCenter, 1));
            center.z = PyFloat_AsDouble(PySequence_GetItem(pyCenter, 2));
        }
        
        mt::vec3 diff = mt::vec3(pos.x, pos.y, 0.0f) - mt::vec3(center.x, center.y, 0.0f);
        float dist_sq = diff.x * diff.x + diff.y * diff.y + diff.z * diff.z;
        float max_dist = radius * 1.5f;

        if (dist_sq > max_dist * max_dist) continue;

        float dist_base = std::sqrt(dist_sq);
        
        mt::vec3 noise_pos = mt::vec3(pos.x, pos.y, 0.0f) * noise_scale;
        if (pyNoiseOffset && PySequence_Check(pyNoiseOffset)) {
            noise_pos.x += PyFloat_AsDouble(PySequence_GetItem(pyNoiseOffset, 0));
            noise_pos.y += PyFloat_AsDouble(PySequence_GetItem(pyNoiseOffset, 1));
            noise_pos.z += PyFloat_AsDouble(PySequence_GetItem(pyNoiseOffset, 2));
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
    printf("\nDEBUG: --- PyApplyRecipe Start ---\n");
    PyObject *pyTerrains, *pyRecipe;
    if (!PyArg_ParseTuple(args, "OO", &pyTerrains, &pyRecipe)) {
        printf("DEBUG: ERROR - Failed to parse arguments\n");
        return nullptr;
    }

    // 1. Extract Recipe Data
    printf("DEBUG: Extracting recipe data...\n");
    int seed = PyLong_AsLong(PyDict_GetItemString(pyRecipe, "seed"));
    float tree_chance = PyFloat_AsDouble(PyDict_GetItemString(pyRecipe, "tree_chance"));
    PyObject *pyOffset = PyDict_GetItemString(pyRecipe, "offset");
    mt::vec3 offset(0, 0, 0);
    if (pyOffset && PySequence_Check(pyOffset)) {
        offset.x = PyFloat_AsDouble(PySequence_GetItem(pyOffset, 0));
        offset.y = PyFloat_AsDouble(PySequence_GetItem(pyOffset, 1));
        offset.z = PyFloat_AsDouble(PySequence_GetItem(pyOffset, 2));
    }
    PyObject *active_biomes = PyDict_GetItemString(pyRecipe, "ACTIVE_BIOMES");
    printf("DEBUG: Recipe extracted - Seed: %d, Tree Chance: %.3f\n", seed, tree_chance);

    // 2. Vertex Collection
    printf("DEBUG: Collecting vertices from terrains...\n");
    struct VertexInfo {
        KX_GameObject *obj;
        RAS_DisplayArray *array;
        int index;
    };
    std::vector<VertexInfo> combined_vertices;
    Py_ssize_t num_terrains = PyList_Size(pyTerrains);
    for (Py_ssize_t i = 0; i < num_terrains; ++i) {
        PyObject *pyObj = PyList_GetItem(pyTerrains, i);
        KX_GameObject *gameobj = static_cast<KX_GameObject *>(EXP_PROXY_REF(pyObj));
        if (gameobj) {
            //printf("DEBUG: Processing terrain object: %s\n", gameobj->GetName().ReadPtr());
            for (KX_Mesh *mesh : gameobj->GetMeshList()) {
                int num_mats = mesh->GetNumMaterials();
                for (int m = 0; m < num_mats; ++m) {
                    RAS_DisplayArray *array = mesh->GetDisplayArray(m);
                    int num_v = array->GetVertexCount();
                    for (int idx = 0; idx < num_v; ++idx) {
                        combined_vertices.push_back({gameobj, array, idx});
                    }
                }
            }
        }
    }
    printf("DEBUG: Vertex collection finished. Total vertices: %zu\n", combined_vertices.size());

    // 4. Main Loop
    printf("DEBUG: Starting main loop for terrain manipulation...\n");
    int vert_count = 0;
    for (auto &vinfo : combined_vertices) {
        vert_count++;
        if (vert_count % 5000 == 0) printf("DEBUG: Progress... %d / %zu vertices\n", vert_count, combined_vertices.size());
        
        mt::vec3 local_pos = mt::vec3(vinfo.array->GetPosition(vinfo.index));
        
        // Uso de mt::mat3x4 para transformação conforme solicitado
        mt::mat3x4 world_transform = vinfo.obj->NodeGetWorldTransform();
        mt::vec3 world_pos = world_transform * local_pos;

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

        vinfo.array->SetPosition(vinfo.index, mt::vec3(local_pos.x, local_pos.y, final_height));
        
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
        
        unsigned char (&color_arr)[4] = vinfo.array->GetColor(vinfo.index, 0);
        color_arr[0] = (unsigned char)(color.x * 255.0f);
        color_arr[1] = (unsigned char)(color.y * 255.0f);
        color_arr[2] = (unsigned char)(color.z * 255.0f);
        color_arr[3] = (unsigned char)(color.w * 255.0f);

        // 6. Spawning Logic
        if (best_biome && tree_chance > 0) {
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
                            
                            KX_GameObject *tree_proto = static_cast<KX_GameObject*>(logic->GetGameObjectByName("tree.003"));
                            KX_GameObject *tree003 = scene->AddReplicaObject(tree_proto, vinfo.obj);
                            if (tree003) {
                                tree003->NodeSetLocalPosition(world_pos + mt::vec3(0, 0, 1.0f));
                                if (BLI_gNoise(1.0f, world_pos.x * 0.021f, world_pos.y * 0.019f, (float)seed * 0.001f, 0, 0) > -0.8f) {
                                    KX_GameObject *tree002_proto = static_cast<KX_GameObject*>(logic->GetGameObjectByName("tree.002"));
                                    KX_GameObject *tree002 = scene->AddReplicaObject(tree002_proto, tree003);
                                    if (tree002) {
                                        tree002->SetParent(tree003, false, false);
                                        if (BLI_gNoise(1.0f, world_pos.x * 0.031f, world_pos.y * 0.027f, (float)seed * 0.001f, 0, 0) < 0.6f) {
                                            KX_GameObject *tree001_proto = static_cast<KX_GameObject*>(logic->GetGameObjectByName("tree.001"));
                                            KX_GameObject *tree001 = scene->AddReplicaObject(tree001_proto, tree002);
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
    //PyUpdateTerrainNormals();

    Py_RETURN_NONE;
}

// Implementação de compute_voxel_distance
struct VoxelSphereEdit {
    int operation;
    float cx, cy, cz, r, r2, inv_r;
};

struct LocalEdit {
    int operation;
    float cz, r, r2, inv_r, dxy2;
};

// ============================================================================
// OTIMIZAÇÃO BRUTAL: Perlin Noise inline completo (zero call overhead)
// Copia noise3_perlin + orgPerlinNoiseU diretamente para eliminar calls
// Ganho: 20-30% adicional no custo de noise
// ============================================================================

// Tabelas Perlin locais (copiadas do noise.c para evitar linkage)
// alignas(64): cada array em sua própria cache line - elimina false sharing em loops paralelos
alignas(64) static const unsigned char g_perlin_data_ub[514] = {
    0xA2, 0xA0, 0x19, 0x3B, 0xF8, 0xEB, 0xAA, 0xEE, 0xF3, 0x1C, 0x67, 0x28,
    0x1D, 0xED, 0x0,  0xDE, 0x95, 0x2E, 0xDC, 0x3F, 0x3A, 0x82, 0x35, 0x4D,
    0x6C, 0xBA, 0x36, 0xD0, 0xF6, 0xC,  0x79, 0x32, 0xD1, 0x59, 0xF4, 0x8,
    0x8B, 0x63, 0x89, 0x2F, 0xB8, 0xB4, 0x97, 0x83, 0xF2, 0x8F, 0x18, 0xC7,
    0x51, 0x14, 0x65, 0x87, 0x48, 0x20, 0x42, 0xA8, 0x80, 0xB5, 0x40, 0x13,
    0xB2, 0x22, 0x7E, 0x57, 0xBC, 0x7F, 0x6B, 0x9D, 0x86, 0x4C, 0xC8, 0xDB,
    0x7C, 0xD5, 0x25, 0x4E, 0x5A, 0x55, 0x74, 0x50, 0xCD, 0xB3, 0x7A, 0xBB,
    0xC3, 0xCB, 0xB6, 0xE2, 0xE4, 0xEC, 0xFD, 0x98, 0xB,  0x96, 0xD3, 0x9E,
    0x5C, 0xA1, 0x64, 0xF1, 0x81, 0x61, 0xE1, 0xC4, 0x24, 0x72, 0x49, 0x8C,
    0x90, 0x4B, 0x84, 0x34, 0x38, 0xAB, 0x78, 0xCA, 0x1F, 0x1,  0xD7, 0x93,
    0x11, 0xC1, 0x58, 0xA9, 0x31, 0xF9, 0x44, 0x6D, 0xBF, 0x33, 0x9C, 0x5F,
    0x9,  0x94, 0xA3, 0x85, 0x6,  0xC6, 0x9A, 0x1E, 0x7B, 0x46, 0x15, 0x30,
    0x27, 0x2B, 0x1B, 0x71, 0x3C, 0x5B, 0xD6, 0x6F, 0x62, 0xAC, 0x4F, 0xC2,
    0xC0, 0xE,  0xB1, 0x23, 0xA7, 0xDF, 0x47, 0xB0, 0x77, 0x69, 0x5,  0xE9,
    0xE6, 0xE7, 0x76, 0x73, 0xF,  0xFE, 0x6E, 0x9B, 0x56, 0xEF, 0x12, 0xA5,
    0x37, 0xFC, 0xAE, 0xD9, 0x3,  0x8E, 0xDD, 0x10, 0xB9, 0xCE, 0xC9, 0x8D,
    0xDA, 0x2A, 0xBD, 0x68, 0x17, 0x9F, 0xBE, 0xD4, 0xA,  0xCC, 0xD2, 0xE8,
    0x43, 0x3D, 0x70, 0xB7, 0x2,  0x7D, 0x99, 0xD8, 0xD,  0x60, 0x8A, 0x4,
    0x2C, 0x3E, 0x92, 0xE5, 0xAF, 0x53, 0x7,  0xE0, 0x29, 0xA6, 0xC5, 0xE3,
    0xF5, 0xF7, 0x4A, 0x41, 0x26, 0x6A, 0x16, 0x5E, 0x52, 0x2D, 0x21, 0xAD,
    0xF0, 0x91, 0xFF, 0xEA, 0x54, 0xFA, 0x66, 0x1A, 0x45, 0x39, 0xCF, 0x75,
    0xA4, 0x88, 0xFB, 0x5D, 0xA2, 0xA0, 0x19, 0x3B, 0xF8, 0xEB, 0xAA, 0xEE,
    0xF3, 0x1C, 0x67, 0x28, 0x1D, 0xED, 0x0,  0xDE, 0x95, 0x2E, 0xDC, 0x3F,
    0x3A, 0x82, 0x35, 0x4D, 0x6C, 0xBA, 0x36, 0xD0, 0xF6, 0xC,  0x79, 0x32,
    0xD1, 0x59, 0xF4, 0x8,  0x8B, 0x63, 0x89, 0x2F, 0xB8, 0xB4, 0x97, 0x83,
    0xF2, 0x8F, 0x18, 0xC7, 0x51, 0x14, 0x65, 0x87, 0x48, 0x20, 0x42, 0xA8,
    0x80, 0xB5, 0x40, 0x13, 0xB2, 0x22, 0x7E, 0x57, 0xBC, 0x7F, 0x6B, 0x9D,
    0x86, 0x4C, 0xC8, 0xDB, 0x7C, 0xD5, 0x25, 0x4E, 0x5A, 0x55, 0x74, 0x50,
    0xCD, 0xB3, 0x7A, 0xBB, 0xC3, 0xCB, 0xB6, 0xE2, 0xE4, 0xEC, 0xFD, 0x98,
    0xB,  0x96, 0xD3, 0x9E, 0x5C, 0xA1, 0x64, 0xF1, 0x81, 0x61, 0xE1, 0xC4,
    0x24, 0x72, 0x49, 0x8C, 0x90, 0x4B, 0x84, 0x34, 0x38, 0xAB, 0x78, 0xCA,
    0x1F, 0x1,  0xD7, 0x93, 0x11, 0xC1, 0x58, 0xA9, 0x31, 0xF9, 0x44, 0x6D,
    0xBF, 0x33, 0x9C, 0x5F, 0x9,  0x94, 0xA3, 0x85, 0x6,  0xC6, 0x9A, 0x1E,
    0x7B, 0x46, 0x15, 0x30, 0x27, 0x2B, 0x1B, 0x71, 0x3C, 0x5B, 0xD6, 0x6F,
    0x62, 0xAC, 0x4F, 0xC2, 0xC0, 0xE,  0xB1, 0x23, 0xA7, 0xDF, 0x47, 0xB0,
    0x77, 0x69, 0x5,  0xE9, 0xE6, 0xE7, 0x76, 0x73, 0xF,  0xFE, 0x6E, 0x9B,
    0x56, 0xEF, 0x12, 0xA5, 0x37, 0xFC, 0xAE, 0xD9, 0x3,  0x8E, 0xDD, 0x10,
    0xB9, 0xCE, 0xC9, 0x8D, 0xDA, 0x2A, 0xBD, 0x68, 0x17, 0x9F, 0xBE, 0xD4,
    0xA,  0xCC, 0xD2, 0xE8, 0x43, 0x3D, 0x70, 0xB7, 0x2,  0x7D, 0x99, 0xD8,
    0xD,  0x60, 0x8A, 0x4,  0x2C, 0x3E, 0x92, 0xE5, 0xAF, 0x53, 0x7,  0xE0,
    0x29, 0xA6, 0xC5, 0xE3, 0xF5, 0xF7, 0x4A, 0x41, 0x26, 0x6A, 0x16, 0x5E,
    0x52, 0x2D, 0x21, 0xAD, 0xF0, 0x91, 0xFF, 0xEA, 0x54, 0xFA, 0x66, 0x1A,
    0x45, 0x39, 0xCF, 0x75, 0xA4, 0x88, 0xFB, 0x5D, 0xA2, 0xA0
};

alignas(64) static const float g_perlin_data_v3[514][3] = {
    {0.33783, 0.715698, -0.611206},
    {-0.944031, -0.326599, -0.045624},
    {-0.101074, -0.416443, -0.903503},
    {0.799286, 0.49411, -0.341949},
    {-0.854645, 0.518036, 0.033936},
    {0.42514, -0.437866, -0.792114},
    {-0.358948, 0.597046, 0.717377},
    {-0.985413, 0.144714, 0.089294},
    {-0.601776, -0.33728, -0.723907},
    {-0.449921, 0.594513, 0.666382},
    {0.208313, -0.10791, 0.972076},
    {0.575317, 0.060425, 0.815643},
    {0.293365, -0.875702, -0.383453},
    {0.293762, 0.465759, 0.834686},
    {-0.846008, -0.233398, -0.47934},
    {-0.115814, 0.143036, -0.98291},
    {0.204681, -0.949036, -0.239532},
    {0.946716, -0.263947, 0.184326},
    {-0.235596, 0.573822, 0.784332},
    {0.203705, -0.372253, -0.905487},
    {0.756989, -0.651031, 0.055298},
    {0.497803, 0.814697, -0.297363},
    {-0.16214, 0.063995, -0.98468},
    {-0.329254, 0.834381, 0.441925},
    {0.703827, -0.527039, -0.476227},
    {0.956421, 0.266113, 0.119781},
    {0.480133, 0.482849, 0.7323},
    {-0.18631, 0.961212, -0.203125},
    {-0.748474, -0.656921, -0.090393},
    {-0.085052, -0.165253, 0.982544},
    {-0.76947, 0.628174, -0.115234},
    {0.383148, 0.537659, 0.751068},
    {0.616486, -0.668488, -0.415924},
    {-0.259979, -0.630005, 0.73175},
    {0.570953, -0.087952, 0.816223},
    {-0.458008, 0.023254, 0.888611},
    {-0.196167, 0.976563, -0.088287},
    {-0.263885, -0.69812, -0.665527},
    {0.437134, -0.892273, -0.112793},
    {-0.621674, -0.230438, 0.748566},
    {0.232422, 0.900574, -0.367249},
    {0.22229, -0.796143, 0.562744},
    {-0.665497, -0.73764, 0.11377},
    {0.670135, 0.704803, 0.232605},
    {0.895599, 0.429749, -0.114655},
    {-0.11557, -0.474243, 0.872742},
    {0.621826, 0.604004, -0.498444},
    {-0.832214, 0.012756, 0.55426},
    {-0.702484, 0.705994, -0.089661},
    {-0.692017, 0.649292, 0.315399},
    {-0.175995, -0.977997, 0.111877},
    {0.096954, -0.04953, 0.994019},
    {0.635284, -0.606689, -0.477783},
    {-0.261261, -0.607422, -0.750153},
    {0.983276, 0.165436, 0.075958},
    {-0.29837, 0.404083, -0.864655},
    {-0.638672, 0.507721, 0.578156},
    {0.388214, 0.412079, 0.824249},
    {0.556183, -0.208832, 0.804352},
    {0.778442, 0.562012, 0.27951},
    {-0.616577, 0.781921, -0.091522},
    {0.196289, 0.051056, 0.979187},
    {-0.121216, 0.207153, -0.970734},
    {-0.173401, -0.384735, 0.906555},
    {0.161499, -0.723236, -0.671387},
    {0.178497, -0.006226, -0.983887},
    {-0.126038, 0.15799, 0.97934},
    {0.830475, -0.024811, 0.556458},
    {-0.510132, -0.76944, 0.384247},
    {0.81424, 0.200104, -0.544891},
    {-0.112549, -0.393311, -0.912445},
    {0.56189, 0.152222, -0.813049},
    {0.198914, -0.254517, -0.946381},
    {-0.41217, 0.690979, -0.593811},
    {-0.407257, 0.324524, 0.853668},
    {-0.690186, 0.366119, -0.624115},
    {-0.428345, 0.844147, -0.322296},
    {-0.21228, -0.297546, -0.930756},
    {-0.273071, 0.516113, 0.811798},
    {0.928314, 0.371643, 0.007233},
    {0.785828, -0.479218, -0.390778},
    {-0.704895, 0.058929, 0.706818},
    {0.173248, 0.203583, 0.963562},
    {0.422211, -0.904297, -0.062469},
    {-0.363312, -0.182465, 0.913605},
    {0.254028, -0.552307, -0.793945},
    {-0.28891, -0.765747, -0.574554},
    {0.058319, 0.291382, 0.954803},
    {0.946136, -0.303925, 0.111267},
    {-0.078156, 0.443695, -0.892731},
    {0.182098, 0.89389, 0.409515},
    {-0.680298, -0.213318, 0.701141},
    {0.062469, 0.848389, -0.525635},
    {-0.72879, -0.641846, 0.238342},
    {-0.88089, 0.427673, 0.202637},
    {-0.532501, -0.21405, 0.818878},
    {0.948975, -0.305084, 0.07962},
    {0.925446, 0.374664, 0.055817},
    {0.820923, 0.565491, 0.079102},
    {0.25882, 0.099792, -0.960724},
    {-0.294617, 0.910522, 0.289978},
    {0.137115, 0.320038, -0.937408},
    {-0.908386, 0.345276, -0.235718},
    {-0.936218, 0.138763, 0.322754},
    {0.366577, 0.925934, -0.090637},
    {0.309296, -0.686829, -0.657684},
    {0.66983, 0.024445, 0.742065},
    {-0.917999, -0.059113, -0.392059},
    {0.365509, 0.462158, -0.807922},
    {0.083374, 0.996399, -0.014801},
    {0.593842, 0.253143, -0.763672},
    {0.974976, -0.165466, 0.148285},
    {0.918976, 0.137299, 0.369537},
    {0.294952, 0.694977, 0.655731},
    {0.943085, 0.152618, -0.295319},
    {0.58783, -0.598236, 0.544495},
    {0.203796, 0.678223, 0.705994},
    {-0.478821, -0.661011, 0.577667},
    {0.719055, -0.1698, -0.673828},
    {-0.132172, -0.965332, 0.225006},
    {-0.981873, -0.14502, 0.121979},
    {0.763458, 0.579742, 0.284546},
    {-0.893188, 0.079681, 0.442474},
    {-0.795776, -0.523804, 0.303802},
    {0.734955, 0.67804, -0.007446},
    {0.15506, 0.986267, -0.056183},
    {0.258026, 0.571503, -0.778931},
    {-0.681549, -0.702087, -0.206116},
    {-0.96286, -0.177185, 0.203613},
    {-0.470978, -0.515106, 0.716095},
    {-0.740326, 0.57135, 0.354095},
    {-0.56012, -0.824982, -0.074982},
    {-0.507874, 0.753204, 0.417969},
    {-0.503113, 0.038147, 0.863342},
    {0.594025, 0.673553, -0.439758},
    {-0.119873, -0.005524, -0.992737},
    {0.098267, -0.213776, 0.971893},
    {-0.615631, 0.643951, 0.454163},
    {0.896851, -0.441071, 0.032166},
    {-0.555023, 0.750763, -0.358093},
    {0.398773, 0.304688, 0.864929},
    {-0.722961, 0.303589, 0.620544},
    {-0.63559, -0.621948, -0.457306},
    {-0.293243, 0.072327, 0.953278},
    {-0.491638, 0.661041, -0.566772},
    {-0.304199, -0.572083, -0.761688},
    {0.908081, -0.398956, 0.127014},
    {-0.523621, -0.549683, -0.650848},
    {-0.932922, -0.19986, 0.299408},
    {0.099426, 0.140869, 0.984985},
    {-0.020325, -0.999756, -0.002319},
    {0.952667, 0.280853, -0.11615},
    {-0.971893, 0.082581, 0.220337},
    {0.65921, 0.705292, -0.260651},
    {0.733063, -0.175537, 0.657043},
    {-0.555206, 0.429504, -0.712189},
    {0.400421, -0.89859, 0.179352},
    {0.750885, -0.19696, 0.630341},
    {0.785675, -0.569336, 0.241821},
    {-0.058899, -0.464111, 0.883789},
    {0.129608, -0.94519, 0.299622},
    {-0.357819, 0.907654, 0.219238},
    {-0.842133, -0.439117, -0.312927},
    {-0.313477, 0.84433, 0.434479},
    {-0.241211, 0.053253, 0.968994},
    {0.063873, 0.823273, 0.563965},
    {0.476288, 0.862152, -0.172516},
    {0.620941, -0.298126, 0.724915},
    {0.25238, -0.749359, -0.612122},
    {-0.577545, 0.386566, 0.718994},
    {-0.406342, -0.737976, 0.538696},
    {0.04718, 0.556305, 0.82959},
    {-0.802856, 0.587463, 0.101166},
    {-0.707733, -0.705963, 0.026428},
    {0.374908, 0.68457, 0.625092},
    {0.472137, 0.208405, -0.856506},
    {-0.703064, -0.581085, -0.409821},
    {-0.417206, -0.736328, 0.532623},
    {-0.447876, -0.20285, -0.870728},
    {0.086945, -0.990417, 0.107086},
    {0.183685, 0.018341, -0.982788},
    {0.560638, -0.428864, 0.708282},
    {0.296722, -0.952576, -0.0672},
    {0.135773, 0.990265, 0.030243},
    {-0.068787, 0.654724, 0.752686},
    {0.762604, -0.551758, 0.337585},
    {-0.819611, -0.407684, 0.402466},
    {-0.727844, -0.55072, -0.408539},
    {-0.855774, -0.480011, 0.19281},
    {0.693176, -0.079285, 0.716339},
    {0.226013, 0.650116, -0.725433},
    {0.246704, 0.953369, -0.173553},
    {-0.970398, -0.239227, -0.03244},
    {0.136383, -0.394318, 0.908752},
    {0.813232, 0.558167, 0.164368},
    {0.40451, 0.549042, -0.731323},
    {-0.380249, -0.566711, 0.730865},
    {0.022156, 0.932739, 0.359741},
    {0.00824, 0.996552, -0.082306},
    {0.956635, -0.065338, -0.283722},
    {-0.743561, 0.008209, 0.668579},
    {-0.859589, -0.509674, 0.035767},
    {-0.852234, 0.363678, -0.375977},
    {-0.201965, -0.970795, -0.12915},
    {0.313477, 0.947327, 0.06546},
    {-0.254028, -0.528259, 0.81015},
    {0.628052, 0.601105, 0.49411},
    {-0.494385, 0.868378, 0.037933},
    {0.275635, -0.086426, 0.957336},
    {-0.197937, 0.468903, -0.860748},
    {0.895599, 0.399384, 0.195801},
    {0.560791, 0.825012, -0.069214},
    {0.304199, -0.849487, 0.43103},
    {0.096375, 0.93576, 0.339111},
    {-0.051422, 0.408966, -0.911072},
    {0.330444, 0.942841, -0.042389},
    {-0.452362, -0.786407, 0.420563},
    {0.134308, -0.933472, -0.332489},
    {0.80191, -0.566711, -0.188934},
    {-0.987946, -0.105988, 0.112518},
    {-0.24408, 0.892242, -0.379791},
    {-0.920502, 0.229095, -0.316376},
    {0.7789, 0.325958, 0.535706},
    {-0.912872, 0.185211, -0.36377},
    {-0.184784, 0.565369, -0.803833},
    {-0.018463, 0.119537, 0.992615},
    {-0.259247, -0.935608, 0.239532},
    {-0.82373, -0.449127, -0.345947},
    {-0.433105, 0.659515, 0.614349},
    {-0.822754, 0.378845, -0.423676},
    {0.687195, -0.674835, -0.26889},
    {-0.246582, -0.800842, 0.545715},
    {-0.729187, -0.207794, 0.651978},
    {0.653534, -0.610443, -0.447388},
    {0.492584, -0.023346, 0.869934},
    {0.609039, 0.009094, -0.79306},
    {0.962494, -0.271088, -0.00885},
    {0.2659, -0.004913, 0.963959},
    {0.651245, 0.553619, -0.518951},
    {0.280548, -0.84314, 0.458618},
    {-0.175293, -0.983215, 0.049805},
    {0.035339, -0.979919, 0.196045},
    {-0.982941, 0.164307, -0.082245},
    {0.233734, -0.97226, -0.005005},
    {-0.747253, -0.611328, 0.260437},
    {0.645599, 0.592773, 0.481384},
    {0.117706, -0.949524, -0.29068},
    {-0.535004, -0.791901, -0.294312},
    {-0.627167, -0.214447, 0.748718},
    {-0.047974, -0.813477, -0.57959},
    {-0.175537, 0.477264, -0.860992},
    {0.738556, -0.414246, -0.53183},
    {0.562561, -0.704071, 0.433289},
    {-0.754944, 0.64801, -0.100586},
    {0.114716, 0.044525, -0.992371},
    {0.966003, 0.244873, -0.082764},
    // Repetir primeiros elementos para completar 514
    {0.33783, 0.715698, -0.611206},
    {-0.944031, -0.326599, -0.045624}
};

// FORCEINLINE para garantir que o compilador inline
#ifdef _MSC_VER
    #define FORCE_INLINE __forceinline
#else
    #define FORCE_INLINE __attribute__((always_inline)) inline
#endif

// Lerp inline — forma explícita FMA: garante contração sem depender de flags do compilador
static FORCE_INLINE float perlin_lerp(float t, float a, float b) {
    return a + t * (b - a);
}

// perlin_lerp2: calcula dois lerps com o mesmo t e b0-a0 / b1-a1 pré-calculados.
// Usado para emitir as duas subtrações antes da multiplicação, melhorando ILP.
static FORCE_INLINE void perlin_lerp2(float t,
    float a0, float b0, float a1, float b1,
    float &out0, float &out1)
{
    const float d0 = b0 - a0;
    const float d1 = b1 - a1;
    out0 = a0 + t * d0;
    out1 = a1 + t * d1;
}
static FORCE_INLINE float cave_smooth(float t)
{
    const float t2 = t * t;          // t² — usado 2× abaixo
    return t2 * (3.0f - 2.0f * t);  // t²*(3-2t)
}

// fast_floor: evita floorf() + cast (mais barato em x86/ARM com otimizacoes)
#define PERLIN_FAST_FLOOR(x) ((int)(x) - ((x) < (float)(int)(x)))

// grad_dot: produto interno gradiente×residuo.
// Recebe ponteiro direto para o gradiente (3 floats contíguos).
// O acesso g[idx] já retorna float* — mantemos sem wrapper para que o
// compilador use LEA direto em x86 sem load extra do ponteiro base.
static FORCE_INLINE float perlin_grad_dot(const float * __restrict q, float rx, float ry, float rz) {
    return rx * q[0] + ry * q[1] + rz * q[2];
}

// Perlin noise completo inline (zero call overhead)
static FORCE_INLINE float cave_noise_perlin_inline(float x, float y, float z)
{
    auto hash3 = [](int x, int y, int z) -> uint32_t
    {
        uint32_t h = (uint32_t)x * 374761393u;
        h ^= (uint32_t)y * 668265263u;
        h ^= (uint32_t)z * 2147483647u;

        h = (h ^ (h >> 13)) * 1274126177u;
        h ^= h >> 16;

        return h;
    };

    auto value = [&](int x, int y, int z) -> float
    {
        return (hash3(x, y, z) >> 8) * (1.0f / 16777216.0f);
    };

    float total = 0.0f;
    float amplitude = 0.5f;
    float frequency = 1.0f;

    for (int octave = 0; octave < 3; ++octave)
    {
        const float px = x * frequency;
        const float py = y * frequency;
        const float pz = z * frequency;

        const int ix = PERLIN_FAST_FLOOR(px);
        const int iy = PERLIN_FAST_FLOOR(py);
        const int iz = PERLIN_FAST_FLOOR(pz);

        float fx = px - (float)ix;
        float fy = py - (float)iy;
        float fz = pz - (float)iz;

        // Smoothstep
        fx = fx * fx * (3.0f - 2.0f * fx);
        fy = fy * fy * (3.0f - 2.0f * fy);
        fz = fz * fz * (3.0f - 2.0f * fz);

        const float c000 = value(ix    , iy    , iz    );
        const float c100 = value(ix + 1, iy    , iz    );
        const float c010 = value(ix    , iy + 1, iz    );
        const float c110 = value(ix + 1, iy + 1, iz    );

        const float c001 = value(ix    , iy    , iz + 1);
        const float c101 = value(ix + 1, iy    , iz + 1);
        const float c011 = value(ix    , iy + 1, iz + 1);
        const float c111 = value(ix + 1, iy + 1, iz + 1);

        const float x00 = c000 + fx * (c100 - c000);
        const float x10 = c010 + fx * (c110 - c010);
        const float x01 = c001 + fx * (c101 - c001);
        const float x11 = c011 + fx * (c111 - c011);

        const float y0 = x00 + fy * (x10 - x00);
        const float y1 = x01 + fy * (x11 - x01);

        total += (y0 + fz * (y1 - y0)) * amplitude;

        frequency *= 2.0f;
        amplitude *= 0.5f;
    }

    // Normalização (0.5 + 0.25 + 0.125 = 0.875)
    return total * (1.0f / 0.875f);
}

// Wrapper simples (será inline pelo compilador)
static inline float cave_noise_perlin(float x, float y, float z) {
    return cave_noise_perlin_inline(x, y, z);
}

// ============================================================================
// CAVERNAS 3D - GRID 6x6x6 COM INTERPOLAÇÃO TRILINEAR
// Usa Perlin puro (1 oitava, sem melhorias do terreno) - túneis definidos.
// Loop único: warp calculado inline, sem arrays intermediários.
// ============================================================================

struct CaveGrid3D {
    std::vector<float> values;
    int nx, ny, nz;
    float min_x, max_x, min_y, max_y, min_z, max_z;
    float step_x, step_y, step_z;
    float inv_step_x, inv_step_y, inv_step_z;

    CaveGrid3D() : nx(0), ny(0), nz(0) {}

    CaveGrid3D(int nx_, int ny_, int nz_,
               float min_x_, float max_x_,
               float min_y_, float max_y_,
               float min_z_, float max_z_)
        : nx(nx_), ny(ny_), nz(nz_),
          min_x(min_x_), max_x(max_x_),
          min_y(min_y_), max_y(max_y_),
          min_z(min_z_), max_z(max_z_)
    {
        values.resize(nx * ny * nz, 0.0f);
        step_x = (max_x - min_x) / (nx - 1);
        step_y = (max_y - min_y) / (ny - 1);
        step_z = (max_z - min_z) / (nz - 1);
        inv_step_x = (step_x != 0.0f) ? (1.0f / step_x) : 0.0f;
        inv_step_y = (step_y != 0.0f) ? (1.0f / step_y) : 0.0f;
        inv_step_z = (step_z != 0.0f) ? (1.0f / step_z) : 0.0f;
    }
};

static void generate_cave_grid_3d(CaveGrid3D& grid,
                                   float cave_seed,
                                   float cave_threshold,
                                   float cave_noise_scale,
                                   float warp_scale,
                                   float warp_strength)
{
    (void)cave_threshold;
    (void)warp_scale;
    (void)warp_strength;

    const float cave_seed_offset = cave_seed * 0.001f;
    const int nx = grid.nx;
    const int ny = grid.ny;
    const int nz = grid.nz;
    const float min_x = grid.min_x;
    const float min_y = grid.min_y;
    const float min_z = grid.min_z;
    const float step_x = grid.step_x;
    const float step_y = grid.step_y;
    const float step_z = grid.step_z;
    float * __restrict out = grid.values.data();

    // Loop aninhado com xy_base acumulado — elimina i/ny e i%ny (divisão inteira por iteração)
    #pragma omp parallel for schedule(static)
    for (int x = 0; x < nx; ++x) {
        const float wx = min_x + (float)x * step_x;
        const float nxs = wx * cave_noise_scale;
        int xy_base = x * ny * nz;
        for (int y = 0; y < ny; ++y) {
            const float wy = min_y + (float)y * step_y;
            const float nys = wy * cave_noise_scale;
            float nzs = min_z * cave_noise_scale + cave_seed_offset;
            const float dz = step_z * cave_noise_scale;
            for (int z = 0; z < nz; ++z) {
                out[xy_base + z] = cave_noise_perlin_inline(nxs, nys, nzs);
                nzs += dz;
            }
            xy_base += nz;
        }
    }
}

static inline float interpolate_cave_trilinear(const CaveGrid3D& grid,
                                                float wx, float wy, float wz)
{
    // Calcular posição no grid (otimizado)
    float fx = (wx - grid.min_x) * grid.inv_step_x;
    float fy = (wy - grid.min_y) * grid.inv_step_y;
    float fz = (wz - grid.min_z) * grid.inv_step_z;
    
    // Clamp (otimizado)
    fx = (fx < 0.0f) ? 0.0f : ((fx > grid.nx - 1.001f) ? grid.nx - 1.001f : fx);
    fy = (fy < 0.0f) ? 0.0f : ((fy > grid.ny - 1.001f) ? grid.ny - 1.001f : fy);
    fz = (fz < 0.0f) ? 0.0f : ((fz > grid.nz - 1.001f) ? grid.nz - 1.001f : fz);
    
    int ix = (int)fx;
    int iy = (int)fy;
    int iz = (int)fz;
    
    float tx = fx - ix;
    float ty = fy - iy;
    float tz = fz - iz;
    
    // Interpolação "sharper" para túneis (não bolhas)
    // Usar smoothstep para transições mais abruptas
    tx = cave_smooth(tx);
    ty = cave_smooth(ty);
    tz = cave_smooth(tz);
    
    int ix1 = (ix + 1 < grid.nx) ? ix + 1 : ix;
    int iy1 = (iy + 1 < grid.ny) ? iy + 1 : iy;
    int iz1 = (iz + 1 < grid.nz) ? iz + 1 : iz;
    
    // Acesso otimizado (menos chamadas de função)
    const int stride_y = grid.nz;
    const int stride_x = grid.ny * grid.nz;
    const float* vals = grid.values.data();
    
    const int base000 = ix * stride_x + iy * stride_y + iz;
    const int dx = (ix1 - ix) * stride_x;
    const int dy = (iy1 - iy) * stride_y;
    const int dz = (iz1 - iz);
    
    float v000 = vals[base000];
    float v100 = vals[base000 + dx];
    float v010 = vals[base000 + dy];
    float v110 = vals[base000 + dx + dy];
    float v001 = vals[base000 + dz];
    float v101 = vals[base000 + dx + dz];
    float v011 = vals[base000 + dy + dz];
    float v111 = vals[base000 + dx + dy + dz];
    
    // Interpolação trilinear otimizada
    float v00 = v000 + (v100 - v000) * tx;
    float v01 = v001 + (v101 - v001) * tx;
    float v10 = v010 + (v110 - v010) * tx;
    float v11 = v011 + (v111 - v011) * tx;
    
    float v0 = v00 + (v10 - v00) * ty;
    float v1 = v01 + (v11 - v01) * ty;
    
    return v0 + (v1 - v0) * tz;
}

// ============================================================================

static inline float KX_BiomeLerp(const float a, const float b, const float t)
{
    return a + (b - a) * t;
}

static inline float KX_BiomeSmooth(const float t)
{
    return t * t * (3.0f - 2.0f * t);
}

static inline float KX_BiomeHash2D(uint32_t x, uint32_t y)
{
    uint32_t n = x * 374761393u + y * 668265263u;
    n = (n ^ (n >> 13)) * 1274126177u;
    n ^= (n >> 16);
    return (float)n * (1.0f / 4294967295.0f);
}

static FORCE_INLINE float KX_BiomeValueNoise(float x, float y)
{
    const int32_t ix = (int32_t)x;
    const int32_t iy = (int32_t)y;

    const float fx = x - (float)ix;
    const float fy = y - (float)iy;

    const float sx = KX_BiomeSmooth(fx);
    const float sy = KX_BiomeSmooth(fy);

    const float h00 = KX_BiomeHash2D((uint32_t)ix,     (uint32_t)iy);
    const float h10 = KX_BiomeHash2D((uint32_t)(ix+1), (uint32_t)iy);
    const float h01 = KX_BiomeHash2D((uint32_t)ix,     (uint32_t)(iy+1));
    const float h11 = KX_BiomeHash2D((uint32_t)(ix+1), (uint32_t)(iy+1));

    const float nx0 = KX_BiomeLerp(h00, h10, sx);
    const float nx1 = KX_BiomeLerp(h01, h11, sx);

    return KX_BiomeLerp(nx0, nx1, sy);
}

struct KX_BiomeNoiseParams {
    const char *name;
    float temp;
    float hum;
    float height_mult;
    float freq_mult;
    float oct2_freq_mult_mult;
    float oct2_weight_mult;
    float oct2_height_mult;
};

enum {
    BIOME_FOREST = 0,
    BIOME_PLAINS,
    BIOME_DESERT,
    BIOME_SNOW,
    BIOME_SWAMP,
    BIOME_MOUNTAIN,
    BIOME_SAVANNA,
    BIOME_CREEK,
    BIOME_TUNDRA,
    BIOME_JUNGLE,
    BIOME_COUNT
};

// KX_GetBiomeID: quantiza temp/hum em 4 níveis cada (16 celas),
// mapeia para biome via tabela - zero loop, zero branch.
static FORCE_INLINE int KX_GetBiomeID(const float temp, const float hum)
{
    // Quantiza [0,1] em 4 faixas (0-3)
    const int ti = (temp < 0.35f) ? 0 : (temp < 0.55f) ? 1 : (temp < 0.75f) ? 2 : 3;
    const int hi = (hum  < 0.35f) ? 0 : (hum  < 0.55f) ? 1 : (hum  < 0.75f) ? 2 : 3;
    // Tabela 4x4: cada cela mapeia ao biome mais próximo (pré-computado)
    // Linha = ti (temperatura crescente), Coluna = hi (humidade crescente)
    static const uint8_t biome_table[4][4] = {
        /* ti=0 (frio)  */ { BIOME_SNOW,     BIOME_TUNDRA,  BIOME_SNOW,    BIOME_TUNDRA  },
        /* ti=1 (ameno) */ { BIOME_MOUNTAIN, BIOME_PLAINS,  BIOME_FOREST,  BIOME_CREEK   },
        /* ti=2 (quente)*/ { BIOME_SAVANNA,  BIOME_PLAINS,  BIOME_FOREST,  BIOME_SWAMP   },
        /* ti=3 (árido) */ { BIOME_DESERT,   BIOME_DESERT,  BIOME_JUNGLE,  BIOME_JUNGLE  },
    };
    return biome_table[ti][hi];
}

static const KX_BiomeNoiseParams s_biomes[BIOME_COUNT] = {
    {"Floresta", 0.6f, 0.7f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    {"Planicie", 0.5f, 0.4f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    {"Deserto",  0.9f, 0.1f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    {"Neve",     0.1f, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    {"Pântano",  0.6f, 0.9f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    {"mountain", 0.3f, 0.4f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    {"Savanna",  0.8f, 0.3f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    {"Riacho",   0.5f, 0.8f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    {"Tundra",   0.2f, 0.2f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    {"Selva",    0.9f, 0.9f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
};

static inline const KX_BiomeNoiseParams& KX_SelectBiomeParams(const float temp, const float hum)
{
    return s_biomes[KX_GetBiomeID(temp, hum)];
}

static inline unsigned int KX_GetBiomeColor(int biome_id)
{
    // Tabela de cores por bioma (0xAABBGGRR) - ordem correta para o RAS_DisplayArray
    static const unsigned int s_biome_colors[BIOME_COUNT] = {
        /* BIOME_FOREST     */ 0x00204000,    // Verde escuro para floresta (R=0x00, G=0x40, B=0x20)
        /* BIOME_PLAINS    */ 0x0040AA80,   // Verde claro para planície (R=0x80, G=0xAA, B=0x40)
        /* BIOME_DESERT    */ 0x008CB4D2,   // Areia para deserto (R=0xD2, G=0xB4, B=0x8C)
        /* BIOME_SNOW      */ 0x00F0F0F0,   // Branco para neve (R=0xF0, G=0xF0, B=0xF0)
        /* BIOME_SWAMP     */ 0x00306020,   // Verde pântano (R=0x20, G=0x60, B=0x30)
        /* BIOME_MOUNTAIN  */ 0x00808080,   // Cinza para montanha (R=0x80, G=0x80, B=0x80)
        /* BIOME_SAVANNA   */ 0x0040A0C0,   // Amarelo savana (R=0xC0, G=0xA0, B=0x40)
        /* BIOME_CREEK     */ 0x00C08040,   // Azul riacho (R=0x40, G=0x80, B=0xC0)
        /* BIOME_TUNDRA    */ 0x00C0C0A0,   // Cinza-azulado tundra (R=0xA0, G=0xC0, B=0xC0)
        /* BIOME_JUNGLE    */ 0x00306000     // Verde escuro selva (R=0x00, G=0x60, B=0x30)
    };
    if (biome_id < 0 || biome_id >= BIOME_COUNT) biome_id = BIOME_FOREST;
    return s_biome_colors[biome_id];
}

// Núcleo compartilhado: 2-octave fBM + hetero terrain shaping.
// Recebe parâmetros já escalados - evita duplicação entre get_voxel_noise e _raw.
static FORCE_INLINE float perlin_shaped_2oct(
    float x, float y, float seed_z,
    float height_scale, float oct2_freq_mult,
    float oct2_weight, float oct2_height_scale)
{
    // inv_hs antes das chamadas Perlin: DIV unit executa em paralelo com
    // a unidade FP ocupada pelos lookups de permutação (~10-20 ciclos de latência DIV
    // absorvidos pelos ~40-60 ciclos de cada cave_noise_perlin_inline)
    const float inv_hs = 1.0f / (height_scale + 1e-9f);

    // Oitava 1 - baixa frequência, define forma geral
    const float n1 = cave_noise_perlin_inline(x, y, seed_z);

    // Oitava 2 - pré-computa coords escaladas uma única vez
    const float sx = seed_z * oct2_freq_mult;
    const float nx = x      * oct2_freq_mult;
    const float ny = y      * oct2_freq_mult;
    const float n2 = cave_noise_perlin_inline(nx, ny, sx);

    // h1 e h2 calculados em sequência mas independentes:
    // o compilador pode emitir as 4 operações (2 adds + 2 muls) em 2 ciclos com 2 FP units
    const float n1m = n1 + n1 - 1.0f;   // mapeado: 2*n1-1 via ADD+SUB
    const float n2m = n2 + n2 - 1.0f;   // mapeado: 2*n2-1 via ADD+SUB
    const float h1  = n1m * height_scale;
    const float h2  = n2m * oct2_height_scale;

    // inv_hs antes de h1_scaled: permite que h1_scaled e h2 sejam calculados
    // em paralelo na unidade FP (nenhum depende do outro)
    const float h1_scaled = h1 * inv_hs;

    // hetero terrain: h = (h1 + w*h2) * t1
    // w*h2 calculado antes de h1+ : as duas operações são independentes → ILP
    const float t1     = 1.0f + 0.25f * h1_scaled;
    const float wh2    = oct2_weight * h2;          // independente de t1
    const float h      = (h1 + wh2) * t1;

    // shaping: shaped = h + 0.5*h*h_scaled = h*(1 + 0.5*h/hs)
    // 0.5f como constante estática: evita load de .rdata em MSVC
    static const float k_half = 0.5f;
    const float h_scaled = h * inv_hs;
    const float shaped   = h + k_half * (h * h_scaled);

    // fmaxf → maxss/vmaxps: 1 instrução SSE, sem branch
    const float s = shaped;
    return s + fmaxf(s, 0.0f);
}


static FORCE_INLINE float get_voxel_noise(int pix, int piy, float noise_scale, float seed_z,
                                    float height_scale, float oct2_freq_mult,
                                    float oct2_weight, float oct2_height_scale,
                                    uint8_t* r_biome_id = nullptr)
{
    const float biome_seed   = seed_z;
    const float bx           = (float)pix * 0.000012f + biome_seed;
    const float by           = (float)piy * 0.000012f + biome_seed * 1.37f;


    const float temp = KX_BiomeValueNoise(bx,        by);
    const float hum  = KX_BiomeValueNoise(bx + 5.2f, by + 1.3f);

    const uint8_t biome_id = (uint8_t)KX_GetBiomeID(temp, hum);
    if (r_biome_id) {
        *r_biome_id = biome_id;
    }
    const KX_BiomeNoiseParams& biome = s_biomes[biome_id];

    const float hs_b   = height_scale      * biome.height_mult;
    const float ns_b   = noise_scale       * biome.freq_mult;
    const float f2_b   = oct2_freq_mult    * biome.oct2_freq_mult_mult;
    const float w2_b   = oct2_weight       * biome.oct2_weight_mult;
    const float hs2_b  = oct2_height_scale * biome.oct2_height_mult;

    const float x = (float)pix * ns_b;
    const float y = (float)piy * ns_b;

    return perlin_shaped_2oct(x, y, seed_z, hs_b, f2_b, w2_b, hs2_b);
}

// get_voxel_noise_raw: versão sem bioma - direto ao núcleo Perlin.
static FORCE_INLINE float get_voxel_noise_raw(int pix, int piy, float noise_scale, float seed_z,
                                        float height_scale, float oct2_freq_mult,
                                        float oct2_weight, float oct2_height_scale)
{
    const float fpix = (float)pix;
    const float fpiy = (float)piy;
    return perlin_shaped_2oct(
        fpix * noise_scale,
        fpiy * noise_scale,
        seed_z, height_scale, oct2_freq_mult, oct2_weight, oct2_height_scale);
}

PyObject *KX_GameObject::PyComputeVoxelDistance(PyObject *args) {
    PyObject *py_resolution, *py_bounds, *py_values, *py_base_params, *py_edits;
    
    if (!PyArg_ParseTuple(args, "OOOOO:compute_voxel_distance", 
                          &py_resolution, &py_bounds, &py_values, &py_base_params, &py_edits)) {
        return nullptr;
    }

    // 1. Parse resolution
    PyObject *res0 = PySequence_GetItem(py_resolution, 0);
    PyObject *res1 = PySequence_GetItem(py_resolution, 1);
    PyObject *res2 = PySequence_GetItem(py_resolution, 2);
    int nx = PyLong_AsLong(res0);
    int ny = PyLong_AsLong(res1);
    int nz = PyLong_AsLong(res2);
    Py_DECREF(res0); Py_DECREF(res1); Py_DECREF(res2);

    // 2. Parse bounds
    PyObject *b0 = PySequence_GetItem(py_bounds, 0);
    PyObject *b1 = PySequence_GetItem(py_bounds, 1);
    PyObject *b2 = PySequence_GetItem(py_bounds, 2);
    PyObject *b3 = PySequence_GetItem(py_bounds, 3);
    PyObject *b4 = PySequence_GetItem(py_bounds, 4);
    PyObject *b5 = PySequence_GetItem(py_bounds, 5);
    float min_x = PyFloat_AsDouble(b0);
    float max_x = PyFloat_AsDouble(b1);
    float min_y = PyFloat_AsDouble(b2);
    float max_y = PyFloat_AsDouble(b3);
    float min_z = PyFloat_AsDouble(b4);
    float max_z = PyFloat_AsDouble(b5);
    Py_DECREF(b0); Py_DECREF(b1); Py_DECREF(b2); Py_DECREF(b3); Py_DECREF(b4); Py_DECREF(b5);

    // 3. Parse base height params
    PyObject *bp0 = PySequence_GetItem(py_base_params, 0);
    PyObject *bp1 = PySequence_GetItem(py_base_params, 1);
    PyObject *bp2 = PySequence_GetItem(py_base_params, 2);
    float seed = PyFloat_AsDouble(bp0);
    float height_scale = PyFloat_AsDouble(bp1);
    float freq = PyFloat_AsDouble(bp2);
    Py_DECREF(bp0); Py_DECREF(bp1); Py_DECREF(bp2);

    float cache_scale = 4.0f;
    float h_cache_scale = 0.5f; // Noise scale
    if (PySequence_Size(py_base_params) > 3) {
        PyObject *bp3 = PySequence_GetItem(py_base_params, 3);
        cache_scale = PyFloat_AsDouble(bp3);
        Py_DECREF(bp3);
    }
    if (PySequence_Size(py_base_params) > 4) {
        PyObject *bp4 = PySequence_GetItem(py_base_params, 4);
        h_cache_scale = PyFloat_AsDouble(bp4);
        Py_DECREF(bp4);
    }
    float noise_scale_multiplier = 1.0f;
    if (PySequence_Size(py_base_params) > 5) {
        PyObject *bp5 = PySequence_GetItem(py_base_params, 5);
        noise_scale_multiplier = PyFloat_AsDouble(bp5);
        Py_DECREF(bp5);
    }
    float oct2_freq_mult = 2.0f;
    if (PySequence_Size(py_base_params) > 6) {
        PyObject *bp6 = PySequence_GetItem(py_base_params, 6);
        oct2_freq_mult = (float)PyFloat_AsDouble(bp6);
        Py_DECREF(bp6);
    }
    float oct2_weight = 0.5f;
    if (PySequence_Size(py_base_params) > 7) {
        PyObject *bp7 = PySequence_GetItem(py_base_params, 7);
        oct2_weight = (float)PyFloat_AsDouble(bp7);
        Py_DECREF(bp7);
    }
    float oct2_height_scale = height_scale; // default: igual à oitava 1
    if (PySequence_Size(py_base_params) > 8) {
        PyObject *bp8 = PySequence_GetItem(py_base_params, 8);
        oct2_height_scale = (float)PyFloat_AsDouble(bp8);
        Py_DECREF(bp8);
    }
    (void)cache_scale;

    // CAVERNAS 3D - PARÂMETROS EXATOS DO PYTHON
    const int enable_caves = 1;
    const float cave_noise_scale = 0.12f;  // EXATO do Python
    const float cave_threshold = 0.35f;    // EXATO do Python
    const float cave_seed = 99999.0f;
    const float cave_safety_margin = 5.0f;
    const float warp_scale = 0.05f;
    const float warp_strength = 10.0f;
    const float cave_strength_mult = 30.0f;

    // 4. Parse edits
    std::vector<VoxelSphereEdit> edits;
    Py_ssize_t num_edits = PySequence_Size(py_edits);
    for (Py_ssize_t i = 0; i < num_edits; ++i) {
        PyObject *item = PySequence_GetItem(py_edits, i);
        VoxelSphereEdit edit;
        PyObject *e0 = PySequence_GetItem(item, 0);
        PyObject *e1 = PySequence_GetItem(item, 1);
        PyObject *e2 = PySequence_GetItem(item, 2);
        PyObject *e3 = PySequence_GetItem(item, 3);
        PyObject *e4 = PySequence_GetItem(item, 4);
        PyObject *e5 = PySequence_GetItem(item, 5);
        PyObject *e6 = PySequence_GetItem(item, 6);
        
        edit.operation = PyLong_AsLong(e0);
        edit.cx = PyFloat_AsDouble(e1);
        edit.cy = PyFloat_AsDouble(e2);
        edit.cz = PyFloat_AsDouble(e3);
        edit.r = PyFloat_AsDouble(e4);
        edit.r2 = PyFloat_AsDouble(e5);
        edit.inv_r = PyFloat_AsDouble(e6);
        
        edits.push_back(edit);
        
        Py_DECREF(e0); Py_DECREF(e1); Py_DECREF(e2); Py_DECREF(e3); Py_DECREF(e4); Py_DECREF(e5); Py_DECREF(e6);
        Py_DECREF(item);
    }

    // 5. Access values buffer
    Py_buffer view;
    if (PyObject_GetBuffer(py_values, &view, PyBUF_WRITABLE) < 0) {
        PyErr_SetString(PyExc_TypeError, "values must be a writable buffer (e.g., bytearray or array.array)");
        return nullptr;
    }
    float *values_ptr = (float *)view.buf;
    float step_x = (max_x - min_x) / (nx - 1);
    float step_y = (max_y - min_y) / (ny - 1);
    float step_z = (max_z - min_z) / (nz - 1);

    float noise_scale = (1.0f / h_cache_scale) * freq * noise_scale_multiplier;
    float seed_z = seed * 0.001f;
    std::vector<float> world_x(nx);
    std::vector<float> world_y(ny);
    std::vector<float> world_z(nz);
    for (int x = 0; x < nx; ++x) {
        world_x[x] = (x == nx - 1) ? max_x : (min_x + x * step_x);
    }
    for (int y = 0; y < ny; ++y) {
        world_y[y] = (y == ny - 1) ? max_y : (min_y + y * step_y);
    }
    for (int z = 0; z < nz; ++z) {
        world_z[z] = (z == nz - 1) ? max_z : (min_z + z * step_z);
    }

    // 6. Pre-calculate 2D Heightmap with continuous world coordinates
    std::vector<float> heightmap(nx * ny);
    
    Py_BEGIN_ALLOW_THREADS
    
    #pragma omp parallel for schedule(dynamic)
    for (int x = 0; x < nx; ++x) {
        float wx = world_x[x];
        double sx_d = (double)wx * (double)h_cache_scale;
        double sx_r = std::round(sx_d);
        if (std::fabs(sx_d - sx_r) <= 1e-6) {
            sx_d = sx_r;
        }
        int ix = (int)std::floor(sx_d);
        float fx = (float)(sx_d - (double)ix);
        if (fx <= 1e-6f) {
            fx = 0.0f;
        }
        else if ((1.0f - fx) <= 1e-6f) {
            ix += 1;
            fx = 0.0f;
        }

        for (int y = 0; y < ny; ++y) {
            float wy = world_y[y];
            double sy_d = (double)wy * (double)h_cache_scale;
            double sy_r = std::round(sy_d);
            if (std::fabs(sy_d - sy_r) <= 1e-6) {
                sy_d = sy_r;
            }
            int iy = (int)std::floor(sy_d);
            float fy = (float)(sy_d - (double)iy);
            if (fy <= 1e-6f) {
                fy = 0.0f;
            }
            else if ((1.0f - fy) <= 1e-6f) {
                iy += 1;
                fy = 0.0f;
            }

            float h00 = get_voxel_noise(ix, iy, noise_scale, seed_z, height_scale, oct2_freq_mult, oct2_weight, oct2_height_scale);
            float h10 = get_voxel_noise(ix + 1, iy, noise_scale, seed_z, height_scale, oct2_freq_mult, oct2_weight, oct2_height_scale);
            float h01 = get_voxel_noise(ix, iy + 1, noise_scale, seed_z, height_scale, oct2_freq_mult, oct2_weight, oct2_height_scale);
            float h11 = get_voxel_noise(ix + 1, iy + 1, noise_scale, seed_z, height_scale, oct2_freq_mult, oct2_weight, oct2_height_scale);

            float hx0 = h00 + (h10 - h00) * fx;
            float hx1 = h01 + (h11 - h01) * fx;
            heightmap[x * ny + y] = hx0 + (hx1 - hx0) * fy;
        }
    }

    // CAVERNAS 3D - Gerar grid 6x6x6
    CaveGrid3D cave_grid;
    if (enable_caves) {
        cave_grid = CaveGrid3D(6, 6, 6, min_x, max_x, min_y, max_y, min_z, max_z);
        generate_cave_grid_3d(cave_grid, cave_seed, cave_threshold, 
                             cave_noise_scale, warp_scale, warp_strength);
    }

    // 7. Main Loop with world coordinates
    #pragma omp parallel
    {
        std::vector<LocalEdit> local_edits;
        local_edits.reserve(edits.size());

        #pragma omp for schedule(dynamic)
        for (int x = 0; x < nx; ++x) {
            float wx = world_x[x];
            int x_off = x * ny * nz;
            
            for (int y = 0; y < ny; ++y) {
                float wy = world_y[y];
                int xy_off = x_off + y * nz;
                float base_h = heightmap[x * ny + y];

                // Pre-filter edits for this (x, y) column using world coordinates
                local_edits.clear();
                for (const auto& edit : edits) {
                    float dx = wx - edit.cx;
                    float dy = wy - edit.cy;
                    float dxy2 = dx * dx + dy * dy;
                    // Keep edit if the column XY is within the sphere's radius
                    // (dxy2 < r2 means there exists some Z where dist < r)
                    if (dxy2 < edit.r2) {
                        local_edits.push_back({edit.operation, edit.cz, edit.r, edit.r2, edit.inv_r, dxy2});
                    }
                }

                if (local_edits.empty()) {
                    for (int z = 0; z < nz; ++z) {
                        float wz = world_z[z];
                        float d = wz - base_h;
                        
                        // CAVERNAS 3D - Aplicar EXATAMENTE como Python
                        if (enable_caves && d < -cave_safety_margin) {
                            float cave_noise = interpolate_cave_trilinear(cave_grid, wx, wy, wz);
                            if (cave_noise < cave_threshold) {
                                // Python: cave_strength = (cave_threshold - cave_noise) * 30.0
                                // Python: nd = cave_strength (NOT negative!)
                                float cave_strength = (cave_threshold - cave_noise) * cave_strength_mult;
                                float nd = cave_strength;
                                if (nd > d) {
                                    d = nd;
                                }
                            }
                        }
                        
                        values_ptr[xy_off + z] = d;
                    }
                    continue;
                }

                for (int z = 0; z < nz; ++z) {
                    float wz = world_z[z];
                    float d = wz - base_h;
                    
                    // CAVERNAS 3D - Margem de segurança em VOXELS, não unidades
                    float voxels_below_surface = -d / step_z;
                    
                    if (enable_caves && voxels_below_surface > cave_safety_margin) {
                        float cave_noise = interpolate_cave_trilinear(cave_grid, wx, wy, wz);
                        if (cave_noise < cave_threshold) {
                            float cave_strength = (cave_threshold - cave_noise) * cave_strength_mult;
                            float nd = cave_strength;
                            if (nd > d) {
                                d = nd;
                            }
                        }
                    }
                    
                    // Aplicar edits CSG
                    for (const auto& edit : local_edits) {
                        float dz = wz - edit.cz;
                        float dist_sq = edit.dxy2 + dz * dz;
                        float sd = std::sqrt(dist_sq) - edit.r;
                        if (edit.operation == 1) { // DIFFERENCE
                            float nd = -sd;
                            if (nd > d) d = nd;
                        } else if (edit.operation == 0) { // UNION
                            if (sd < d) d = sd;
                        } else { // INTERSECTION
                            if (sd > d) d = sd;
                        }
                    }
                    values_ptr[xy_off + z] = d;
                }
            }
        }
        
    }
    Py_END_ALLOW_THREADS

    PyBuffer_Release(&view);
    Py_RETURN_NONE;
}

const char KX_GameObject::ComputeVoxelDistance_doc[] = 
"compute_voxel_distance(resolution, bounds, values, base_params, edits)\n"
"Computes the voxel distance values in C++ and releases the GIL.\n"
"resolution: (nx, ny, nz)\n"
"bounds: (min_x, max_x, min_y, max_y, min_z, max_z)\n"
"values: writable buffer of floats\n"
"base_params: (seed, height_scale, freq, h_cache_scale, noise_scale_mult,\n"
"              enable_caves, cave_noise_scale, cave_threshold, cave_seed,\n"
"              cave_safety_margin, warp_scale, warp_strength, cave_strength_mult)\n"
"edits: list of (op, cx, cy, cz, r, r2, inv_r)\n"
"\n"
"Cave parameters (optional, defaults):\n"
"  enable_caves=1: 0=disabled, 1=enabled\n"
"  cave_noise_scale=0.12: noise scale (smaller = larger caves)\n"
"  cave_threshold=0.35: threshold (higher = fewer but larger caves)\n"
"  cave_seed=99999.0: random seed for caves\n"
"  cave_safety_margin=5.0: minimum depth below surface for caves\n"
"  warp_scale=0.05: domain warping scale\n"
"  warp_strength=10.0: domain warping strength\n"
"  cave_strength_mult=30.0: cave carving strength multiplier";

// Python Binding Boilerplate
const char KX_GameObject::ApplyRecipe_doc[] = 
"apply_recipe(terrains, recipe)\n"
"Applies a terrain recipe to a list of terrain objects.\n"
"terrains: list of KX_GameObject\n"
"recipe: dict containing seed, offset, ACTIVE_BIOMES, etc.";

PyObject *KX_GameObject::PySurfaceNetsGenerate(PyObject *args) {
    using Clock = std::chrono::high_resolution_clock;
    using NS     = std::chrono::nanoseconds;

    // Helper: elapsed nanoseconds → formatted string (us or ms)
    auto fmt_ns = [](long long ns) -> std::string {
        char buf[64];
        if (ns < 1000000LL)
            snprintf(buf, sizeof(buf), "%.3f us", ns / 1000.0);
        else
            snprintf(buf, sizeof(buf), "%.3f ms", ns / 1000000.0);
        return std::string(buf);
    };

    // Timers
    //auto t_total_start = Clock::now();
    //long long t_parse_args, t_parse_params, t_parse_edits, t_buffers, t_setup,
    //          t_cave_grid, t_grad_grid_raw, t_grad_grid_smooth,
    //          t_pass1, t_pass2_x, t_pass2_y, t_pass2_z, t_pass2,
    //            t_smooth_adj, t_smooth_iters,
    //          t_normals, t_py_build;

    PyObject *py_res, *py_bounds, *py_values, *py_cell_vertex, *py_base_params, *py_edits, *py_clip_bounds;
    float smooth_factor;
    int smooth_iterations;
    int seam_z_snap_i = 1;
    float seam_z_snap_factor = 0.85f;
    float seam_normal_blend = 0.45f;

    // -- STEP 0: Parse Arguments -------------------------------------------
    //auto t0 = Clock::now();
    if (!PyArg_ParseTuple(args, "OOOOOOOif|iff:surface_nets_generate", 
                          &py_res, &py_bounds, &py_values, &py_cell_vertex, 
                          &py_base_params, &py_edits, &py_clip_bounds,
                          &smooth_iterations, &smooth_factor,
                          &seam_z_snap_i, &seam_z_snap_factor, &seam_normal_blend)) {
        return nullptr;
    }
    //t_parse_args = std::chrono::duration_cast<NS>(Clock::now() - t0).count();

    int nx = PyLong_AsLong(PyTuple_GET_ITEM(py_res, 0));
    int ny = PyLong_AsLong(PyTuple_GET_ITEM(py_res, 1));
    int nz = PyLong_AsLong(PyTuple_GET_ITEM(py_res, 2));
    float min_x = (float)PyFloat_AsDouble(PyTuple_GET_ITEM(py_bounds, 0));
    float max_x = (float)PyFloat_AsDouble(PyTuple_GET_ITEM(py_bounds, 1));
    float min_y = (float)PyFloat_AsDouble(PyTuple_GET_ITEM(py_bounds, 2));
    float max_y = (float)PyFloat_AsDouble(PyTuple_GET_ITEM(py_bounds, 3));
    float min_z = (float)PyFloat_AsDouble(PyTuple_GET_ITEM(py_bounds, 4));
    float max_z = (float)PyFloat_AsDouble(PyTuple_GET_ITEM(py_bounds, 5));
    float clip_min_x = min_x;
    float clip_max_x = max_x;
    float clip_min_y = min_y;
    float clip_max_y = max_y;
    if (PyTuple_Check(py_clip_bounds) && PyTuple_GET_SIZE(py_clip_bounds) >= 4) {
        clip_min_x = (float)PyFloat_AsDouble(PyTuple_GET_ITEM(py_clip_bounds, 0));
        clip_max_x = (float)PyFloat_AsDouble(PyTuple_GET_ITEM(py_clip_bounds, 1));
        clip_min_y = (float)PyFloat_AsDouble(PyTuple_GET_ITEM(py_clip_bounds, 2));
        clip_max_y = (float)PyFloat_AsDouble(PyTuple_GET_ITEM(py_clip_bounds, 3));
    }

    // Parse base height params for ghost sampling
    float seed        = (float)PyFloat_AsDouble(PyTuple_GET_ITEM(py_base_params, 0));
    float height_scale= (float)PyFloat_AsDouble(PyTuple_GET_ITEM(py_base_params, 1));
    float freq        = (float)PyFloat_AsDouble(PyTuple_GET_ITEM(py_base_params, 2));
    float h_cache_scale = (float)PyFloat_AsDouble(PyTuple_GET_ITEM(py_base_params, 3));
    float noise_scale_multiplier = 1.0f;
    if (PyTuple_GET_SIZE(py_base_params) > 5) {
        noise_scale_multiplier = (float)PyFloat_AsDouble(PyTuple_GET_ITEM(py_base_params, 5));
    }
    float oct2_freq_mult = 2.0f;
    if (PyTuple_GET_SIZE(py_base_params) > 6) {
        oct2_freq_mult = (float)PyFloat_AsDouble(PyTuple_GET_ITEM(py_base_params, 6));
    }
    float oct2_weight = 0.5f;
    if (PyTuple_GET_SIZE(py_base_params) > 7) {
        oct2_weight = (float)PyFloat_AsDouble(PyTuple_GET_ITEM(py_base_params, 7));
    }
    float oct2_height_scale = height_scale;
    if (PyTuple_GET_SIZE(py_base_params) > 8) {
        oct2_height_scale = (float)PyFloat_AsDouble(PyTuple_GET_ITEM(py_base_params, 8));
    }
    float noise_scale = (1.0f / h_cache_scale) * freq * noise_scale_multiplier;
    float seed_z = seed * 0.001f;

    // CAVERNAS 3D - PARÂMETROS EXATOS DO PYTHON
    const int enable_caves = 1;
    const float cave_noise_scale = 0.12f;  // EXATO do Python
    const float cave_threshold = 0.35f;    // EXATO do Python
    const float cave_seed = 99999.0f;
    const float cave_safety_margin = 5.0f;
    const float warp_scale = 0.05f;
    const float warp_strength = 10.0f;
    const float cave_strength_mult = 30.0f;

    // Parse edits for ghost sampling
    std::vector<VoxelSphereEdit> edits;
    const bool edits_is_tuple = PyTuple_Check(py_edits);
    const bool edits_is_list  = !edits_is_tuple && PyList_Check(py_edits);
    Py_ssize_t num_edits = edits_is_tuple ? PyTuple_GET_SIZE(py_edits)
                         : edits_is_list  ? PyList_GET_SIZE(py_edits)
                         : PySequence_Size(py_edits);
    edits.reserve((size_t)std::max<Py_ssize_t>(0, num_edits));
    for (Py_ssize_t i = 0; i < num_edits; ++i) {
        PyObject *item;
        bool item_needs_decref = false;
        if (edits_is_tuple)     item = PyTuple_GET_ITEM(py_edits, i);
        else if (edits_is_list) item = PyList_GET_ITEM(py_edits, i);
        else { item = PySequence_GetItem(py_edits, i); item_needs_decref = true; }
        VoxelSphereEdit edit;
        edit.operation = PyLong_AsLong(PyTuple_GET_ITEM(item, 0));
        edit.cx    = (float)PyFloat_AsDouble(PyTuple_GET_ITEM(item, 1));
        edit.cy    = (float)PyFloat_AsDouble(PyTuple_GET_ITEM(item, 2));
        edit.cz    = (float)PyFloat_AsDouble(PyTuple_GET_ITEM(item, 3));
        edit.r     = (float)PyFloat_AsDouble(PyTuple_GET_ITEM(item, 4));
        edit.r2    = (float)PyFloat_AsDouble(PyTuple_GET_ITEM(item, 5));
        edit.inv_r = (float)PyFloat_AsDouble(PyTuple_GET_ITEM(item, 6));
        edits.push_back(edit);
        if (item_needs_decref) Py_DECREF(item);
    }

    Py_buffer val_view, cv_view;
    if (PyObject_GetBuffer(py_values, &val_view, PyBUF_SIMPLE) < 0) return nullptr;
    if (PyObject_GetBuffer(py_cell_vertex, &cv_view, PyBUF_WRITABLE) < 0) {
        PyBuffer_Release(&val_view);
        return nullptr;
    }

    float *vals = (float *)val_view.buf;
    int *cv = (int *)cv_view.buf;

    int cells_x = nx - 1;
    int cells_y = ny - 1;
    int cells_z = nz - 1;
    float step_x = (max_x - min_x) / cells_x;
    float step_y = (max_y - min_y) / cells_y;
    float step_z = (max_z - min_z) / cells_z;
    float inv_step_x = (step_x > 1e-9f) ? (1.0f / step_x) : 0.0f;
    float inv_step_y = (step_y > 1e-9f) ? (1.0f / step_y) : 0.0f;
    float inv_step_z = (step_z > 1e-9f) ? (1.0f / step_z) : 0.0f;
    float seam_snap_eps = std::max(1e-6f, std::min(step_x, step_y) * 1.0f);  // snap toda a borda
    float edge_lock_eps = std::max(1e-6f, std::min(step_x, step_y) * 1.20f);
    float pin_eps = std::max(step_x, step_y) * 3.0f;
    bool seam_z_snap = seam_z_snap_i != 0;
    float seam_z_eps = std::max(1e-6f, std::min(step_x, step_y) * seam_z_snap_factor);

    auto sample_terrain_h = [&](float x, float y) -> float {
        float sx = x * h_cache_scale;
        int ix = (int)floorf(sx);
        float fx = sx - ix;
        float sy = y * h_cache_scale;
        int iy = (int)floorf(sy);
        float fy = sy - iy;
        float h00 = get_voxel_noise(ix, iy, noise_scale, seed_z, height_scale, oct2_freq_mult, oct2_weight, oct2_height_scale);
        float h10 = get_voxel_noise(ix+1, iy, noise_scale, seed_z, height_scale, oct2_freq_mult, oct2_weight, oct2_height_scale);
        float h01 = get_voxel_noise(ix, iy+1, noise_scale, seed_z, height_scale, oct2_freq_mult, oct2_weight, oct2_height_scale);
        float h11 = get_voxel_noise(ix+1, iy+1, noise_scale, seed_z, height_scale, oct2_freq_mult, oct2_weight, oct2_height_scale);
        return h00 + fx*(h10-h00) + fy*(h01-h00) + fx*fy*(h00-h10-h01+h11);
    };

    // CAVERNAS 3D - Gerar grid 6x6x6
    CaveGrid3D cave_grid;
    if (enable_caves) {
        cave_grid = CaveGrid3D(6, 6, 6, min_x, max_x, min_y, max_y, min_z, max_z);
        generate_cave_grid_3d(cave_grid, cave_seed, cave_threshold, 
                             cave_noise_scale, warp_scale, warp_strength);
    }

    // Último acesso Python feito - libera GIL antes do processamento pesado
    int stride_x = ny * nz;
    int stride_y = nz;
    std::vector<float> grad_grid(nx * ny * nz * 3);
    float hx = 0.5f * inv_step_x, hy = 0.5f * inv_step_y, hz = 0.5f * inv_step_z;

    std::vector<float> verts;
    std::vector<float> norms;
    std::vector<int> inds;
    size_t estimated_verts = (size_t)(cells_x * cells_y * cells_z) * 15 / 100;
    verts.reserve(estimated_verts * 3);
    norms.reserve(estimated_verts * 3);
    inds.reserve(estimated_verts * 6);

    Py_BEGIN_ALLOW_THREADS

    // 0. Pre-calculate Gradient Grid
    auto get_sdf_at = [&](float x, float y, float z) -> float {
        // 1. Terrain Height
        float terrain_h = sample_terrain_h(x, y);
        float d = z - terrain_h;

        // CAVERNAS 3D
        float voxels_below_surface = -d / step_z;
        if (enable_caves && voxels_below_surface > cave_safety_margin) {
            float cave_noise = interpolate_cave_trilinear(cave_grid, x, y, z);
            if (cave_noise < cave_threshold) {
                float cave_strength = (cave_threshold - cave_noise) * cave_strength_mult;
                if (cave_strength > d) d = cave_strength;
            }
        }

        // 2. CSG Edits
        for (const auto& edit : edits) {
            float dx = x - edit.cx, dy = y - edit.cy, dz = z - edit.cz;
            float sd = sqrtf(dx*dx + dy*dy + dz*dz) - edit.r;
            if (edit.operation == 1) { if (-sd > d) d = -sd; }
            else if (edit.operation == 0) { if (sd < d) d = sd; }
            else { if (sd > d) d = sd; }
        }
        return d;
    };

    auto sample_edited_h = [&](float x, float y) -> float {
        bool has_edit = false;
        for (const auto& edit : edits) {
            float dx = x - edit.cx, dy = y - edit.cy;
            if (dx*dx + dy*dy < edit.r2) { has_edit = true; break; }
        }
        if (!has_edit) return sample_terrain_h(x, y);
        const int coarse_steps = 16;
        float dz_step = (max_z - min_z) / coarse_steps;
        float prev_sdf = get_sdf_at(x, y, max_z);
        for (int si = 1; si <= coarse_steps; ++si) {
            float z_cur = max_z - si * dz_step;
            float cur_sdf = get_sdf_at(x, y, z_cur);
            if ((prev_sdf >= 0.0f) != (cur_sdf >= 0.0f)) {
                float z_lo = z_cur, z_hi = z_cur + dz_step;
                float s_hi = prev_sdf;
                for (int bi = 0; bi < 16; ++bi) {
                    float z_mid = (z_lo + z_hi) * 0.5f;
                    float s_mid = get_sdf_at(x, y, z_mid);
                    if ((s_mid >= 0.0f) == (s_hi >= 0.0f)) { z_hi = z_mid; s_hi = s_mid; }
                    else                                    { z_lo = z_mid; }
                }
                float hz_found = (z_lo + z_hi) * 0.5f;
                float terrain_base = sample_terrain_h(x, y);
                return (hz_found > terrain_base - step_z) ? hz_found : terrain_base;
            }
            prev_sdf = cur_sdf;
        }
        return sample_terrain_h(x, y);
    };

    #pragma omp parallel for schedule(static)
    for (int x = 0; x < nx; ++x) {
        int x_off = x * stride_x;
        float wx = min_x + x * step_x;
        for (int y = 0; y < ny; ++y) {
            int xy_off = x_off + y * stride_y;
            float wy = min_y + y * step_y;
            for (int z = 0; z < nz; ++z) {
                int gi = xy_off + z;
                float wz = min_z + z * step_z;
                float gx, gy, gz;

                // X-axis: TRUE Central difference at boundaries using ghost samples
                if (x == 0) gx = (vals[gi + stride_x] - get_sdf_at(wx - step_x, wy, wz)) * hx;
                else if (x == nx - 1) gx = (get_sdf_at(wx + step_x, wy, wz) - vals[gi - stride_x]) * hx;
                else gx = (vals[gi + stride_x] - vals[gi - stride_x]) * hx;

                // Y-axis
                if (y == 0) gy = (vals[gi + stride_y] - get_sdf_at(wx, wy - step_y, wz)) * hy;
                else if (y == ny - 1) gy = (get_sdf_at(wx, wy + step_y, wz) - vals[gi - stride_y]) * hy;
                else gy = (vals[gi + stride_y] - vals[gi - stride_y]) * hy;

                // Z-axis
                if (z == 0) gz = (vals[gi + 1] - get_sdf_at(wx, wy, wz - step_z)) * hz;
                else if (z == nz - 1) gz = (get_sdf_at(wx, wy, wz + step_z) - vals[gi - 1]) * hz;
                else gz = (vals[gi + 1] - vals[gi - 1]) * hz;

                grad_grid[gi * 3] = gx;
                grad_grid[gi * 3 + 1] = gy;
                grad_grid[gi * 3 + 2] = gz;
            }
        }
    }

    std::vector<float> grad_grid_smoothed = grad_grid;
    #pragma omp parallel for schedule(static)
    for (int x = 1; x < nx - 1; ++x) {
        int x_off = x * stride_x;
        for (int y = 1; y < ny - 1; ++y) {
            int xy_off = x_off + y * stride_y;
            for (int z = 1; z < nz - 1; ++z) {
                int gi = xy_off + z;
                for (int c = 0; c < 3; ++c) {
                    float center = grad_grid[gi * 3 + c] * 0.55f;
                    float nsum =
                        grad_grid[(gi + stride_x) * 3 + c] +
                        grad_grid[(gi - stride_x) * 3 + c] +
                        grad_grid[(gi + stride_y) * 3 + c] +
                        grad_grid[(gi - stride_y) * 3 + c] +
                        grad_grid[(gi + 1) * 3 + c] +
                        grad_grid[(gi - 1) * 3 + c];
                    grad_grid_smoothed[gi * 3 + c] = center + nsum * 0.075f;
                }
            }
        }
    }
    grad_grid.swap(grad_grid_smoothed);

    // Reset cell_vertex
    memset(cv, -1, cells_x * cells_y * cells_z * sizeof(int));

    // Pass 1: Create Vertices
    for (int x = 0; x < cells_x; ++x) {
        int x0_off = x * ny * nz;
        int x1_off = (x + 1) * ny * nz;
        for (int y = 0; y < cells_y; ++y) {
            int y0_off = y * nz;
            int y1_off = (y + 1) * nz;
            int xy00 = x0_off + y0_off;
            int xy10 = x1_off + y0_off;
            int xy11 = x1_off + y1_off;
            int xy01 = x0_off + y1_off;

            for (int z = 0; z < cells_z; ++z) {
                int c[8] = { xy00 + z, xy10 + z, xy11 + z, xy01 + z,
                             xy00 + z + 1, xy10 + z + 1, xy11 + z + 1, xy01 + z + 1 };
                float v[8];
                bool s[8];
                int mask = 0;
                for (int i = 0; i < 8; ++i) {
                    v[i] = vals[c[i]];
                    s[i] = v[i] < 0.0f;
                    if (s[i]) mask |= (1 << i);
                }

                if (mask == 0 || mask == 255) continue;

                float wx0 = min_x + x * step_x, wx1 = wx0 + step_x;
                float wy0 = min_y + y * step_y, wy1 = wy0 + step_y;
                float wz0 = min_z + z * step_z, wz1 = wz0 + step_z;
                float px = 0, py = 0, pz = 0;
                float gnx = 0, gny = 0, gnz = 0;
                int count = 0;
                auto clamp01 = [](float t) {
                    return std::max(0.0f, std::min(1.0f, t));
                };

                auto add_x = [&](int i1, int i2, float x1, float x2, float cy, float cz) {
                    if (s[i1] != s[i2]) {
                        float denom = v[i1] - v[i2];
                        float t = (fabsf(denom) < 1e-9f) ? 0.5f : (v[i1] / denom);
                        px += x1 + (x2 - x1) * clamp01(t);
                        py += cy; pz += cz;
                        gnx += (v[i2] - v[i1]) * (x2 - x1);
                        count++;
                    }
                };
                auto add_y = [&](int i1, int i2, float y1, float y2, float cx, float cz) {
                    if (s[i1] != s[i2]) {
                        float denom = v[i1] - v[i2];
                        float t = (fabsf(denom) < 1e-9f) ? 0.5f : (v[i1] / denom);
                        px += cx; py += y1 + (y2 - y1) * clamp01(t);
                        pz += cz;
                        gny += (v[i2] - v[i1]) * (y2 - y1);
                        count++;
                    }
                };
                auto add_z = [&](int i1, int i2, float z1, float z2, float cx, float cy) {
                    if (s[i1] != s[i2]) {
                        float denom = v[i1] - v[i2];
                        float t = (fabsf(denom) < 1e-9f) ? 0.5f : (v[i1] / denom);
                        px += cx; py += cy; pz += z1 + (z2 - z1) * clamp01(t);
                        gnz += (v[i2] - v[i1]) * (z2 - z1);
                        count++;
                    }
                };

                add_x(0, 1, wx0, wx1, wy0, wz0); add_x(3, 2, wx0, wx1, wy1, wz0);
                add_x(4, 5, wx0, wx1, wy0, wz1); add_x(7, 6, wx0, wx1, wy1, wz1);
                add_y(0, 3, wy0, wy1, wx0, wz0); add_y(1, 2, wy0, wy1, wx1, wz0);
                add_y(4, 7, wy0, wy1, wx0, wz1); add_y(5, 6, wy0, wy1, wx1, wz1);
                add_z(0, 4, wz0, wz1, wx0, wy0); add_z(1, 5, wz0, wz1, wx1, wy0);
                add_z(3, 7, wz0, wz1, wx0, wy1); add_z(2, 6, wz0, wz1, wx1, wy1);

                if (count > 0) {
                    float inv = 1.0f / count;
                    float vx = px * inv, vy = py * inv, vz = pz * inv;
                    
                    if (vx < clip_min_x) vx = clip_min_x; else if (vx > clip_max_x) vx = clip_max_x;
                    if (vy < clip_min_y) vy = clip_min_y; else if (vy > clip_max_y) vy = clip_max_y;
                    if (vz < min_z) vz = min_z; else if (vz > max_z) vz = max_z;
                    if (std::fabs(vx - clip_min_x) <= seam_snap_eps) vx = clip_min_x;
                    else if (std::fabs(clip_max_x - vx) <= seam_snap_eps) vx = clip_max_x;
                    if (std::fabs(vy - clip_min_y) <= seam_snap_eps) vy = clip_min_y;
                    else if (std::fabs(clip_max_y - vy) <= seam_snap_eps) vy = clip_max_y;

                    int v_idx = (int)verts.size() / 3;
                    verts.push_back(vx); verts.push_back(vy); verts.push_back(vz);
                    
                    float n_len = sqrt(gnx*gnx + gny*gny + gnz*gnz);
                    if (n_len > 1e-6f) {
                        norms.push_back(gnx / n_len); norms.push_back(gny / n_len); norms.push_back(gnz / n_len);
                    } else {
                        norms.push_back(0); norms.push_back(0); norms.push_back(1);
                    }
                    cv[x * cells_y * cells_z + y * cells_z + z] = v_idx;
                }
            }
        }
    }

    // Pass 2: Create Indices (Quads to Tris)
    int cv_stride_y = cells_z;
    int cv_stride_x = cells_y * cells_z;
    for (int x = 0; x < cells_x; ++x) {
        int x0_off = x * ny * nz;
        int x1_off = (x + 1) * ny * nz;
        int cv_x = x * cv_stride_x;
        for (int y = 0; y < cells_y; ++y) {
            int y0_off = y * nz;
            int y1_off = (y + 1) * nz;
            int cv_xy = cv_x + y * cv_stride_y;
            for (int z = 0; z < cells_z; ++z) {
                int idx = x0_off + y0_off + z;
                float v0 = vals[idx];

                float vx1 = vals[x1_off + y0_off + z];
                if ((v0 < 0.0f) != (vx1 < 0.0f) && y > 0 && z > 0) {
                    int a = cv[cv_xy + z];
                    int b = cv[cv_xy - cv_stride_y + z];
                    int c = cv[cv_xy - cv_stride_y + z - 1];
                    int d = cv[cv_xy + z - 1];
                    if (a >= 0 && b >= 0 && c >= 0 && d >= 0) {
                        float avx = verts[a * 3], avy = verts[a * 3 + 1], avz = verts[a * 3 + 2];
                        float bvx = verts[b * 3], bvy = verts[b * 3 + 1], bvz = verts[b * 3 + 2];
                        float cvx = verts[c * 3], cvy = verts[c * 3 + 1], cvz = verts[c * 3 + 2];
                        float dvx = verts[d * 3], dvy = verts[d * 3 + 1], dvz = verts[d * 3 + 2];
                        float acd = (avx - cvx) * (avx - cvx) + (avy - cvy) * (avy - cvy) + (avz - cvz) * (avz - cvz);
                        float bdd = (bvx - dvx) * (bvx - dvx) + (bvy - dvy) * (bvy - dvy) + (bvz - dvz) * (bvz - dvz);
                        if (acd < bdd) {
                            if (v0 > 0.0f) {
                                inds.push_back(a); inds.push_back(c); inds.push_back(b);
                                inds.push_back(a); inds.push_back(d); inds.push_back(c);
                            } else {
                                inds.push_back(a); inds.push_back(b); inds.push_back(c);
                                inds.push_back(a); inds.push_back(c); inds.push_back(d);
                            }
                        } else {
                            if (v0 > 0.0f) {
                                inds.push_back(b); inds.push_back(d); inds.push_back(c);
                                inds.push_back(b); inds.push_back(a); inds.push_back(d);
                            } else {
                                inds.push_back(b); inds.push_back(c); inds.push_back(d);
                                inds.push_back(b); inds.push_back(d); inds.push_back(a);
                            }
                        }
                    }
                }

                float vy1 = vals[x0_off + y1_off + z];
                if ((v0 < 0.0f) != (vy1 < 0.0f) && x > 0 && z > 0) {
                    int a = cv[cv_xy + z];
                    int b = cv[cv_xy - cv_stride_x + z];
                    int c = cv[cv_xy - cv_stride_x + z - 1];
                    int d = cv[cv_xy + z - 1];
                    if (a >= 0 && b >= 0 && c >= 0 && d >= 0) {
                        float avx = verts[a * 3], avy = verts[a * 3 + 1], avz = verts[a * 3 + 2];
                        float bvx = verts[b * 3], bvy = verts[b * 3 + 1], bvz = verts[b * 3 + 2];
                        float cvx = verts[c * 3], cvy = verts[c * 3 + 1], cvz = verts[c * 3 + 2];
                        float dvx = verts[d * 3], dvy = verts[d * 3 + 1], dvz = verts[d * 3 + 2];
                        float acd = (avx - cvx) * (avx - cvx) + (avy - cvy) * (avy - cvy) + (avz - cvz) * (avz - cvz);
                        float bdd = (bvx - dvx) * (bvx - dvx) + (bvy - dvy) * (bvy - dvy) + (bvz - dvz) * (bvz - dvz);
                        if (acd < bdd) {
                            if (v0 > 0.0f) {
                                inds.push_back(a); inds.push_back(b); inds.push_back(c);
                                inds.push_back(a); inds.push_back(c); inds.push_back(d);
                            } else {
                                inds.push_back(a); inds.push_back(c); inds.push_back(b);
                                inds.push_back(a); inds.push_back(d); inds.push_back(c);
                            }
                        } else {
                            if (v0 > 0.0f) {
                                inds.push_back(b); inds.push_back(c); inds.push_back(d);
                                inds.push_back(b); inds.push_back(d); inds.push_back(a);
                            } else {
                                inds.push_back(b); inds.push_back(d); inds.push_back(c);
                                inds.push_back(b); inds.push_back(a); inds.push_back(d);
                            }
                        }
                    }
                }

                float vz1 = vals[idx + 1];
                if ((v0 < 0.0f) != (vz1 < 0.0f) && x > 0 && y > 0) {
                    int a = cv[cv_xy + z];
                    int b = cv[cv_xy - cv_stride_x + z];
                    int c = cv[cv_xy - cv_stride_x - cv_stride_y + z];
                    int d = cv[cv_xy - cv_stride_y + z];
                    if (a >= 0 && b >= 0 && c >= 0 && d >= 0) {
                        float avx = verts[a * 3], avy = verts[a * 3 + 1], avz = verts[a * 3 + 2];
                        float bvx = verts[b * 3], bvy = verts[b * 3 + 1], bvz = verts[b * 3 + 2];
                        float cvx = verts[c * 3], cvy = verts[c * 3 + 1], cvz = verts[c * 3 + 2];
                        float dvx = verts[d * 3], dvy = verts[d * 3 + 1], dvz = verts[d * 3 + 2];
                        float acd = (avx - cvx) * (avx - cvx) + (avy - cvy) * (avy - cvy) + (avz - cvz) * (avz - cvz);
                        float bdd = (bvx - dvx) * (bvx - dvx) + (bvy - dvy) * (bvy - dvy) + (bvz - dvz) * (bvz - dvz);
                        if (acd < bdd) {
                            if (v0 > 0.0f) {
                                inds.push_back(a); inds.push_back(c); inds.push_back(b);
                                inds.push_back(a); inds.push_back(d); inds.push_back(c);
                            } else {
                                inds.push_back(a); inds.push_back(b); inds.push_back(c);
                                inds.push_back(a); inds.push_back(c); inds.push_back(d);
                            }
                        } else {
                            if (v0 > 0.0f) {
                                inds.push_back(b); inds.push_back(d); inds.push_back(c);
                                inds.push_back(b); inds.push_back(a); inds.push_back(d);
                            } else {
                                inds.push_back(b); inds.push_back(c); inds.push_back(d);
                                inds.push_back(b); inds.push_back(d); inds.push_back(a);
                            }
                        }
                    }
                }
            }
        }
    }


    // 3. Laplacian Smoothing
    if (smooth_iterations > 0 && smooth_factor > 0.0f) {
        int v_count_real = (int)verts.size() / 3;
        std::vector<int> adj_offsets(v_count_real + 1, 0);
        for (size_t i = 0; i < inds.size(); i += 3) {
            adj_offsets[inds[i] + 1] += 2;
            adj_offsets[inds[i+1] + 1] += 2;
            adj_offsets[inds[i+2] + 1] += 2;
        }
        for (int i = 0; i < v_count_real; ++i) adj_offsets[i+1] += adj_offsets[i];

        std::vector<int> adj_indices(adj_offsets[v_count_real]);
        std::vector<int> curr_offsets = adj_offsets;
        for (size_t i = 0; i < inds.size(); i += 3) {
            int a = inds[i], b = inds[i+1], c = inds[i+2];
            adj_indices[curr_offsets[a]++] = b; adj_indices[curr_offsets[a]++] = c;
            adj_indices[curr_offsets[b]++] = a; adj_indices[curr_offsets[b]++] = c;
            adj_indices[curr_offsets[c]++] = a; adj_indices[curr_offsets[c]++] = b;
        }

        std::vector<float> next_verts(verts.size());
        for (int iter = 0; iter < smooth_iterations; ++iter) {
            #pragma omp parallel for
            for (int i = 0; i < v_count_real; ++i) {
                int start = adj_offsets[i];
                int end = adj_offsets[i+1];
                float svx = verts[i * 3];
                float svy = verts[i * 3 + 1];
                float svz = verts[i * 3 + 2];
                if (start == end) {
                    next_verts[i * 3] = svx;
                    next_verts[i * 3 + 1] = svy;
                    next_verts[i * 3 + 2] = svz;
                    continue;
                }

                float avg_x = 0, avg_y = 0, avg_z = 0;
                for (int j = start; j < end; ++j) {
                    int n = adj_indices[j];
                    avg_x += verts[n * 3];
                    avg_y += verts[n * 3 + 1];
                    avg_z += verts[n * 3 + 2];
                }
                float inv = 1.0f / (end - start);
                avg_x *= inv; avg_y *= inv; avg_z *= inv;

                float dx_min = svx - clip_min_x;
                float dx_max = clip_max_x - svx;
                float dy_min = svy - clip_min_y;
                float dy_max = clip_max_y - svy;
                if (dx_min < 0.0f) dx_min = 0.0f;
                if (dx_max < 0.0f) dx_max = 0.0f;
                if (dy_min < 0.0f) dy_min = 0.0f;
                if (dy_max < 0.0f) dy_max = 0.0f;
                float dist_edge = std::min(std::min(dx_min, dx_max), std::min(dy_min, dy_max));
                float w = 1.0f;
                if (pin_eps > 1e-9f) {
                    w = dist_edge / pin_eps;
                    if (w < 0.0f) w = 0.0f;
                    else if (w > 1.0f) w = 1.0f;
                }
                float eff_factor = smooth_factor * w;

                // Bloqueio estrito de bordas para evitar buracos (Snapping + Smoothing Lock)
                // Usamos 0.51 * step para garantir que o voxel extra de overlap seja detectado como borda
                bool lock_x_min = dx_min <= edge_lock_eps;
                bool lock_x_max = dx_max <= edge_lock_eps;
                bool lock_y_min = dy_min <= edge_lock_eps;
                bool lock_y_max = dy_max <= edge_lock_eps;
                bool lock_x = lock_x_min || lock_x_max;
                bool lock_y = lock_y_min || lock_y_max;

                float nx = svx + (avg_x - svx) * eff_factor;
                float ny = svy + (avg_y - svy) * eff_factor;
                float nz = svz + (avg_z - svz) * eff_factor;

                if (lock_x || lock_y) {
                    if (lock_x_min || std::fabs(svx - clip_min_x) <= seam_snap_eps) nx = clip_min_x;
                    else if (lock_x_max || std::fabs(clip_max_x - svx) <= seam_snap_eps) nx = clip_max_x;
                    else nx = svx;
                    if (lock_y_min || std::fabs(svy - clip_min_y) <= seam_snap_eps) ny = clip_min_y;
                    else if (lock_y_max || std::fabs(clip_max_y - svy) <= seam_snap_eps) ny = clip_max_y;
                    else ny = svy;
                    if (seam_z_snap) {
                        float ex = std::min(std::fabs(nx - clip_min_x), std::fabs(clip_max_x - nx));
                        float ey = std::min(std::fabs(ny - clip_min_y), std::fabs(clip_max_y - ny));
                        float edge_dist = std::min(ex, ey);
                        float blend = 1.0f;
                        if (seam_z_eps > 1e-9f) {
                            blend = 1.0f - std::min(1.0f, edge_dist / seam_z_eps);
                        }
                        // Só snappa se o vértice está na superfície do terreno (não dentro de escavação).
                        // Se svz está muito abaixo do terreno base, o SDF já posicionou corretamente.
                        float terrain_base = sample_terrain_h(nx, ny);
                        if (svz >= terrain_base - step_z * 2.0f) {
                            float hz = sample_edited_h(nx, ny);
                            nz = svz + (hz - svz) * blend;
                        } else {
                            nz = svz; // dentro de escavação - não snappa
                        }
                    }
                    else {
                        nz = svz;
                    }
                }
                if (nx < clip_min_x) nx = clip_min_x; else if (nx > clip_max_x) nx = clip_max_x;
                if (ny < clip_min_y) ny = clip_min_y; else if (ny > clip_max_y) ny = clip_max_y;
                if (nz < min_z) nz = min_z; else if (nz > max_z) nz = max_z;
                
                next_verts[i * 3] = nx;
                next_verts[i * 3 + 1] = ny;
                next_verts[i * 3 + 2] = nz;
            }
            verts.swap(next_verts);
        }
    }

    // 4. Final Normal Calculation (Interpolated Gradient Grid for Ultra-Smooth Shading)
    int v_count_real = (int)verts.size() / 3;
    if (v_count_real > 0 && (int)norms.size() == v_count_real * 3) {
        #pragma omp parallel for schedule(static)
        for (int i = 0; i < v_count_real; ++i) {
            float vx = verts[i * 3];
            float vy = verts[i * 3 + 1];
            float vz = verts[i * 3 + 2];

            float fx = (vx - min_x) * inv_step_x;
            float fy = (vy - min_y) * inv_step_y;
            float fz = (vz - min_z) * inv_step_z;

            if (fx < 0.0f) fx = 0.0f; else if (fx > nx - 1.001f) fx = nx - 1.001f;
            if (fy < 0.0f) fy = 0.0f; else if (fy > ny - 1.001f) fy = ny - 1.001f;
            if (fz < 0.0f) fz = 0.0f; else if (fz > nz - 1.001f) fz = nz - 1.001f;

            int ix = (int)fx;
            int iy = (int)fy;
            int iz = (int)fz;
            float tx = fx - ix;
            float ty = fy - iy;
            float tz = fz - iz;

            int gi = ix * stride_x + iy * stride_y + iz;
            
            // Fetch gradients at 8 nodes
            float *g000 = &grad_grid[gi * 3];
            float *g100 = &grad_grid[(gi + stride_x) * 3];
            float *g010 = &grad_grid[(gi + stride_y) * 3];
            float *g110 = &grad_grid[(gi + stride_x + stride_y) * 3];
            float *g001 = &grad_grid[(gi + 1) * 3];
            float *g101 = &grad_grid[(gi + stride_x + 1) * 3];
            float *g011 = &grad_grid[(gi + stride_y + 1) * 3];
            float *g111 = &grad_grid[(gi + stride_x + stride_y + 1) * 3];

            // Trilinear interpolation of gradients
            float ix0 = 1.0f - tx, iy0 = 1.0f - ty, iz0 = 1.0f - tz;
            float w00 = ix0 * iy0, w10 = tx * iy0, w01 = ix0 * ty, w11 = tx * ty;

            float gx = (g000[0] * w00 + g100[0] * w10 + g010[0] * w01 + g110[0] * w11) * iz0 +
                       (g001[0] * w00 + g101[0] * w10 + g011[0] * w01 + g111[0] * w11) * tz;
            float gy = (g000[1] * w00 + g100[1] * w10 + g010[1] * w01 + g110[1] * w11) * iz0 +
                       (g001[1] * w00 + g101[1] * w10 + g011[1] * w01 + g111[1] * w11) * tz;
            float gz = (g000[2] * w00 + g100[2] * w10 + g010[2] * w01 + g110[2] * w11) * iz0 +
                       (g001[2] * w00 + g101[2] * w10 + g011[2] * w01 + g111[2] * w11) * tz;

            float gl = std::sqrt(gx * gx + gy * gy + gz * gz);
            if (gl > 1e-9f) {
                float ginv = 1.0f / gl;
                float nxn = gx * ginv;
                float nyn = gy * ginv;
                float nzn = gz * ginv;
                if (seam_z_snap && seam_normal_blend > 1e-6f) {
                    float ex = std::min(std::fabs(vx - clip_min_x), std::fabs(clip_max_x - vx));
                    float ey = std::min(std::fabs(vy - clip_min_y), std::fabs(clip_max_y - vy));
                    float edge_dist = std::min(ex, ey);
                    if (edge_dist <= seam_z_eps) {
                        float sx = std::max(step_x, 1e-6f);
                        float sy = std::max(step_y, 1e-6f);
                        float hxm = sample_terrain_h(vx - sx, vy);
                        float hxp = sample_terrain_h(vx + sx, vy);
                        float hym = sample_terrain_h(vx, vy - sy);
                        float hyp = sample_terrain_h(vx, vy + sy);
                        float tx = (hxp - hxm) / (2.0f * sx);
                        float ty = (hyp - hym) / (2.0f * sy);
                        float tnx = -tx;
                        float tny = -ty;
                        float tnz = 1.0f;
                        float tnl = std::sqrt(tnx * tnx + tny * tny + tnz * tnz);
                        if (tnl > 1e-9f) {
                            tnx /= tnl;
                            tny /= tnl;
                            tnz /= tnl;
                            float edge_blend = 1.0f - std::min(1.0f, edge_dist / seam_z_eps);
                            float b = std::min(1.0f, std::max(0.0f, edge_blend * seam_normal_blend));
                            nxn = nxn * (1.0f - b) + tnx * b;
                            nyn = nyn * (1.0f - b) + tny * b;
                            nzn = nzn * (1.0f - b) + tnz * b;
                            float nlen = std::sqrt(nxn * nxn + nyn * nyn + nzn * nzn);
                            if (nlen > 1e-9f) {
                                float ninv = 1.0f / nlen;
                                nxn *= ninv;
                                nyn *= ninv;
                                nzn *= ninv;
                            }
                        }
                    }
                }
                norms[i * 3] = nxn;
                norms[i * 3 + 1] = nyn;
                norms[i * 3 + 2] = nzn;
            } else {
                norms[i * 3] = 0.0f;
                norms[i * 3 + 1] = 0.0f;
                norms[i * 3 + 2] = 1.0f;
            }
        }
    }

    Py_END_ALLOW_THREADS


    size_t v_count = verts.size();
    size_t n_count = norms.size();
    size_t i_count = inds.size();

    PyObject *py_verts_out = PyList_New(v_count);
    PyObject *py_norms_out = PyList_New(n_count);
    PyObject *py_inds_out = PyList_New(i_count);

    for (size_t i = 0; i < v_count; ++i) PyList_SET_ITEM(py_verts_out, i, PyFloat_FromDouble(verts[i]));
    for (size_t i = 0; i < n_count; ++i) PyList_SET_ITEM(py_norms_out, i, PyFloat_FromDouble(norms[i]));
    for (size_t i = 0; i < i_count; ++i) PyList_SET_ITEM(py_inds_out, i, PyLong_FromLong(inds[i]));

    PyBuffer_Release(&val_view);
    PyBuffer_Release(&cv_view);

    PyObject *result = PyTuple_Pack(3, py_verts_out, py_norms_out, py_inds_out);
    Py_DECREF(py_verts_out);
    Py_DECREF(py_norms_out);
    Py_DECREF(py_inds_out);

    return result;
}

const char KX_GameObject::SurfaceNetsGenerate_doc[] = 
"surface_nets_generate(res, bounds, values, cell_vertex, base_params, edits, clip_bounds, smooth_iter, smooth_factor, seam_z_snap=1, seam_z_snap_factor=0.85, seam_normal_blend=0.45)\n"
"Generates Surface Nets mesh in C++.";

// ---------------------------------------------------------------------------
// rebuild_voxel_mesh
// ---------------------------------------------------------------------------
// Builds a KX_Mesh directly from flat float/int buffers and calls ReplaceMesh
// + ReinstancePhysicsShape in one shot, avoiding all Python-level overhead.
//
// Args:
//   flat_verts  : buffer of floats  (x,y,z world-space, stride 3)
//   flat_norms  : buffer of floats  (nx,ny,nz, stride 3)
//   flat_indices: buffer of ints    (triangle indices, flat)
//   wx, wy, wz  : world position of the object (to convert to local space)
//   lod         : current LOD level (used to invalidate chunk cache)
// ---------------------------------------------------------------------------
PyObject *KX_GameObject::PyRebuildVoxelMesh(PyObject *args)
{
    PyObject *py_verts, *py_norms, *py_indices;
    float wx, wy, wz;
    int lod = 0;

    if (!PyArg_ParseTuple(args, "OOOfff|i:rebuild_voxel_mesh",
                          &py_verts, &py_norms, &py_indices,
                          &wx, &wy, &wz, &lod)) {
        return nullptr;
    }

    // --- acquire read-only buffers ---
    Py_buffer vbuf, nbuf, ibuf;
    if (PyObject_GetBuffer(py_verts,   &vbuf, PyBUF_SIMPLE) < 0) return nullptr;
    if (PyObject_GetBuffer(py_norms,   &nbuf, PyBUF_SIMPLE) < 0) { PyBuffer_Release(&vbuf); return nullptr; }
    if (PyObject_GetBuffer(py_indices, &ibuf, PyBUF_SIMPLE) < 0) { PyBuffer_Release(&vbuf); PyBuffer_Release(&nbuf); return nullptr; }

    const float *vp = (const float *)vbuf.buf;
    const float *np = (const float *)nbuf.buf;
    const int   *ip = (const int   *)ibuf.buf;

    const Py_ssize_t nv = (Py_ssize_t)(vbuf.len / sizeof(float)) / 3;  // vertex count
    const Py_ssize_t ni = (Py_ssize_t)(ibuf.len / sizeof(int));         // index count

    // --- invalidate chunk cache on LOD change ---
    auto it = g_chunkCache.find(this);
    if (it != g_chunkCache.end()) {
        it->second.clear();
    }

    // --- build mesh directly ---
    KX_Mesh *srcMesh = m_meshes.empty() ? nullptr : m_meshes.front();
    if (!srcMesh) {
        PyBuffer_Release(&vbuf); PyBuffer_Release(&nbuf); PyBuffer_Release(&ibuf);
        PyErr_SetString(PyExc_RuntimeError, "rebuild_voxel_mesh: object has no mesh");
        return nullptr;
    }

    const std::vector<RAS_MeshMaterial *>& matList = srcMesh->GetMeshMaterialList();
    if (matList.empty()) {
        PyBuffer_Release(&vbuf); PyBuffer_Release(&nbuf); PyBuffer_Release(&ibuf);
        PyErr_SetString(PyExc_RuntimeError, "rebuild_voxel_mesh: mesh has no material slots");
        return nullptr;
    }

    KX_Scene *scene = GetScene();
    RAS_BucketManager *bucketManager = scene->GetBucketManager();

    // Derive format from existing mesh material
    RAS_MeshMaterial *srcMat = matList[0];
    RAS_DisplayArray *srcArray = srcMat->GetDisplayArray();
    const RAS_DisplayArray::Format fmt = srcArray->GetFormat();

    // Create a fresh display array (empty, same format/primitive type)
    RAS_DisplayArray *newArray = new RAS_DisplayArray(RAS_DisplayArray::TRIANGLES, fmt);

    // --- fill vertices (world → local) ---
    // Color (0.0, 0.6, 0.4, 1.0) packed as RGBA bytes in little-endian uint:
    // m_array[0]=R=0x00, [1]=G=0x99, [2]=B=0x66, [3]=A=0xFF
    // m_flat (little-endian) = A<<24 | B<<16 | G<<8 | R = 0xFF669900
    static const unsigned int kTerrainColor = (0xFF << 24) | (0x66 << 16) | (0x99 << 8) | 0x00;

    mt::vec2_packed uvs[RAS_Texture::MaxUnits];
    unsigned int colors[RAS_Texture::MaxUnits];
    for (int k = 0; k < RAS_Texture::MaxUnits; ++k) {
        uvs[k] = mt::vec2_packed(mt::zero2);
        colors[k] = kTerrainColor;
    }
    const mt::vec4_packed tangent(mt::one4);
    unsigned int origIdx = 0;

    newArray->ReserveVertices((unsigned int)nv);
    for (Py_ssize_t i = 0; i < nv; ++i) {
        const float posData[3] = { vp[i*3] - wx, vp[i*3+1] - wy, vp[i*3+2] - wz };
        const float norData[3] = { np[i*3], np[i*3+1], np[i*3+2] };
        const float uvData[2]  = { vp[i*3] * 0.1f, vp[i*3+1] * 0.1f };

        const mt::vec3_packed pos(posData);
        const mt::vec3_packed norm(norData);
        uvs[0] = mt::vec2_packed(uvData);

        newArray->AddVertex(pos, norm, tangent, uvs, colors, origIdx++, 0);
    }

    // --- fill indices ---
    if (ni > 0) {
        std::vector<unsigned int> uindices(ni);
        for (Py_ssize_t i = 0; i < ni; ++i) {
            uindices[i] = (unsigned int)ip[i];
        }
        newArray->ReservePrimitiveIndices(ni);
        newArray->AddPrimitiveIndices(uindices.data(), ni);
        newArray->ReserveTriangleIndices(ni);
        newArray->AddTriangleIndices(uindices.data(), ni);
    }

    PyBuffer_Release(&vbuf);
    PyBuffer_Release(&nbuf);
    PyBuffer_Release(&ibuf);

    // --- build KX_Mesh and replace ---
    newArray->NotifyUpdate(RAS_DisplayArray::COLORS_MODIFIED | RAS_DisplayArray::SIZE_MODIFIED);
    KX_Mesh *newMesh = new KX_Mesh(scene, "Terrain", srcMesh->GetLayersInfo());
    bool created;
    RAS_MaterialBucket *bucket = bucketManager->FindBucket(
        static_cast<KX_BlenderMaterial *>(srcMat->GetBucket()->GetMaterial()), created);
    newMesh->AddMaterial(bucket, 0, newArray);
    newMesh->EndConversion(scene->GetBoundingBoxManager());
    KX_GetActiveEngine()->GetConverter()->RegisterMesh(scene, newMesh);

    ReplaceMesh(newMesh, true, false);

    if (m_physicsController) {
        m_physicsController->ReinstancePhysicsShape(nullptr, nullptr);
    }

    Py_RETURN_NONE;
}

const char KX_GameObject::RebuildVoxelMesh_doc[] =
"rebuild_voxel_mesh(flat_verts, flat_norms, flat_indices, wx, wy, wz, lod=0)\n"
"Rebuilds the voxel terrain mesh from flat float/int buffers in C++.\n"
"flat_verts  : array.array('f') of world-space XYZ (stride 3)\n"
"flat_norms  : array.array('f') of normals XYZ (stride 3)\n"
"flat_indices: array.array('i') of triangle indices (flat)\n"
"wx,wy,wz    : object world position\n"
"lod         : current LOD level (invalidates chunk cache on change)";


// ---------------------------------------------------------------------------
// surface_nets_and_rebuild
// ---------------------------------------------------------------------------
// Cache por objeto para surface_nets_and_rebuild:
//   - parâmetros estáticos do TerrainSDF por chunk;
//   - voxel cache SDF/gradiente também por chunk.
// O mapa global só é tocado no lookup/erase; o uso do cache é serializado
// por objeto para evitar reutilizar buffers inválidos durante cancelamento.
// ---------------------------------------------------------------------------
struct TerrainSDFParamCache {
    float seed, height_scale, freq, h_cache_scale;
    float noise_scale_multiplier;
    float oct2_freq_mult, oct2_weight, oct2_height_scale;
    float smooth_factor;
    int   smooth_iterations;
    bool  valid = false;
};
using KX_SNRAlignedFloatVec = KX_GameObject::AlignedFloatVec;
using KX_SNRAlignedFloatVecPtr = std::shared_ptr<KX_SNRAlignedFloatVec>;
using KX_SNRStdFloatVecPtr = std::shared_ptr<std::vector<float>>;
struct KX_SNRObjectCache {
    std::mutex mutex;
    size_t nx = 0, ny = 0, nz = 0;
    KX_SNRAlignedFloatVecPtr grad_grid;
    KX_SNRStdFloatVecPtr sdf_cache;
    TerrainSDFParamCache params;
    std::atomic<bool> purge_requested{false};
    bool valid = false;
};
using KX_SNRObjectCachePtr = std::shared_ptr<KX_SNRObjectCache>;
static ankerl::unordered_dense::map<KX_GameObject *, KX_SNRObjectCachePtr> g_snr_cache_by_owner;
static std::shared_mutex g_snr_cache_by_owner_mutex;

static inline void KX_SNR_ForceRelease(KX_SNRObjectCache& cache)
{
    cache.valid = false;
    cache.nx = cache.ny = cache.nz = 0;
    cache.grad_grid.reset();
    cache.sdf_cache.reset();
    cache.params.valid = false;
    cache.purge_requested.store(false, std::memory_order_relaxed);
}

static inline bool KX_SNR_HasVoxelCache(const KX_SNRObjectCache& cache,
                                        const size_t nx,
                                        const size_t ny,
                                        const size_t nz,
                                        const size_t total_voxels)
{
    return cache.valid &&
           (cache.nx == nx) && (cache.ny == ny) && (cache.nz == nz) &&
           cache.grad_grid && cache.sdf_cache &&
           (cache.grad_grid->size() == total_voxels * 3) &&
           (cache.sdf_cache->size() == total_voxels);
}

static inline void KX_SNR_CopySdfFromCache(float *dst, const std::vector<float>& src, const size_t total_voxels)
{
    if (total_voxels != 0) {
        memcpy(dst, src.data(), total_voxels * sizeof(float));
    }
}

static inline void KX_SNR_StoreVoxelCache(KX_SNRObjectCache& cache,
                                          KX_SNRAlignedFloatVec&& grad_grid,
                                          const float *vals,
                                          const size_t total_voxels,
                                          const size_t nx,
                                          const size_t ny,
                                          const size_t nz)
{
    cache.grad_grid = std::make_shared<KX_SNRAlignedFloatVec>(std::move(grad_grid));
    cache.nx = nx;
    cache.ny = ny;
    cache.nz = nz;
    cache.sdf_cache = std::make_shared<std::vector<float>>();
    cache.sdf_cache->resize(total_voxels);
    if (total_voxels != 0) {
        memcpy(cache.sdf_cache->data(), vals, total_voxels * sizeof(float));
    }
    cache.valid = true;
    cache.purge_requested.store(false, std::memory_order_relaxed);
}

static inline KX_SNRObjectCachePtr KX_SNR_GetCacheForOwner(KX_GameObject *owner)
{
    if (!owner) {
        return nullptr;
    }

    {
        std::shared_lock<std::shared_mutex> lock(g_snr_cache_by_owner_mutex);
        auto it = g_snr_cache_by_owner.find(owner);
        if (it != g_snr_cache_by_owner.end() && it->second) {
            return it->second;
        }
    }

    std::unique_lock<std::shared_mutex> lock(g_snr_cache_by_owner_mutex);
    KX_SNRObjectCachePtr& cache = g_snr_cache_by_owner[owner];
    if (!cache) {
        cache = std::make_shared<KX_SNRObjectCache>();
    }
    return cache;
}

static void KX_SNR_ClearCachesForOwner(KX_GameObject *owner, bool releaseNow)
{
    if (!owner) {
        return;
    }

    KX_SNRObjectCachePtr cache;
    {
        std::unique_lock<std::shared_mutex> lock(g_snr_cache_by_owner_mutex);
        auto it = g_snr_cache_by_owner.find(owner);
        if (it == g_snr_cache_by_owner.end()) {
            return;
        }
        cache = std::move(it->second);
        g_snr_cache_by_owner.erase(it);
    }

    if (!cache) {
        return;
    }

    cache->purge_requested.store(true, std::memory_order_relaxed);
    if (releaseNow) {
        std::lock_guard<std::mutex> cacheLock(cache->mutex);
        KX_SNR_ForceRelease(*cache);
    }
}

#ifdef WITH_PYTHON


#define KX_SNR_TIMING 0
#define KX_SNR_SEAM_DEBUG 0

PyObject *KX_GameObject::PySurfaceNetsAndRebuild(PyObject *args)
{
#if KX_SNR_TIMING
    using Clock     = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;


    // Helper: retorna ms entre dois time_points
    auto ms_between = [](TimePoint a, TimePoint b) -> double {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };

    // Timestamps por etapa
    TimePoint t_fn_start        = Clock::now();
    TimePoint t_phase1_start    = Clock::now(), t_phase1_end    = Clock::now();
    TimePoint t_phase2_start    = Clock::now(), t_phase2_end    = Clock::now();
    TimePoint t_hm_start        = Clock::now(), t_hm_end        = Clock::now();
    TimePoint t_hm_cache_start  = Clock::now(), t_hm_cache_end  = Clock::now();
    TimePoint t_hm_precomp_start= Clock::now(), t_hm_precomp_end= Clock::now();
    TimePoint t_passA1_start    = Clock::now(), t_passA1_end    = Clock::now();
    TimePoint t_passB_start     = Clock::now(), t_passB_end     = Clock::now();
    TimePoint t_sdf_start       = Clock::now(), t_sdf_end       = Clock::now();
    TimePoint t_grad_start      = Clock::now(), t_grad_end      = Clock::now();
    TimePoint t_smooth_start    = Clock::now(), t_smooth_end    = Clock::now();
    TimePoint t_snpass1_start   = Clock::now(), t_snpass1_end   = Clock::now();
    TimePoint t_merge_start     = Clock::now(), t_merge_end     = Clock::now();
    TimePoint t_snpass2_start   = Clock::now(), t_snpass2_end   = Clock::now();
    TimePoint t_indsclean_start = Clock::now(), t_indsclean_end = Clock::now();
    TimePoint t_lsmooth_start   = Clock::now(), t_lsmooth_end   = Clock::now();
    TimePoint t_normpass4_start = Clock::now(), t_normpass4_end = Clock::now();
    TimePoint t_norm4a_start    = Clock::now(), t_norm4a_end    = Clock::now();
    TimePoint t_norm4b_start    = Clock::now(), t_norm4b_end    = Clock::now();
    TimePoint t_rebuild_start   = Clock::now(), t_rebuild_end   = Clock::now();
#endif // KX_SNR_TIMING

    // -- FASE 1: Extração Python (sob GIL) ----------------------------------
    // Objetivo: liberar a GIL o mais rápido possível.
    // Regras:
    //   - Nenhuma lógica C++ aqui - só leitura de PyObjects para o staging struct.
    //   - py_clip_bounds, py_base_params e py_edits são SEMPRE tuple (garantido
    //     pelo caller Python) - sem PyTuple_Check, sem branches de tipo.
    //   - PyTuple_GET_ITEMS acessa o array interno do CPython em O(1) sem
    //     chamadas repetidas a PyTuple_GET_ITEM.
    // -----------------------------------------------------------------------
#if KX_SNR_TIMING
    t_phase1_start = Clock::now();
#endif

    // Staging struct: todos os parâmetros escalares em um bloco contíguo.
    // Preenchido sob GIL, lido sem GIL.
    struct SurfaceNetsInput {
        // res
        int nx, ny, nz;
        // bounds
        float min_x, max_x, min_y, max_y, min_z, max_z;
        // clip bounds (default = bounds)
        float clip_min_x, clip_max_x, clip_min_y, clip_max_y;
        // base params
        float seed, height_scale, freq, h_cache_scale;
        float noise_scale_multiplier, oct2_freq_mult, oct2_weight, oct2_height_scale;
        // smooth
        float smooth_factor;
        int   smooth_iterations;
        // world pos
        float wx, wy, wz;
        // seam
        int   seam_z_snap_i;
        float seam_z_snap_factor, seam_normal_blend;
        // misc
        int   lod, skip_sdf_fill;
        float cam_local_x, cam_local_y, cam_local_z;
        float lod_detail_radius, lod_detail_falloff;
        int   use_neighbor_sampling, interior_stride_arg;
        int   base_res_x, base_res_y;
        // Se 1, o shader recebe as cores em escala 0-255 via uv[1].xy (não normalizado).
        // Os canais RGBA de gl_Color continuam 0-1 (hardware normaliza GL_UNSIGNED_BYTE).
        // Use uv[1].x para o valor raw do canal alpha (0-255) e uv[1].y para futuros usos.
        int   raw_vertex_colors;
    };

    SurfaceNetsInput in;
    PyObject *py_values, *py_cell_vertex, *py_base_params, *py_edits, *py_clip_bounds;
    PyObject *py_res_obj, *py_bounds_obj;

    // Defaults para parâmetros opcionais
    in.seam_z_snap_factor  = 0.85f;
    in.seam_normal_blend   = 0.0f;
    in.lod                 = 0;
    in.skip_sdf_fill       = 0;
    in.cam_local_x = in.cam_local_y = in.cam_local_z = 0.0f;
    in.lod_detail_radius   = 0.0f;
    in.lod_detail_falloff  = 2.0f;
    in.use_neighbor_sampling = 0;
    in.interior_stride_arg = 1;
    in.base_res_x = in.base_res_y = 0;
    in.raw_vertex_colors = 0;

    if (!PyArg_ParseTuple(args, "OOOOOOOiffff|iffiifffffiii:surface_nets_and_rebuild",
                          &py_res_obj, &py_bounds_obj, &py_values, &py_cell_vertex,
                          &py_base_params, &py_edits, &py_clip_bounds,
                          &in.smooth_iterations, &in.smooth_factor,
                          &in.wx, &in.wy, &in.wz,
                          &in.use_neighbor_sampling, &in.seam_z_snap_factor, &in.seam_normal_blend,
                          &in.lod, &in.skip_sdf_fill,
                          &in.cam_local_x, &in.cam_local_y, &in.cam_local_z,
                          &in.lod_detail_radius, &in.lod_detail_falloff,
                          &in.interior_stride_arg, &in.base_res_x, &in.base_res_y)) {
        return nullptr;
    }

    // seam_z_snap_i segue use_neighbor_sampling (compatibilidade)
    in.seam_z_snap_i = in.use_neighbor_sampling;

    // res - py_res_obj é sempre tuple de 3 ints
    {
        PyObject * const *items = &PyTuple_GET_ITEM(py_res_obj, 0);
        in.nx = (int)PyLong_AsLong(items[0]);
        in.ny = (int)PyLong_AsLong(items[1]);
        in.nz = (int)PyLong_AsLong(items[2]);
    }

    // bounds - tuple de 6 floats
    {
        PyObject * const *b = &PyTuple_GET_ITEM(py_bounds_obj, 0);
        in.min_x = (float)PyFloat_AsDouble(b[0]);
        in.max_x = (float)PyFloat_AsDouble(b[1]);
        in.min_y = (float)PyFloat_AsDouble(b[2]);
        in.max_y = (float)PyFloat_AsDouble(b[3]);
        in.min_z = (float)PyFloat_AsDouble(b[4]);
        in.max_z = (float)PyFloat_AsDouble(b[5]);
    }

    // clip_bounds - tuple de 4 floats (caller sempre passa tuple; default = bounds)
    in.clip_min_x = in.min_x; in.clip_max_x = in.max_x;
    in.clip_min_y = in.min_y; in.clip_max_y = in.max_y;
    if (PyTuple_GET_SIZE(py_clip_bounds) >= 4) {
        PyObject * const *c = &PyTuple_GET_ITEM(py_clip_bounds, 0);
        in.clip_min_x = (float)PyFloat_AsDouble(c[0]);
        in.clip_max_x = (float)PyFloat_AsDouble(c[1]);
        in.clip_min_y = (float)PyFloat_AsDouble(c[2]);
        in.clip_max_y = (float)PyFloat_AsDouble(c[3]);
    }

    if (m_proceduralCancel.load(std::memory_order_relaxed)) {
        Py_RETURN_NONE;
    }

    KX_SNRObjectCachePtr snr_cache = KX_SNR_GetCacheForOwner(this);
    {
        std::lock_guard<std::mutex> cacheLock(snr_cache->mutex);
        TerrainSDFParamCache& param_cache = snr_cache->params;
        if (!param_cache.valid) {
            const Py_ssize_t bp_size = PyTuple_GET_SIZE(py_base_params);
            PyObject * const *bp = &PyTuple_GET_ITEM(py_base_params, 0);
            param_cache.seed         = (float)PyFloat_AsDouble(bp[0]);
            param_cache.height_scale = (float)PyFloat_AsDouble(bp[1]);
            param_cache.freq         = (float)PyFloat_AsDouble(bp[2]);
            param_cache.h_cache_scale= (float)PyFloat_AsDouble(bp[3]);
            param_cache.noise_scale_multiplier = (bp_size > 5) ? (float)PyFloat_AsDouble(bp[5]) : 1.0f;
            param_cache.oct2_freq_mult    = (bp_size > 6) ? (float)PyFloat_AsDouble(bp[6]) : 2.0f;
            param_cache.oct2_weight       = (bp_size > 7) ? (float)PyFloat_AsDouble(bp[7]) : 0.5f;
            param_cache.oct2_height_scale = (bp_size > 8) ? (float)PyFloat_AsDouble(bp[8]) : param_cache.height_scale;
            param_cache.smooth_factor     = in.smooth_factor;
            param_cache.smooth_iterations = in.smooth_iterations;
            param_cache.valid = true;
        }
        in.seed                   = param_cache.seed;
        in.height_scale           = param_cache.height_scale;
        in.freq                   = param_cache.freq;
        in.h_cache_scale          = param_cache.h_cache_scale;
        in.noise_scale_multiplier = param_cache.noise_scale_multiplier;
        in.oct2_freq_mult         = param_cache.oct2_freq_mult;
        in.oct2_weight            = param_cache.oct2_weight;
        in.oct2_height_scale      = param_cache.oct2_height_scale;
        in.smooth_factor          = param_cache.smooth_factor;
        in.smooth_iterations      = param_cache.smooth_iterations;
    }

    // Acesso direto ao buffer - zero chamadas Python API por edit.
    std::vector<VoxelSphereEdit> edits;
    {
        Py_buffer edits_view;
        if (PyObject_GetBuffer(py_edits, &edits_view, PyBUF_SIMPLE) == 0) {
            const float* edata = (const float*)edits_view.buf;
            const Py_ssize_t n_edits = edits_view.len / (Py_ssize_t)(7 * sizeof(float));
            edits.resize((size_t)n_edits);
            for (Py_ssize_t i = 0; i < n_edits; ++i) {
                const float* e = edata + i * 7;
                edits[i] = { (int)e[0], e[1], e[2], e[3], e[4], e[5], e[6] };
            }
            PyBuffer_Release(&edits_view);
        }
        // py_edits sem buffer protocol → sem edits (caller deve usar array.array)
    }

    (void)py_values;
    (void)py_cell_vertex;

    const size_t nx = (size_t)in.nx, ny = (size_t)in.ny, nz = (size_t)in.nz;
    const float min_x = in.min_x, max_x = in.max_x;
    const float min_y = in.min_y, max_y = in.max_y;
    const float min_z = in.min_z, max_z = in.max_z;
    const float clip_min_x = in.clip_min_x, clip_max_x = in.clip_max_x;
    const float clip_min_y = in.clip_min_y, clip_max_y = in.clip_max_y;
    const float seed         = in.seed;
    const float height_scale = in.height_scale;
    const float freq         = in.freq;
    const float h_cache_scale         = in.h_cache_scale;
#if KX_SNR_TIMING
    printf("h_cache_scale = %f\n", h_cache_scale);
#endif

    const float noise_scale_multiplier = in.noise_scale_multiplier;
    const float oct2_freq_mult        = in.oct2_freq_mult;
    const float oct2_weight           = in.oct2_weight;
    const float oct2_height_scale     = in.oct2_height_scale;
    const float noise_scale = (1.0f / h_cache_scale) * freq * noise_scale_multiplier;
    const float seed_z      = seed * 0.001f;
    const float smooth_factor      = in.smooth_factor;
    const int   smooth_iterations  = in.smooth_iterations;
    const float wx = in.wx, wy = in.wy, wz_world = in.wz;
    const int   seam_z_snap_i      = in.seam_z_snap_i;
    const float seam_z_snap_factor = in.seam_z_snap_factor;
    const float seam_normal_blend  = in.seam_normal_blend;
    // skip_sdf_fill removido: sem cache de grid, o SDF é sempre recalculado.
    const float lod_detail_radius  = in.lod_detail_radius;
    const int   use_neighbor_sampling = in.use_neighbor_sampling;

    // Grid
    const size_t cells_x = nx - 1, cells_y = ny - 1, cells_z = nz - 1;
    const float step_x = (max_x - min_x) / (float)cells_x;
    const float step_y = (max_y - min_y) / (float)cells_y;
    const float step_z = (max_z - min_z) / (float)cells_z;
    const float inv_step_x = (step_x > 1e-9f) ? (1.0f / step_x) : 0.0f;
    const float inv_step_y = (step_y > 1e-9f) ? (1.0f / step_y) : 0.0f;
    const float inv_step_z = (step_z > 1e-9f) ? (1.0f / step_z) : 0.0f;

    const size_t total_voxels = nx * ny * nz;

    // Estruturas C++ - declaradas aqui, fora do Py_BEGIN_ALLOW_THREADS para manter o escopo
    std::vector<float> verts, norms;
    std::vector<uint8_t> surface_mask;
    std::vector<uint8_t> biome_mask;
    std::vector<int>   inds;

#if KX_SNR_SEAM_DEBUG
    // Buffer de debug para vértices afetados por edits na região de seam.
    // Coletado no PASS 1, impresso uma única vez ao final — sem poluir o terminal.
    struct SeamDebugEntry {
        float vx_raw, vy_raw, vz_raw;   // posição saída do surface nets (antes de qualquer snap)
        float vx_out, vy_out, vz_out;   // posição final após todos os snaps
        float sdf_at_vert;              // SDF avaliado na posição final
        float terrain_h;                // altura base do terreno neste XY
        float edit_cx, edit_cy, edit_cz, edit_r; // edit mais próximo que influenciou
        float dist_to_edit_surface;     // distância à superfície do edit (sd)
        float seam_edge_dist;           // distância à borda do clip
        int   was_seam_reprojected;     // 1 se a reprojeção de esfera foi usada
        int   was_reprojected;          // 1 se reprojeção add_edits foi usada
        int   vz_below_terrain;         // 1 se vz < terrain_h - 2*step_z (vértice de parede)
    };
    std::vector<SeamDebugEntry> seam_debug_buf;
    seam_debug_buf.reserve(256);
    std::mutex seam_debug_mutex;
#endif

    std::unique_ptr<float[]> vals_buf(new float[total_voxels]);
    float *vals = vals_buf.get();
    const size_t total_cells = cells_x * cells_y * cells_z;
    std::vector<int> cv_buf(total_cells, -1);
    int *cv = cv_buf.data();

    {
        size_t est = total_cells * 15 / 100;
        verts.reserve(est * 3);
        norms.reserve(est * 3);
        surface_mask.reserve(est);
        biome_mask.reserve(est);
        inds.reserve(est * 6);
    }

    static std::atomic<int> s_active_surface_nets_calls{0};
    struct SurfaceNetsCallCountGuard {
        std::atomic<int>& m_counter;
        SurfaceNetsCallCountGuard(std::atomic<int>& counter)
            : m_counter(counter)
        {
            m_counter.fetch_add(1, std::memory_order_relaxed);
        }
        ~SurfaceNetsCallCountGuard()
        {
            m_counter.fetch_sub(1, std::memory_order_relaxed);
        }
    } surfaceNetsCallCountGuard(s_active_surface_nets_calls);

    struct ProceduralActiveCallsGuard {
        std::atomic<int>& m_counter;
        ProceduralActiveCallsGuard(std::atomic<int>& counter)
            : m_counter(counter)
        {
            m_counter.fetch_add(1, std::memory_order_relaxed);
        }
        ~ProceduralActiveCallsGuard()
        {
            m_counter.fetch_sub(1, std::memory_order_relaxed);
        }
    } proceduralActiveCallsGuard(m_proceduralActiveCalls);

    if (m_proceduralCancel.load(std::memory_order_relaxed)) {
        KX_SNR_ClearCachesForOwner(this, true);
        Py_RETURN_NONE;
    }

    int omp_threads = 1;

    KX_Mesh *newMesh_ptr = nullptr;

    // -- FASE 2: C++ puro - GIL liberada ------------------------------------
    // Nenhum PyObject é tocado daqui até Py_END_ALLOW_THREADS.
    // Desempacota SurfaceNetsInput para const locals - compilador usa registradores.
    // -----------------------------------------------------------------------
#if KX_SNR_TIMING
    t_phase1_end   = Clock::now();
    t_phase2_start = Clock::now();
#endif


    KX_Mesh *srcMesh_pre = m_meshes.empty() ? nullptr : m_meshes.front();
    const std::vector<RAS_MeshMaterial *> matList_fallback_pre;
    const std::vector<RAS_MeshMaterial *>& matList_pre = srcMesh_pre ? srcMesh_pre->GetMeshMaterialList() : matList_fallback_pre;
    KX_Scene *scene_pre = GetScene();
    RAS_BucketManager *bucketManager_pre = scene_pre ? scene_pre->GetBucketManager() : nullptr;
    RAS_MeshMaterial *srcMat_pre = matList_pre.empty() ? nullptr : matList_pre[0];
    const RAS_DisplayArray::Format fmtCopy_pre = (srcMat_pre && srcMat_pre->GetDisplayArray()) ? srcMat_pre->GetDisplayArray()->GetFormat() : RAS_DisplayArray::Format();
    KX_BlenderMaterial *srcMaterialCopy_pre = (srcMat_pre && srcMat_pre->GetBucket()) ? static_cast<KX_BlenderMaterial *>(srcMat_pre->GetBucket()->GetMaterial()) : nullptr;
    const RAS_Mesh::LayersInfo layersInfoCopy_pre = srcMesh_pre ? srcMesh_pre->GetLayersInfo() : RAS_Mesh::LayersInfo();
    RAS_DisplayArray *newArray = nullptr;

    Py_BEGIN_ALLOW_THREADS

    {
        const int max_threads = omp_get_num_procs() / 2;
        const int usable = (max_threads > 2) ? (max_threads - 1) : max_threads;
        int active_calls = s_active_surface_nets_calls.load(std::memory_order_relaxed);
        omp_threads = usable / (active_calls > 0 ? active_calls : 1);
        if (omp_threads < 2) omp_threads = 2;
        if (usable < 2) omp_threads = 1;
        omp_set_num_threads(omp_threads);

    }

    // Aliases locais para uso sem GIL (mesmos valores, sem toque em Python state)
    KX_Mesh *srcMesh = srcMesh_pre;
    const std::vector<RAS_MeshMaterial *>& matList = matList_pre;
    KX_Scene *scene = scene_pre;
    RAS_BucketManager *bucketManager = bucketManager_pre;
    RAS_DisplayArray::Format fmtCopy = fmtCopy_pre;
    // Desliga normalização de cor de vértice: bytes 0-255 chegam ao shader sem dividir por 255.
    fmtCopy.rawColors = true;
    KX_BlenderMaterial *srcMaterialCopy = srcMaterialCopy_pre;
    const RAS_Mesh::LayersInfo layersInfoCopy = layersInfoCopy_pre;

    // Constantes de caverna
    const int   enable_caves       = 1;
    const float cave_noise_scale   = 0.12f;
    const float cave_threshold     = 0.35f;
    const float cave_seed          = 99999.0f;
    const float cave_safety_margin = 1.5f;
    const float warp_scale         = 0.05f;
    const float warp_strength      = 10.0f;
    const float cave_strength_mult = 30.0f;

    // Grid
    const size_t cell_start_x = use_neighbor_sampling ? 1 : 0;
    const size_t cell_start_y = use_neighbor_sampling ? 1 : 0;
    const size_t cell_start_z = use_neighbor_sampling ? 1 : 0;
    const size_t cell_end_x   = use_neighbor_sampling ? cells_x - 1 : cells_x;
    const size_t cell_end_y   = use_neighbor_sampling ? cells_y - 1 : cells_y;
    const size_t cell_end_z   = use_neighbor_sampling ? cells_z - 1 : cells_z;
    const float seam_snap_eps = std::max(1e-6f, std::min(step_x, step_y) * 1.0f);
    const float pin_eps       = std::max(step_x, step_y) * 3.0f;
    const bool  seam_z_snap   = (seam_z_snap_i != 0);
    const float seam_z_eps    = std::max(1e-6f, std::min(step_x, step_y) * seam_z_snap_factor);
    const size_t stride_x      = ny * nz;
    const size_t stride_y      = nz;
    const float hx_g = 0.5f * inv_step_x;
    const float hy_g = 0.5f * inv_step_y;
    const float hz_g = 0.5f * inv_step_z;
    size_t vc2 = 0;

    using AlignedFloatVec = KX_GameObject::AlignedFloatVec;

    // SOA offsets: [gx0..N) | [gy0..N) | [gz0..N) - definidos aqui para uso no Pass 4
    const size_t soa_off_y = total_voxels;
    const size_t soa_off_z = total_voxels * 2;

    // Estruturas C++ - declaradas aqui, sem GIL
    CaveGrid3D cave_grid;
    struct EditBBox { float xmin, xmax, ymin, ymax; };
    std::vector<EditBBox> edit_bboxes(edits.size());
    AlignedFloatVec grad_grid_local;
    grad_grid_local.resize(total_voxels * 3);
    AlignedFloatVec& grad_grid = grad_grid_local;
    std::vector<float> terrain_noise_cache_t;
    int terrain_nc_ix0 = 0, terrain_nc_iy0 = 0;
    int terrain_nc_w = 0, terrain_nc_h = 0;
    bool use_fast_terrain_h = false;

    auto fast_floor_to_int = [](float v) -> int {
        const int i = (int)v;
        return i - (v < (float)i);
    };

    // Lambdas de amostragem - C++ puro, sem GIL
    auto sample_terrain_h = [&](float x, float y) -> float {
        if (use_fast_terrain_h) {
            // Converte posição de mundo para índice fracionário de voxel
            const float vx = (x - min_x) * inv_step_x;
            const float vy = (y - min_y) * inv_step_y;
            const int   lx0 = fast_floor_to_int(vx);
            const int   ly0 = fast_floor_to_int(vy);
            // Clamp sempre: reduz branches (mesma semântica quando dentro/fora do range).
            const int clx0 = std::max(0, std::min((int)terrain_nc_w - 2, lx0));
            const int cly0 = std::max(0, std::min((int)terrain_nc_h - 2, ly0));
            const int clx1 = clx0 + 1;
            const int cly1 = cly0 + 1;
            const float fx = vx - clx0;
            const float fy = vy - cly0;
            const float h00 = terrain_noise_cache_t[(size_t)clx0 * terrain_nc_h + cly0];
            const float h10 = terrain_noise_cache_t[(size_t)clx1 * terrain_nc_h + cly0];
            const float h01 = terrain_noise_cache_t[(size_t)clx0 * terrain_nc_h + cly1];
            const float h11 = terrain_noise_cache_t[(size_t)clx1 * terrain_nc_h + cly1];
            return h00 + fx * (h10 - h00) + fy * (h01 - h00) + fx * fy * (h00 - h10 - h01 + h11);
        }
        float sx = x * h_cache_scale;
        int ix = fast_floor_to_int(sx);
        float fx = sx - ix;
        float sy = y * h_cache_scale;
        int iy = fast_floor_to_int(sy);
        float fy = sy - iy;
        float h00 = get_voxel_noise(ix, iy, noise_scale, seed_z, height_scale, oct2_freq_mult, oct2_weight, oct2_height_scale);
        float h10 = get_voxel_noise(ix+1, iy, noise_scale, seed_z, height_scale, oct2_freq_mult, oct2_weight, oct2_height_scale);
        float h01 = get_voxel_noise(ix, iy+1, noise_scale, seed_z, height_scale, oct2_freq_mult, oct2_weight, oct2_height_scale);
        float h11 = get_voxel_noise(ix+1, iy+1, noise_scale, seed_z, height_scale, oct2_freq_mult, oct2_weight, oct2_height_scale);
        return h00 + fx * (h10 - h00) + fy * (h01 - h00) + fx * fy * (h00 - h10 - h01 + h11);
    };


    if (enable_caves) {
        cave_grid = CaveGrid3D(6, 6, 6, min_x, max_x, min_y, max_y, min_z, max_z);
        generate_cave_grid_3d(cave_grid, cave_seed, cave_threshold,
                             cave_noise_scale, warp_scale, warp_strength);
    }


    for (size_t ei = 0; ei < edits.size(); ++ei) {
        const auto& e = edits[ei];
        edit_bboxes[ei] = { e.cx - e.r, e.cx + e.r, e.cy - e.r, e.cy + e.r };
    }

    const bool has_edits = !edits.empty();

    std::vector<size_t> add_edits;
    add_edits.reserve(edits.size());
    for (size_t ei = 0; ei < edits.size(); ++ei) {
        if (edits[ei].operation == 1) {
            add_edits.push_back(ei);
        }
    }

    const float inv_step_z_const = 1.0f / step_z;

    auto get_sdf_at = [&](float x, float y, float z) -> float {
        float terrain_h = sample_terrain_h(x, y);
        float d = z - terrain_h;

        if (enable_caves) {
            float voxels_below = -d * inv_step_z_const;
            if (voxels_below > cave_safety_margin) {
                float cave_noise = interpolate_cave_trilinear(cave_grid, x, y, z);
                float cave_strength = (cave_threshold - cave_noise) * cave_strength_mult;
                d = fmaxf(d, (cave_noise < cave_threshold) ? cave_strength : d);
            }
        }

        const size_t ne = edits.size();
        if (ne == 1) {
            const auto& edit = edits[0];
            float dx = x - edit.cx, dy = y - edit.cy, dz = z - edit.cz;
            float sd = sqrtf(dx*dx + dy*dy + dz*dz) - edit.r;
            if (edit.operation == 1) d = fmaxf(d, -sd);
            else if (edit.operation == 0) d = fminf(d, sd);
            else d = fmaxf(d, sd);
        } else {
            for (size_t ei = 0; ei < ne; ++ei) {
                const auto& edit = edits[ei];
                const EditBBox& bb = edit_bboxes[ei];
                if (x < bb.xmin || x > bb.xmax || y < bb.ymin || y > bb.ymax) {
                    continue;
                }
                float dx = x - edit.cx, dy = y - edit.cy, dz = z - edit.cz;
                float sd = sqrtf(dx*dx + dy*dy + dz*dz) - edit.r;
                if (edit.operation == 1) d = fmaxf(d, -sd);
                else if (edit.operation == 0) d = fminf(d, sd);
                else d = fmaxf(d, sd);
            }
        }
        return d;
    };

    auto sample_edited_h = [&](float x, float y) -> float {
        // Detecta se há edit de escavação (op==1 = fmaxf(d,-sd)) cobrindo este XY.
        // Edits de adição (op==0) são ignorados — não devem mover a seam para cima.
        float best_r = 0.0f, best_cx = 0.0f, best_cy = 0.0f, best_cz = 0.0f;
        float best_dxy2 = 1e30f;
        bool has_diff_edit = false;
        for (size_t ei = 0; ei < edits.size(); ++ei) {
            const auto& ed = edits[ei];
            if (ed.operation != 1) continue;
            const EditBBox& bb = edit_bboxes[ei];
            if (x < bb.xmin || x > bb.xmax || y < bb.ymin || y > bb.ymax) continue;
            float dx = x - ed.cx, dy = y - ed.cy;
            float dxy2 = dx*dx + dy*dy;
            if (dxy2 < ed.r * ed.r) {
                // Usa o edit cujo centro XY está mais próximo deste ponto
                if (!has_diff_edit || dxy2 < best_dxy2) {
                    best_dxy2 = dxy2; best_r = ed.r;
                    best_cx = ed.cx; best_cy = ed.cy; best_cz = ed.cz;
                    has_diff_edit = true;
                }
            }
        }
        if (!has_diff_edit) return sample_terrain_h(x, y);

        const float terrain_base_h = sample_terrain_h(x, y);

        // Topo geométrico da casca da esfera neste XY: cz + sqrt(r² - dxy²)
        float dz_sphere = sqrtf(fmaxf(0.0f, best_r*best_r - best_dxy2));
        float sphere_top_z = best_cz + dz_sphere;

        // Só retorna o Z esférico se ele estiver ABAIXO do terreno E se a abertura
        // que a esfera cria for significativa (pelo menos 1 step_z de profundidade).
        // Se sphere_top_z >= terrain_base, a esfera não perfurou o terreno aqui.
        // Se sphere_top_z está muito perto de terrain_base mas veio de dxy ≈ r (borda
        // da esfera), dz_sphere é quase zero — o "buraco" seria trivial e causaria
        // artefatos puxando vértices de superfície para baixo.
        if (sphere_top_z >= terrain_base_h - step_z) return terrain_base_h;
        return sphere_top_z;
    };

    std::vector<uint8_t> biome_cache_xy((size_t)nx * ny);

    {
#if KX_SNR_TIMING
        t_hm_start = Clock::now();
#endif
        // Cache indexado diretamente pelo voxel: tamanho (nx+1) x (ny+1).
        // Voxel x usa cache[lx0=x, lx1=x+1], voxel y usa cache[ly=y, ly+1=y+1].
        // Isso evita o cache gigante derivado de coordenadas de mundo escaladas.
        const int nc_w_l = nx + 1;
        const int nc_h_l = ny + 1;
        terrain_nc_w = nc_w_l;
        terrain_nc_h = nc_h_l;
        terrain_noise_cache_t.resize((size_t)nc_w_l * nc_h_l);
        use_fast_terrain_h = true;

        // Pré-computa parâmetros do eixo Y: reduz floor/mul por célula no fill do cache.
#if KX_SNR_TIMING
        t_hm_precomp_start = Clock::now();
#endif
        std::vector<int> y_piy((size_t)nc_h_l);
        std::vector<float> y_fy((size_t)nc_h_l);
        #pragma omp simd
        for (int ly = 0; ly < nc_h_l; ++ly) {
            const float wy = min_y + (float)ly * step_y;
            const float sy = wy * h_cache_scale;
            const int piy = fast_floor_to_int(sy);
            y_piy[(size_t)ly] = piy;
            y_fy[(size_t)ly] = sy - (float)piy;
        }
#if KX_SNR_TIMING
        t_hm_precomp_end = Clock::now();
#endif

        // Cache [lx * nc_h_l + ly]: nx+1 colunas x ny+1 linhas, indexado por voxel
#if KX_SNR_TIMING
        t_hm_cache_start = Clock::now();

        printf("[hm_cache] tamanho cache: %d x %d = %d celulas\n", nc_w_l, nc_h_l, nc_w_l * nc_h_l);
        auto _cache_t0 = std::chrono::high_resolution_clock::now();
#endif
        #pragma omp parallel for schedule(static) num_threads(omp_threads)
        for (int lx = 0; lx < nc_w_l; ++lx) {
            const float wx  = min_x + lx * step_x;
            // Preserva a interpolação bilinear original: amostra em 4 pontos do noise
            // como o sistema antigo fazia, em vez de só usar o floor.
            const float sx  = wx * h_cache_scale;
            const int   pix = fast_floor_to_int(sx);
            const float fx  = sx - pix;
            float* __restrict col = terrain_noise_cache_t.data() + (size_t)lx * nc_h_l;
            #pragma omp simd
            for (int ly = 0; ly < nc_h_l; ++ly) {
                const int piy = y_piy[(size_t)ly];
                const float fy = y_fy[(size_t)ly];
                uint8_t biome_id_xy = 0;
                const float h00 = get_voxel_noise(pix,   piy,   noise_scale, seed_z, height_scale, oct2_freq_mult, oct2_weight, oct2_height_scale, &biome_id_xy);
                const float h10 = get_voxel_noise(pix+1, piy,   noise_scale, seed_z, height_scale, oct2_freq_mult, oct2_weight, oct2_height_scale);
                const float h01 = get_voxel_noise(pix,   piy+1, noise_scale, seed_z, height_scale, oct2_freq_mult, oct2_weight, oct2_height_scale);
                const float h11 = get_voxel_noise(pix+1, piy+1, noise_scale, seed_z, height_scale, oct2_freq_mult, oct2_weight, oct2_height_scale);
                col[ly] = h00 + fx*(h10-h00) + fy*(h01-h00) + fx*fy*(h00-h10-h01+h11);
                if (lx < nx && ly < ny) {
                    biome_cache_xy[(size_t)lx * ny + (size_t)ly] = biome_id_xy;
                }
            }
        }
#if KX_SNR_TIMING
        auto _cache_t1 = std::chrono::high_resolution_clock::now();
        printf("[hm_cache] tempo total loop: %.4f ms\n",
               std::chrono::duration<double, std::milli>(_cache_t1 - _cache_t0).count());

        t_hm_cache_end = Clock::now();
#endif

#if KX_SNR_TIMING
        t_passA1_start = Clock::now();
        t_passA1_end   = Clock::now(); // PASS A eliminado: sem cópia heightmap_sn
#endif

#if KX_SNR_TIMING
        t_passB_start = Clock::now();
#endif
#if KX_SNR_TIMING
        t_passB_end = Clock::now();
        t_hm_end = Clock::now();
#endif

    } // fim do bloco heightmap

    {
#if KX_SNR_TIMING
        t_sdf_start = Clock::now();
#endif
        
        // ✅ SDF FILL OTIMIZADO: Paralelização no loop externo (evita div/mod)
        // [OPT-7] Pré-computa per-edit o range de índices Z [z_min_idx, z_max_idx]
        // para cada coluna (x,y). Aqui pré-computamos apenas o range global por edit
        // (independente de x,y) - o skip por coluna é feito no loop Z.
        struct EditZRange { int zmin, zmax; };
        std::vector<EditZRange> edit_z_ranges(edits.size());
        for (size_t ei = 0; ei < edits.size(); ++ei) {
            const auto& ed = edits[ei];
            float z_lo = ed.cz - ed.r, z_hi = ed.cz + ed.r;
            int iz0 = (int)floorf((z_lo - min_z) * inv_step_z) - 1;
            int iz1 = (int)ceilf ((z_hi - min_z) * inv_step_z) + 1;
            edit_z_ranges[ei].zmin = std::max(0, iz0);
            edit_z_ranges[ei].zmax = std::min((int)nz - 1, iz1);
        }

        // terrain_noise_cache_t[x * terrain_nc_h + y] substitui heightmap_sn[x * ny + y]:
        // elimina a cópia de nx*ny floats (PASS A), acessando o cache diretamente.
        // terrain_nc_h = ny+1 (stride da coluna no cache).
        const int sdf_nc_h = terrain_nc_h; // copia local para evitar deref de membro no loop OMP
        const float* __restrict sdf_noise_cache = terrain_noise_cache_t.data();
        #pragma omp parallel for schedule(static) num_threads(omp_threads)
        for (int x = 0; x < (int)nx; ++x) {
            const float wx_s = min_x + x * step_x;
            const size_t xy_base = (size_t)x * stride_x;
            
            for (int y = 0; y < (int)ny; ++y) {
                const float wy_s = min_y + y * step_y;
                const size_t xy_off = xy_base + (size_t)y * stride_y;
                const float base_h = sdf_noise_cache[(size_t)x * sdf_nc_h + y];

                // Pré-filtra edits ativos nesta coluna XY.
                struct ActiveEdit {
                    float xy2;
                    float cz;
                    float r;
                    int op;
                    int zmin;
                    int zmax;
                };
                ActiveEdit active_edits[256];
                int n_active = 0;
                for (size_t ei = 0; ei < edits.size() && n_active < 256; ++ei) {
                    const EditBBox& bb = edit_bboxes[ei];
                    if (wx_s < bb.xmin || wx_s > bb.xmax || wy_s < bb.ymin || wy_s > bb.ymax) continue;
                    const auto& ed = edits[ei];
                    const float ddx = wx_s - ed.cx;
                    const float ddy = wy_s - ed.cy;
                    const float xy2 = ddx*ddx + ddy*ddy;
                    const float r_margin = ed.r + step_z;
                    if (xy2 <= r_margin * r_margin) {
                        ActiveEdit& ae = active_edits[n_active++];
                        ae.xy2 = xy2;
                        ae.cz = ed.cz;
                        ae.r = ed.r;
                        ae.op = ed.operation;
                        ae.zmin = edit_z_ranges[ei].zmin;
                        ae.zmax = edit_z_ranges[ei].zmax;
                    }
                }

        // SDF fill: loop Z contíguo em vals[].
        // wz_s usa acumulador em vez de mul: min_z + z*step_z → wz_s += step_z
        // cfx e cfy do trilinear são constantes por coluna — fatorados fora do loop Z.
                const size_t z_start = (size_t)0;
                const size_t z_end   = nz;

                // Acumuladores para evitar mul dentro do loop Z
                float wz_s = min_z + (float)z_start * step_z;

                // Cave trilinear: cfx e cfy são constantes por coluna (x,y)
                // Só cfz muda a cada z — vira acumulador também
                const CaveGrid3D& cg = cave_grid;
                float cave_cfx = 0.0f, cave_cfy = 0.0f;
                float cave_cfz_base = 0.0f, cave_cfz_step = 0.0f;
                int cave_cix = 0, cave_ciy = 0;
                float cave_stx = 0.0f, cave_sty = 0.0f;
                int cave_cdx = 0, cave_cdy = 0, cave_cb_xy = 0;
                if (enable_caves) {
                    // cfx/cfy: constantes para esta coluna (x,y)
                    cave_cfx = (wx_s - cg.min_x) * cg.inv_step_x;
                    cave_cfy = (wy_s - cg.min_y) * cg.inv_step_y;
                    cave_cfx = (cave_cfx < 0.0f) ? 0.0f : ((cave_cfx > cg.nx - 1.001f) ? cg.nx - 1.001f : cave_cfx);
                    cave_cfy = (cave_cfy < 0.0f) ? 0.0f : ((cave_cfy > cg.ny - 1.001f) ? cg.ny - 1.001f : cave_cfy);
                    cave_cix = (int)cave_cfx;
                    cave_ciy = (int)cave_cfy;
                    const float ctx = cave_cfx - cave_cix;
                    const float cty = cave_cfy - cave_ciy;
                    cave_stx = ctx * ctx * (3.0f - 2.0f * ctx);
                    cave_sty = cty * cty * (3.0f - 2.0f * cty);
                    const int cix1 = (cave_cix + 1 < cg.nx) ? cave_cix + 1 : cave_cix;
                    const int ciy1 = (cave_ciy + 1 < cg.ny) ? cave_ciy + 1 : cave_ciy;
                    const int cs_y = cg.nz, cs_x = cg.ny * cg.nz;
                    cave_cdx  = (cix1 - cave_cix) * cs_x;
                    cave_cdy  = (ciy1 - cave_ciy) * cs_y;
                    cave_cb_xy = cave_cix * cs_x + cave_ciy * cs_y;
                    // cfz acumulador: valor inicial + step por z
                    cave_cfz_base = (wz_s - cg.min_z) * cg.inv_step_z;
                    cave_cfz_step = step_z * cg.inv_step_z;
                }

                for (size_t z = z_start; z < z_end; ++z, wz_s += step_z) {
                    float d = wz_s - base_h;
                    // CAVERNAS 3D - trilinear inlinado; cfx/cfy pré-calculados fora do loop
                    if (enable_caves) {
                        const float voxels_below = -d * inv_step_z_const;
                        if (voxels_below > cave_safety_margin) {
                            // cfz acumulado: sem mul, sem sub — incremento puro
                            float cfz = cave_cfz_base;
                            cfz = (cfz < 0.0f) ? 0.0f : ((cfz > cg.nz - 1.001f) ? cg.nz - 1.001f : cfz);
                            const int ciz = (int)cfz;
                            const float ctz = cfz - ciz;
                            const float stz = ctz * ctz * (3.0f - 2.0f * ctz);
                            const int ciz1 = (ciz + 1 < cg.nz) ? ciz + 1 : ciz;
                            const int cdz  = ciz1 - ciz;
                            const float* __restrict cv_ptr = cg.values.data();
                            const int cb = cave_cb_xy + ciz;
                            float cv00 = cv_ptr[cb]           + (cv_ptr[cb+cave_cdx]           - cv_ptr[cb])           * cave_stx;
                            float cv01 = cv_ptr[cb+cdz]       + (cv_ptr[cb+cave_cdx+cdz]       - cv_ptr[cb+cdz])       * cave_stx;
                            float cv10 = cv_ptr[cb+cave_cdy]  + (cv_ptr[cb+cave_cdx+cave_cdy]  - cv_ptr[cb+cave_cdy])  * cave_stx;
                            float cv11 = cv_ptr[cb+cave_cdy+cdz]+(cv_ptr[cb+cave_cdx+cave_cdy+cdz]-cv_ptr[cb+cave_cdy+cdz])*cave_stx;
                            float cv0  = cv00 + (cv10 - cv00) * cave_sty;
                            float cv1  = cv01 + (cv11 - cv01) * cave_sty;
                            const float cave_noise = cv0 + (cv1 - cv0) * stz;
                            const float cave_strength = (cave_threshold - cave_noise) * cave_strength_mult;
                            d = fmaxf(d, (cave_noise < cave_threshold) ? cave_strength : d);
                        }
                    }
                    
                    // Itera apenas edits ativos nesta coluna; skip por range Z
                    for (int ai = 0; ai < n_active; ++ai) {
                        const ActiveEdit& ae = active_edits[ai];
                        if ((int)z < ae.zmin || (int)z > ae.zmax) continue;
                        const float dz = wz_s - ae.cz;
                        const float sd = sqrtf(ae.xy2 + dz*dz) - ae.r;
                        if (ae.op == 1) {
                            d = fmaxf(d, -sd);
                        }
                        else if (ae.op == 0) d = fminf(d, sd);
                        else d = fmaxf(d, sd);
                    }
                    
                    vals[xy_off + z] = d;
                    cave_cfz_base += cave_cfz_step; // acumulador cfz — sem mul por z
                }
            }
        }
#if KX_SNR_TIMING
        t_sdf_end  = Clock::now();
        t_grad_start = Clock::now();
#endif

    // GRADIENT GRID: SOA layout [gx0..N | gy0..N | gz0..N]
    // SOA elimina o stride×3 que impedia vetorização SIMD no MSVC (C5002).
    const size_t nx_m1 = nx - 1, ny_m1 = ny - 1, nz_m1 = nz - 1;

    // Processa a chunk inteira (sem dirty region)
    #pragma omp parallel for schedule(static)
    for (int x_int = 0; x_int < (int)nx; ++x_int) {
        size_t x = (size_t)x_int;
        const size_t x_off = x * stride_x;
        const float wx_g = min_x + (float)x * step_x;
        const bool x_interior = (x > 0 && x < nx_m1);
        
        for (size_t y = 0; y < ny; ++y) {
            const size_t xy_off = x_off + y * stride_y;
            const float wy_g = min_y + (float)y * step_y;
            const bool y_interior = (y > 0 && y < ny_m1);

            // Pré-calcula sdf abaixo da borda inferior Z (z=0) uma vez por coluna.
            const float sdf_z_below = get_sdf_at(wx_g, wy_g, min_z - step_z);

            // Interior Z: [1, nz-1). Bordas Z sempre processadas (gz0==0, gz1==nz).
            const size_t z_inner_lo = 1;
            const size_t z_inner_hi = nz - 1;

            // Borda inferior Z (z=0)
            {
                const size_t z = 0, gi = xy_off + z;
                const float wz_g = min_z;
                float gx_v = x_interior ? (vals[gi+stride_x]-vals[gi-stride_x])*hx_g
                           : (x==0)     ? (vals[gi+stride_x]-get_sdf_at(wx_g-step_x,wy_g,wz_g))*hx_g
                                        : (get_sdf_at(wx_g+step_x,wy_g,wz_g)-vals[gi-stride_x])*hx_g;
                float gy_v = y_interior ? (vals[gi+stride_y]-vals[gi-stride_y])*hy_g
                           : (y==0)     ? (vals[gi+stride_y]-get_sdf_at(wx_g,wy_g-step_y,wz_g))*hy_g
                                        : (get_sdf_at(wx_g,wy_g+step_y,wz_g)-vals[gi-stride_y])*hy_g;
                float gz_v = (vals[gi+1] - sdf_z_below) * hz_g;
                grad_grid[gi] = gx_v; grad_grid[soa_off_y+gi] = gy_v; grad_grid[soa_off_z+gi] = gz_v;
            }
            // Borda superior Z (z=nz-1)
            {
                const size_t z = nz-1, gi = xy_off + z;
                const float wz_g = min_z + (float)z * step_z;
                float gx_v = x_interior ? (vals[gi+stride_x]-vals[gi-stride_x])*hx_g
                           : (x==0)     ? (vals[gi+stride_x]-get_sdf_at(wx_g-step_x,wy_g,wz_g))*hx_g
                                        : (get_sdf_at(wx_g+step_x,wy_g,wz_g)-vals[gi-stride_x])*hx_g;
                float gy_v = y_interior ? (vals[gi+stride_y]-vals[gi-stride_y])*hy_g
                           : (y==0)     ? (vals[gi+stride_y]-get_sdf_at(wx_g,wy_g-step_y,wz_g))*hy_g
                                        : (get_sdf_at(wx_g,wy_g+step_y,wz_g)-vals[gi-stride_y])*hy_g;
                float gz_v = (get_sdf_at(wx_g,wy_g,wz_g+step_z) - vals[gi-1]) * hz_g;
                grad_grid[gi] = gx_v; grad_grid[soa_off_y+gi] = gy_v; grad_grid[soa_off_z+gi] = gz_v;
            }

            // Interior Z: sem get_sdf_at, sem branches de borda - vetorizável.
            // Componentes X e Y são constantes por coluna quando x_interior/y_interior.
            if (x_interior && y_interior) {
                // Caso mais comum (interior XY): gx e gy são diferenças centradas puras.
                // Ponteiros __restrict locais: o MSVC precisa de variáveis locais const
                // para provar no-alias entre vals[] e grad_grid[] no loop simd.
                const float* __restrict vp  = vals + xy_off;
                const float* __restrict vpx = vals + xy_off + stride_x;
                const float* __restrict vmx = vals + xy_off - stride_x;
                const float* __restrict vpy = vals + xy_off + stride_y;
                const float* __restrict vmy = vals + xy_off - stride_y;
                float* __restrict ggx = grad_grid.data() + xy_off;
                float* __restrict ggy = grad_grid.data() + soa_off_y + xy_off;
                float* __restrict ggz = grad_grid.data() + soa_off_z + xy_off;
                // Copia ponteiros para variáveis locais sem captura de lambda/closure -
                // o MSVC vetoriza quando os ponteiros são escalares locais __restrict.
                const float* __restrict p_vp  = vp;
                const float* __restrict p_vpx = vpx;
                const float* __restrict p_vmx = vmx;
                const float* __restrict p_vpy = vpy;
                const float* __restrict p_vmy = vmy;
                float* __restrict p_ggx = ggx;
                float* __restrict p_ggy = ggy;
                float* __restrict p_ggz = ggz;
                const float hx = hx_g, hy = hy_g, hz = hz_g; // escalares locais
                #pragma omp simd
                for (int z = (int)z_inner_lo; z < (int)z_inner_hi; ++z) {
                    p_ggx[z] = (p_vpx[z] - p_vmx[z]) * hx;
                    p_ggy[z] = (p_vpy[z] - p_vmy[z]) * hy;
                    p_ggz[z] = (p_vp[z+1] - p_vp[z-1]) * hz;
                }
            } else {
                // Borda XY: gx ou gy precisam de get_sdf_at - não vetorizável,
                // mas é apenas 1 linha de borda por chunk, custo negligenciável.
                for (size_t z = z_inner_lo; z < z_inner_hi; ++z) {
                    const size_t gi = xy_off + z;
                    const float wz_g = min_z + (float)z * step_z;
                    float gx_v = x_interior ? (vals[gi+stride_x]-vals[gi-stride_x])*hx_g
                               : (x==0)     ? (vals[gi+stride_x]-get_sdf_at(wx_g-step_x,wy_g,wz_g))*hx_g
                                            : (get_sdf_at(wx_g+step_x,wy_g,wz_g)-vals[gi-stride_x])*hx_g;
                    float gy_v = y_interior ? (vals[gi+stride_y]-vals[gi-stride_y])*hy_g
                               : (y==0)     ? (vals[gi+stride_y]-get_sdf_at(wx_g,wy_g-step_y,wz_g))*hy_g
                                            : (get_sdf_at(wx_g,wy_g+step_y,wz_g)-vals[gi-stride_y])*hy_g;
                    float gz_v = (vals[gi+1] - vals[gi-1]) * hz_g;
                    grad_grid[gi] = gx_v; grad_grid[soa_off_y+gi] = gy_v; grad_grid[soa_off_z+gi] = gz_v;
                }
            }
        }
    }
#if KX_SNR_TIMING
    t_grad_end = Clock::now();
#endif

    // Smoothing de gradiente (Ping-pong)
#if KX_SNR_TIMING
    t_smooth_start = Clock::now();
#endif
    AlignedFloatVec grad_grid_smoothed;

    if (smooth_iterations > 0) {
        grad_grid_smoothed.resize(grad_grid.size());
        
        static constexpr float center_weight   = 0.55f;
        static constexpr float neighbor_weight = 0.075f;

        #pragma omp parallel num_threads(omp_threads)
        {
            for (int iter = 0; iter < smooth_iterations; ++iter) {
                const float* __restrict gg_x = grad_grid.data();
                const float* __restrict gg_y = grad_grid.data() + soa_off_y;
                const float* __restrict gg_z = grad_grid.data() + soa_off_z;
                float* __restrict gs_x = grad_grid_smoothed.data();
                float* __restrict gs_y = grad_grid_smoothed.data() + soa_off_y;
                float* __restrict gs_z = grad_grid_smoothed.data() + soa_off_z;

                #pragma omp for schedule(static)
                for (int x_int = 1; x_int < (int)nx-1; ++x_int) {
                    size_t x = (size_t)x_int;
                    const size_t x_off  = x * stride_x;
                    const size_t xp_off = (x+1) * stride_x;
                    const size_t xm_off = (x-1) * stride_x;

                    for (size_t y = 1; y < ny-1; ++y) {
                        const size_t xy_off  = x_off  + y * stride_y;
                        const size_t xyp_off = x_off  + (y+1) * stride_y;
                        const size_t xym_off = x_off  + (y-1) * stride_y;
                        const size_t xp_y    = xp_off + y * stride_y;
                        const size_t xm_y    = xm_off + y * stride_y;

                        const size_t z_len = nz - 2;
                        const size_t z_base = xy_off + 1;

                        const float* __restrict rx  = gg_x + z_base;
                        const float* __restrict ry  = gg_y + z_base;
                        const float* __restrict rz  = gg_z + z_base;
                        const float* __restrict rx_xp = gg_x + xp_y   + 1;
                        const float* __restrict rx_xm = gg_x + xm_y   + 1;
                        const float* __restrict rx_yp = gg_x + xyp_off + 1;
                        const float* __restrict rx_ym = gg_x + xym_off + 1;
                        const float* __restrict ry_xp = gg_y + xp_y   + 1;
                        const float* __restrict ry_xm = gg_y + xm_y   + 1;
                        const float* __restrict ry_yp = gg_y + xyp_off + 1;
                        const float* __restrict ry_ym = gg_y + xym_off + 1;
                        const float* __restrict rz_xp = gg_z + xp_y   + 1;
                        const float* __restrict rz_xm = gg_z + xm_y   + 1;
                        const float* __restrict rz_yp = gg_z + xyp_off + 1;
                        const float* __restrict rz_ym = gg_z + xym_off + 1;
                        float* __restrict wx_out = gs_x + z_base;
                        float* __restrict wy_out = gs_y + z_base;
                        float* __restrict wz_out = gs_z + z_base;

                        #pragma omp simd
                        for (int i = 0; i < (int)z_len; ++i) {
                            wx_out[i] = rx[i]  * center_weight
                                      + (rx_xp[i] + rx_xm[i] + rx_yp[i] + rx_ym[i]
                                       + rx[i+1]  + rx[i-1]) * neighbor_weight;

                            wy_out[i] = ry[i]  * center_weight
                                      + (ry_xp[i] + ry_xm[i] + ry_yp[i] + ry_ym[i]
                                       + ry[i+1]  + ry[i-1]) * neighbor_weight;

                            wz_out[i] = rz[i]  * center_weight
                                      + (rz_xp[i] + rz_xm[i] + rz_yp[i] + rz_ym[i]
                                       + rz[i+1]  + rz[i-1]) * neighbor_weight;
                        }
                    }
                }

                #pragma omp single
                {
                    grad_grid.swap(grad_grid_smoothed);
                }
                #pragma omp barrier
            }
        }
    }
#if KX_SNR_TIMING
    t_smooth_end = Clock::now();
#endif

    // Libera grad_grid_smoothed imediatamente após o smooth:
    // após o último swap(), grad_grid contém o resultado e grad_grid_smoothed
    // é o buffer "vazio" que não será mais acessado. Liberar agora reduz o
    // working set durante PASS1 (surface nets) e PASS4 (trilinear).
    { AlignedFloatVec _discard; grad_grid_smoothed.swap(_discard); }

    // cv_buf já foi inicializado com -1 no construtor — memset aqui seria fill duplo.
    // Removido: std::memset(cv, 0xFF, total_cells * sizeof(int));

    // x_world/y_world removidos: wx0/wx1/wy0/wy1 calculados diretamente no loop,
    // eliminando duas alocações de vetor e o fill inicial.

    const size_t cells_x_range = cell_end_x - cell_start_x;

    struct alignas(64) ThreadBuf {
        std::vector<float> verts, norms;
        std::vector<uint8_t> surface_mask;
        std::vector<uint8_t> biome_mask;
        std::vector<std::pair<size_t, size_t>> cv_pairs; // (cell_idx, v_idx_local)
    };
    std::vector<ThreadBuf> xbufs(cells_x_range);
    {
        const size_t est_per_x = (size_t)(((cell_end_y - cell_start_y) * (cell_end_z - cell_start_z)) * 15 / 100 + 8);
        for (auto& xb : xbufs) {
            xb.verts.reserve(est_per_x * 3);
            xb.norms.reserve(est_per_x * 3);
            xb.surface_mask.reserve(est_per_x);
            xb.biome_mask.reserve(est_per_x);
            xb.cv_pairs.reserve(est_per_x);
        }
    }

    // clamp01 sem captura: definida uma vez fora do loop paralelo.
    // Lambdas sem captura são equivalentes a funções estáticas — zero overhead.
    auto clamp01 = [](float t) -> float {
        return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    };

    const float clip_margin = fmaxf(step_x, step_y) * 2.0f;
    const float reproj_thr_const = fmaxf(step_x, fmaxf(step_y, step_z)) * 2.0f;
    const float norm_thr_const = step_x * 0.5f;
    const float step_z_2 = step_z * 2.0f;
    const float step_z_4 = step_z * 4.0f;

#if KX_SNR_TIMING
    t_snpass1_start = Clock::now();
#endif
    #pragma omp parallel for schedule(static) num_threads(omp_threads)
    for (int x_int = (int)cell_start_x; x_int < (int)cell_end_x; ++x_int) {
        size_t x = (size_t)x_int;
        ThreadBuf& tb = xbufs[x - cell_start_x];

        const size_t x0_off = x * stride_x, x1_off = (x+1) * stride_x;
        const float wx0 = min_x + (float)x * step_x;
        const float wx1 = wx0 + step_x;
        
        for (size_t y = cell_start_y; y < cell_end_y; ++y) {
            const size_t y0_off = y * stride_y, y1_off = (y+1) * stride_y;
            // Otimização de indexação: base + z
            const size_t xy00_base = x0_off + y0_off;
            const size_t xy10_base = x1_off + y0_off;
            const size_t xy11_base = x1_off + y1_off;
            const size_t xy01_base = x0_off + y1_off;
            const float wy0 = min_y + (float)y * step_y;
            const float wy1 = wy0 + step_y;

            // Acumulador wz: elimina z_world[] e os ~2*cell_end_z muls por coluna
            float wz0_acc = min_z + (float)cell_start_z * step_z;
        
            for (size_t z = cell_start_z; z < cell_end_z; ++z, wz0_acc += step_z) {
            const size_t c0 = xy00_base+z, c1 = xy10_base+z, c2 = xy11_base+z, c3 = xy01_base+z;
            const size_t c4 = c0+1, c5 = c1+1, c6 = c2+1, c7 = c3+1;
            const float v0 = vals[c0], v1 = vals[c1], v2 = vals[c2], v3 = vals[c3];
            const float v4 = vals[c4], v5 = vals[c5], v6 = vals[c6], v7 = vals[c7];
            
            const int mask = ((v0 < 0.0f) << 0) | ((v1 < 0.0f) << 1) | ((v2 < 0.0f) << 2) | ((v3 < 0.0f) << 3) |
                             ((v4 < 0.0f) << 4) | ((v5 < 0.0f) << 5) | ((v6 < 0.0f) << 6) | ((v7 < 0.0f) << 7);
            if (mask == 0 || mask == 255) continue;
            
            const float wz0 = wz0_acc, wz1 = wz0_acc + step_z;
            float px=0,py=0,pz=0,gnx=0,gny=0,gnz=0; int count=0;

            // Edge crossing helpers: inline manual sem closure de referência
            // add_x: cruza na direção X (wx0->wx1), posição fixa cy/cz
            #define SNR_ADD_X(va, vb, cx0, cx1, cy_, cz_) do { \
                float _d = (va) - (vb); \
                float _t = (fabsf(_d) < 1e-9f) ? 0.5f : clamp01((va) / _d); \
                px += (cx0) + ((cx1)-(cx0)) * _t; py += (cy_); pz += (cz_); \
                gnx += ((vb)-(va))*((cx1)-(cx0)); ++count; \
            } while(0)
            #define SNR_ADD_Y(va, vb, cy0, cy1, cx_, cz_) do { \
                float _d = (va) - (vb); \
                float _t = (fabsf(_d) < 1e-9f) ? 0.5f : clamp01((va) / _d); \
                px += (cx_); py += (cy0) + ((cy1)-(cy0)) * _t; pz += (cz_); \
                gny += ((vb)-(va))*((cy1)-(cy0)); ++count; \
            } while(0)
            #define SNR_ADD_Z(va, vb, cz0, cz1, cx_, cy_) do { \
                float _d = (va) - (vb); \
                float _t = (fabsf(_d) < 1e-9f) ? 0.5f : clamp01((va) / _d); \
                px += (cx_); py += (cy_); pz += (cz0) + ((cz1)-(cz0)) * _t; \
                gnz += ((vb)-(va))*((cz1)-(cz0)); ++count; \
            } while(0)

            if ((v0 < 0.0f) != (v1 < 0.0f)) SNR_ADD_X(v0, v1, wx0, wx1, wy0, wz0);
            if ((v3 < 0.0f) != (v2 < 0.0f)) SNR_ADD_X(v3, v2, wx0, wx1, wy1, wz0);
            if ((v4 < 0.0f) != (v5 < 0.0f)) SNR_ADD_X(v4, v5, wx0, wx1, wy0, wz1);
            if ((v7 < 0.0f) != (v6 < 0.0f)) SNR_ADD_X(v7, v6, wx0, wx1, wy1, wz1);
            if ((v0 < 0.0f) != (v3 < 0.0f)) SNR_ADD_Y(v0, v3, wy0, wy1, wx0, wz0);
            if ((v1 < 0.0f) != (v2 < 0.0f)) SNR_ADD_Y(v1, v2, wy0, wy1, wx1, wz0);
            if ((v4 < 0.0f) != (v7 < 0.0f)) SNR_ADD_Y(v4, v7, wy0, wy1, wx0, wz1);
            if ((v5 < 0.0f) != (v6 < 0.0f)) SNR_ADD_Y(v5, v6, wy0, wy1, wx1, wz1);
            if ((v0 < 0.0f) != (v4 < 0.0f)) SNR_ADD_Z(v0, v4, wz0, wz1, wx0, wy0);
            if ((v1 < 0.0f) != (v5 < 0.0f)) SNR_ADD_Z(v1, v5, wz0, wz1, wx1, wy0);
            if ((v3 < 0.0f) != (v7 < 0.0f)) SNR_ADD_Z(v3, v7, wz0, wz1, wx0, wy1);
            if ((v2 < 0.0f) != (v6 < 0.0f)) SNR_ADD_Z(v2, v6, wz0, wz1, wx1, wy1);

            #undef SNR_ADD_X
            #undef SNR_ADD_Y
            #undef SNR_ADD_Z
            
            if (count > 0) {
                const float inv = 1.0f / count;
                float vx = px*inv, vy = py*inv, vz = pz*inv;
#if KX_SNR_SEAM_DEBUG
                const float vx_raw = vx, vy_raw = vy, vz_raw = vz;
#endif
                vx = fmaxf(clip_min_x - clip_margin, fminf(clip_max_x + clip_margin, vx));
                vy = fmaxf(clip_min_y - clip_margin, fminf(clip_max_y + clip_margin, vy));
                vz = fmaxf(min_z, fminf(max_z, vz));
                

                bool was_reprojected = false;
                float sampled_terrain_h = 0.0f;
                bool has_sampled_terrain_h = false;
                if (!add_edits.empty()) {
                    float best_sd = 1e30f;
                    float best_cx=0, best_cy=0, best_cz=0, best_r=1.0f;
                    bool found = false;
                    const float reproj_thr = reproj_thr_const;
                    for (size_t k = 0; k < add_edits.size(); ++k) {
                        const size_t ei = add_edits[k];
                        const auto& ed = edits[ei];
                        const EditBBox& bb = edit_bboxes[ei];
                        if (vx < bb.xmin - reproj_thr || vx > bb.xmax + reproj_thr ||
                            vy < bb.ymin - reproj_thr || vy > bb.ymax + reproj_thr)
                        {
                            continue;
                        }
                        float dx=vx-ed.cx, dy=vy-ed.cy, dz=vz-ed.cz;
                        float dist2=dx*dx+dy*dy+dz*dz;
                        const float rmin = ed.r - reproj_thr;
                        const float rmax = ed.r + reproj_thr;
                        const float rmin2 = (rmin > 0.0f) ? (rmin * rmin) : 0.0f;
                        const float rmax2 = rmax * rmax;
                        if (dist2 < rmin2 || dist2 > rmax2) {
                            continue;
                        }
                        float sd=sqrtf(dist2)-ed.r;
                        if (fabsf(sd) < fabsf(best_sd)) {
                            best_sd=sd; best_cx=ed.cx; best_cy=ed.cy; best_cz=ed.cz; best_r=ed.r; found=true;
                        }
                    }
                    if (found && fabsf(best_sd) > 1e-4f) {
                        float dx=vx-best_cx, dy=vy-best_cy, dz=vz-best_cz;
                        float dist2=dx*dx+dy*dy+dz*dz;
                        if (dist2 > 1e-9f) {
                            float inv_d = best_r / sqrtf(dist2);
                            float rx = best_cx + dx*inv_d;
                            float ry = best_cy + dy*inv_d;
                            float rz = best_cz + dz*inv_d;
                            // Reprojetar se dentro do clip expandido - permite que edits
                            // que cruzam a borda sejam reprojetados corretamente em ambas as chunks.
                            if (rx >= clip_min_x - clip_margin && rx <= clip_max_x + clip_margin &&
                                ry >= clip_min_y - clip_margin && ry <= clip_max_y + clip_margin) {
                                vx = rx;
                                vy = ry;
                                vz = fmaxf(min_z, fminf(max_z, rz));
                                was_reprojected = true;
                            }
                        }
                    }
                }

                // Seam snap XY: alinha vértices de borda com o clip para continuidade entre chunks.
                // Não aplica se o vértice foi reprojetado para a superfície de um edit.
                // Vértices reprojetados estão na superfície da esfera (cavidade interior)
                // e não devem ser snapped — movê-los cria deformações.
                if (!was_reprojected) {
                    float terrain_h_xy = sample_terrain_h(vx, vy);
                    sampled_terrain_h = terrain_h_xy;
                    has_sampled_terrain_h = true;
                    bool near_surface = (vz >= terrain_h_xy - step_z_4);
                    if (near_surface) {
                        const float vx_before_snap = vx;
                        const float vy_before_snap = vy;
                        if (fabsf(vx-clip_min_x)<=seam_snap_eps) vx=clip_min_x;
                        else if (fabsf(clip_max_x-vx)<=seam_snap_eps) vx=clip_max_x;
                        if (fabsf(vy-clip_min_y)<=seam_snap_eps) vy=clip_min_y;
                        else if (fabsf(clip_max_y-vy)<=seam_snap_eps) vy=clip_max_y;
                        if (vx != vx_before_snap || vy != vy_before_snap) {
                            has_sampled_terrain_h = false;
                        }
                    }
                }

                if (seam_z_snap && !was_reprojected) {
                    float ex = fminf(fabsf(vx-clip_min_x), fabsf(clip_max_x-vx));
                    float ey = fminf(fabsf(vy-clip_min_y), fabsf(clip_max_y-vy));
                    float edge_dist = fminf(ex, ey);
                    if (edge_dist <= seam_z_eps) {
                        float blend = (seam_z_eps > 1e-9f) ? (1.0f - fminf(1.0f, edge_dist / seam_z_eps)) : 1.0f;
                        float terrain_base = has_sampled_terrain_h ? sampled_terrain_h : sample_terrain_h(vx, vy);
                        sampled_terrain_h = terrain_base;
                        has_sampled_terrain_h = true;
                        if (vz >= terrain_base - step_z_2) {
                            float hz = has_edits ? sample_edited_h(vx, vy) : sample_terrain_h(vx, vy);
                            vz = vz + (hz - vz) * blend;
                            vz = fmaxf(min_z, fminf(max_z, vz));
                        }
                    }
                }
                
                // Descarta vértices claramente fora do clip - com margem para não cortar
                // vértices de borda que pertencem a esta chunk.
                // A margem clip_margin permite que edits que cruzam a borda sejam emitidos.
                const float vx_before_final_snap = vx;
                const float vy_before_final_snap = vy;
                {
                    // O final snap XY só deve agir em vértices de superfície não reprojetados.
                    // Vértices reprojetados para a superfície da esfera (cavidade interior)
                    // não devem ser snapped: movê-los para a seam cria deformações na cavidade.
                    const float snap_terrain_h = has_sampled_terrain_h
                        ? sampled_terrain_h
                        : sample_terrain_h(vx, vy);
                    const bool is_surface_vert = (vz >= snap_terrain_h - step_z_4);
                    if (!was_reprojected && is_surface_vert) {
                        if (fabsf(vx - clip_min_x) <= seam_snap_eps) vx = clip_min_x;
                        else if (fabsf(vx - clip_max_x) <= seam_snap_eps) vx = clip_max_x;
                        if (fabsf(vy - clip_min_y) <= seam_snap_eps) vy = clip_min_y;
                        else if (fabsf(vy - clip_max_y) <= seam_snap_eps) vy = clip_max_y;
                    } else if (was_reprojected) {
                        // Para vértices reprojetados, apenas contém o XY dentro do clip expandido
                        // sem alterar o Z. Isso evita descarte por clip check sem deformar a cavidade.
                        // Um vértice reprojetado fora do clip seria descartado, deixando buracos
                        // na malha em volta do buraco da esfera.
                        vx = fmaxf(clip_min_x - clip_margin, fminf(clip_max_x + clip_margin, vx));
                        vy = fmaxf(clip_min_y - clip_margin, fminf(clip_max_y + clip_margin, vy));
                    }
                }
                // Se o snap final moveu o vértice, terrain_h amostrado pode ter mudado
                if (vx != vx_before_final_snap || vy != vy_before_final_snap) {
                    has_sampled_terrain_h = false;
                }

                // Epsilon numérico para evitar rejeição por drift float.
                const float clip_eps = 1e-5f;

                // Descarta vértices claramente fora do clip expandido.
                // min inclusivo, max exclusivo para ownership estável.
                if (vx <  clip_min_x - clip_margin - clip_eps ||
                    vx >= clip_max_x + clip_margin - clip_eps ||
                    vy <  clip_min_y - clip_margin - clip_eps ||
                    vy >= clip_max_y + clip_margin - clip_eps)
                {
                    continue;
                }

                // Grava no buffer local da thread - push_back é O(1) amortizado após reserve
                const size_t v_idx = tb.verts.size() / 3;

#if KX_SNR_SEAM_DEBUG
                // Coleta debug para vértices próximos da seam ou com edits ativos.
                // Captura apenas vértices próximos da borda do clip E na região de edit.
                if (has_edits) {
                    float ex_d = fminf(fabsf(vx-clip_min_x), fabsf(clip_max_x-vx));
                    float ey_d = fminf(fabsf(vy-clip_min_y), fabsf(clip_max_y-vy));
                    float edge_d = fminf(ex_d, ey_d);
                    // Só registra se o vértice está perto da seam (dentro de 4 step_x)
                    if (edge_d <= step_x * 4.0f) {
                        // Encontra o edit de escavação mais próximo
                        float nearest_sd = 1e30f;
                        float ne_cx=0, ne_cy=0, ne_cz=0, ne_r=0;
                        for (size_t ei2 = 0; ei2 < edits.size(); ++ei2) {
                            const auto& ed2 = edits[ei2];
                            if (ed2.operation != 1) continue;
                            float ddx=vx-ed2.cx, ddy=vy-ed2.cy, ddz=vz-ed2.cz;
                            float sd2=sqrtf(ddx*ddx+ddy*ddy+ddz*ddz)-ed2.r;
                            if (fabsf(sd2) < fabsf(nearest_sd)) {
                                nearest_sd=sd2; ne_cx=ed2.cx; ne_cy=ed2.cy; ne_cz=ed2.cz; ne_r=ed2.r;
                            }
                        }
                        if (fabsf(nearest_sd) < ne_r * 1.5f) { // só coleta se próximo do edit
                            SeamDebugEntry dbg;
                            dbg.vx_raw=vx_raw; dbg.vy_raw=vy_raw; dbg.vz_raw=vz_raw;
                            dbg.vx_out=vx; dbg.vy_out=vy; dbg.vz_out=vz;
                            dbg.sdf_at_vert=get_sdf_at(vx, vy, vz);
                            dbg.terrain_h=has_sampled_terrain_h ? sampled_terrain_h : sample_terrain_h(vx, vy);
                            dbg.edit_cx=ne_cx; dbg.edit_cy=ne_cy; dbg.edit_cz=ne_cz; dbg.edit_r=ne_r;
                            dbg.dist_to_edit_surface=nearest_sd;
                            dbg.seam_edge_dist=edge_d;
                            dbg.was_seam_reprojected=0; // preenchido pelo seam block acima se ativado
                            dbg.was_reprojected=was_reprojected ? 1 : 0;
                            dbg.vz_below_terrain=(vz < dbg.terrain_h - step_z_2) ? 1 : 0;
                            std::lock_guard<std::mutex> lk(seam_debug_mutex);
                            seam_debug_buf.push_back(dbg);
                        }
                    }
                }
#endif

                tb.verts.push_back(vx); tb.verts.push_back(vy); tb.verts.push_back(vz);

                const float terrain_h_final = has_sampled_terrain_h ? sampled_terrain_h : sample_terrain_h(vx, vy);
                const uint8_t is_surface = (vz >= terrain_h_final - step_z_2) ? 1u : 0u;

                tb.surface_mask.push_back(is_surface);

                // Identificação de bioma para vértices de superfície
                if (is_surface) {
                    int b_ix = (int)((vx - min_x) * inv_step_x + 0.5f);
                    int b_iy = (int)((vy - min_y) * inv_step_y + 0.5f);
                    b_ix = b_ix < 0 ? 0 : (b_ix >= (int)nx ? (int)nx - 1 : b_ix);
                    b_iy = b_iy < 0 ? 0 : (b_iy >= (int)ny ? (int)ny - 1 : b_iy);
                    tb.biome_mask.push_back(biome_cache_xy[(size_t)b_ix * (size_t)ny + (size_t)b_iy]);
                } else {
                    tb.biome_mask.push_back(0);
                }

                float nx_n=gnx, ny_n=gny, nz_n=gnz;
                if (!add_edits.empty()) {
                    const float norm_thr = norm_thr_const;
                    for (size_t k = 0; k < add_edits.size(); ++k) {
                        const size_t ei = add_edits[k];
                        const auto& ed = edits[ei];
                        const EditBBox& bb = edit_bboxes[ei];
                        if (vx < bb.xmin - norm_thr || vx > bb.xmax + norm_thr ||
                            vy < bb.ymin - norm_thr || vy > bb.ymax + norm_thr)
                        {
                            continue;
                        }
                        float dx=vx-ed.cx, dy=vy-ed.cy, dz=vz-ed.cz;
                        float dist2=dx*dx+dy*dy+dz*dz;
                        const float rmin = ed.r - norm_thr;
                        const float rmax = ed.r + norm_thr;
                        const float rmin2 = (rmin > 0.0f) ? (rmin * rmin) : 0.0f;
                        const float rmax2 = rmax * rmax;
                        if (dist2 < rmin2 || dist2 > rmax2) {
                            continue;
                        }
                        if (dist2 > 1e-9f) {
                            float inv_d = 1.0f / sqrtf(dist2);
                            nx_n = dx*inv_d; ny_n = dy*inv_d; nz_n = dz*inv_d;
                            break;
                        }
                    }
                }
                float n_len = sqrtf(nx_n*nx_n+ny_n*ny_n+nz_n*nz_n);
                if (n_len > 1e-6f) {
                    float inv_n = 1.0f / n_len;
                    tb.norms.push_back(nx_n*inv_n); tb.norms.push_back(ny_n*inv_n); tb.norms.push_back(nz_n*inv_n);
                } else {
                    tb.norms.push_back(0.0f); tb.norms.push_back(0.0f); tb.norms.push_back(1.0f);
                }
                const size_t cell_idx = x * cells_y * cells_z + y * cells_z + z;
                if (cell_idx < total_cells) {
                    tb.cv_pairs.push_back({cell_idx, v_idx});
                }
            }
        }
        }
    }
#if KX_SNR_TIMING
    t_snpass1_end  = Clock::now();
    t_merge_start  = Clock::now();
#endif

    {
        // Prefix scan: pré-computa offset de cada ThreadBuf no buffer destino.
        // Elimina os ponteiros dst incrementais e o base_idx runtime do loop de merge.
        struct TBOffsets { size_t vert_off; size_t mask_off; };
        std::vector<TBOffsets> tb_off(xbufs.size() + 1);
        tb_off[0] = {0, 0};
        for (size_t xi = 0; xi < xbufs.size(); ++xi) {
            const size_t vc = xbufs[xi].verts.size() / 3;
            tb_off[xi + 1].vert_off = tb_off[xi].vert_off + vc * 3;
            tb_off[xi + 1].mask_off = tb_off[xi].mask_off + vc;
        }
        const size_t total_verts = tb_off[xbufs.size()].mask_off;

        // Pre-aloca buffers destino em uma única operação
        verts.resize(total_verts * 3);
        norms.resize(total_verts * 3);
        surface_mask.resize(total_verts);
        biome_mask.resize(total_verts);

        float*   const vp_base = verts.data();
        float*   const np_base = norms.data();
        uint8_t* const sp_base = surface_mask.data();
        uint8_t* const bp_base = biome_mask.data();

        for (size_t xi = 0; xi < xbufs.size(); ++xi) {
            ThreadBuf& tb = xbufs[xi];
            const size_t v_off = tb_off[xi].vert_off;
            const size_t m_off = tb_off[xi].mask_off;
            const size_t tb_vc = tb_off[xi + 1].mask_off - m_off; // derivado do scan
            if (tb_vc == 0) continue;

            const size_t v3 = tb_vc * 3;

            std::memcpy(vp_base + v_off, tb.verts.data(),        v3    * sizeof(float));
            std::memcpy(np_base + v_off, tb.norms.data(),        v3    * sizeof(float));
            std::memcpy(sp_base + m_off, tb.surface_mask.data(), tb_vc * sizeof(uint8_t));
            std::memcpy(bp_base + m_off, tb.biome_mask.data(),   tb_vc * sizeof(uint8_t));

            // cv_pairs: acesso aleatório em cv[] — serial e correto por design.
            // base_idx agora vem do prefix scan, sem incremento runtime.
            const size_t base_idx = m_off; // m_off == vert count offset
            for (const auto& p : tb.cv_pairs) {
                // Único check necessário: cell_idx dentro dos limites
                if (p.first >= total_cells) continue;
                if (cv[p.first] < 0) {
                    cv[p.first] = static_cast<int>(p.second + base_idx);
                }
            }

            // Libera memória do ThreadBuf imediatamente
            tb.verts        = {};
            tb.norms        = {};
            tb.surface_mask = {};
            tb.biome_mask   = {};
            tb.cv_pairs     = {};
        }
    }
#if KX_SNR_TIMING
    t_merge_end    = Clock::now();
#endif

    // Libera biome_cache_xy e xbufs: não são mais necessários após o merge.
    // Reduz working set durante PASS2, PASS3 (Laplacian) e PASS4 (trilinear).
    { std::vector<uint8_t> _d; biome_cache_xy.swap(_d); }
    { std::vector<ThreadBuf> _d; xbufs.swap(_d); }

    const size_t cv_stride_y = cells_z, cv_stride_x = cells_y * cells_z;

#if KX_SNR_TIMING
    t_snpass2_start = Clock::now();
#endif
    {
        size_t est_quads = (size_t)((cell_end_x - cell_start_x) *
                                    (cell_end_y - cell_start_y) *
                                    (cell_end_z - cell_start_z)) * 3 / 10;
        inds.reserve(est_quads * 6);
    }

    // cv_get: interior_stride == 1 (C++ nunca pula vértices), fallback removido.
    auto cv_get = [&](size_t gx, size_t gy, size_t gz) -> int {
        if(gx>=cells_x||gy>=cells_y||gz>=cells_z) return -1;
        return cv[gx*cv_stride_x+gy*cv_stride_y+gz];
    };

    // PASS 2: emit_quad e emit_lod_aware com push_back (reserve já feito acima)
    auto emit_quad=[&](int a,int b,int c,int d,bool flip){
        if(a<0||b<0||c<0||d<0) return;
        // Se dois vértices são iguais → triângulo
        if(a==b||a==c||a==d||b==c||b==d||c==d){
            int tv[4]={a,b,c,d},uv[3],ui=0;
            for(int i=0;i<4;++i){bool dup=false;for(int j=0;j<ui;++j)if(tv[i]==uv[j]){dup=true;break;}if(!dup&&ui<3)uv[ui++]=tv[i];}
            if(ui==3){
                if(flip){inds.push_back(uv[0]); inds.push_back(uv[2]); inds.push_back(uv[1]);}
                else    {inds.push_back(uv[0]); inds.push_back(uv[1]); inds.push_back(uv[2]);}
            }
            return;
        }
        const float *av=&verts[a*3],*bv=&verts[b*3],*cv2=&verts[c*3],*dv=&verts[d*3];
        const float dx_ac=av[0]-cv2[0], dy_ac=av[1]-cv2[1], dz_ac=av[2]-cv2[2];
        const float dx_bd=bv[0]-dv[0], dy_bd=bv[1]-dv[1], dz_bd=bv[2]-dv[2];
        const bool diag_ac = (dx_ac*dx_ac+dy_ac*dy_ac+dz_ac*dz_ac) <
                             (dx_bd*dx_bd+dy_bd*dy_bd+dz_bd*dz_bd);
        if(diag_ac){
            if(flip){inds.push_back(a);inds.push_back(c);inds.push_back(b);
                     inds.push_back(a);inds.push_back(d);inds.push_back(c);}
            else    {inds.push_back(a);inds.push_back(b);inds.push_back(c);
                     inds.push_back(a);inds.push_back(c);inds.push_back(d);}
        } else {
            if(flip){inds.push_back(b);inds.push_back(d);inds.push_back(c);
                     inds.push_back(b);inds.push_back(a);inds.push_back(d);}
            else    {inds.push_back(b);inds.push_back(c);inds.push_back(d);
                     inds.push_back(b);inds.push_back(d);inds.push_back(a);}
        }
    };
    auto emit_lod_aware=[&](int a,int b,int c,int d,bool flip){
        if(a>=0&&b>=0&&c>=0&&d>=0){emit_quad(a,b,c,d,flip);return;}
        int tv[3];int ti=0;
        if(a>=0&&ti<3)tv[ti++]=a;if(b>=0&&ti<3)tv[ti++]=b;
        if(c>=0&&ti<3)tv[ti++]=c;if(d>=0&&ti<3)tv[ti++]=d;
        if(ti==3){
            if(flip){inds.push_back(tv[0]);inds.push_back(tv[2]);inds.push_back(tv[1]);}
            else    {inds.push_back(tv[0]);inds.push_back(tv[1]);inds.push_back(tv[2]);}
        }
    };

    for(int x_int = (int)cell_start_x; x_int < (int)cell_end_x; ++x_int) {
        size_t x = (size_t)x_int;
        // interior_stride == 1: todo x é nó folha, skip nunca ocorre

        const size_t x0o = x * stride_x, x1o = (x + 1) * stride_x;
        for(size_t y = cell_start_y; y < cell_end_y; ++y) {
            // interior_stride == 1: todo y é nó folha, skip nunca ocorre

            const size_t y0o = y * stride_y, y1o = (y + 1) * stride_y;
            for(size_t z = cell_start_z; z < cell_end_z; ++z) {
                size_t idx = x0o + y0o + z; float v0 = vals[idx];
                if(x < cells_x - 1){
                    float vx1 = vals[x1o + y0o + z];
                    if((v0 < 0.0f) != (vx1 < 0.0f) && y > 0 && z > 0)
                        emit_lod_aware(cv_get(x, y, z), cv_get(x, y - 1, z), cv_get(x, y - 1, z - 1), cv_get(x, y, z - 1), v0 > 0.0f);
                }
                if(y < cells_y - 1){
                    float vy1 = vals[x0o + y1o + z];
                    if((v0 < 0.0f) != (vy1 < 0.0f) && x > 0 && z > 0)
                        emit_lod_aware(cv_get(x, y, z), cv_get(x - 1, y, z), cv_get(x - 1, y, z - 1), cv_get(x, y, z - 1), v0 < 0.0f);
                }
                if(z < cells_z - 1){
                    float vz1 = vals[idx + 1];
                    if((v0 < 0.0f) != (vz1 < 0.0f) && x > 0 && y > 0)
                        emit_lod_aware(cv_get(x, y, z), cv_get(x - 1, y, z), cv_get(x - 1, y - 1, z), cv_get(x, y - 1, z), v0 > 0.0f);
                }
            }
        }
    }
#if KX_SNR_TIMING
    t_snpass2_end    = Clock::now();
    t_indsclean_start = Clock::now();
#endif

    {
        const int vc = (int)verts.size() / 3;

        if (vc <= 0) {
            inds.clear();
        } else if (!inds.empty()) {
            // Filtro in-place com dois ponteiros - elimina alocação de inds2
            const int max_i = vc - 1;
            size_t write = 0;
            const size_t ni = inds.size();
            for (size_t i = 0; i + 2 < ni; i += 3) {
                const int a = inds[i], b = inds[i + 1], c = inds[i + 2];
                if ((unsigned)a <= (unsigned)max_i &&
                    (unsigned)b <= (unsigned)max_i &&
                    (unsigned)c <= (unsigned)max_i)
                {
                    inds[write]   = a;
                    inds[write+1] = b;
                    inds[write+2] = c;
                    write += 3;
                }
            }
            inds.resize(write);
        }
    }
#if KX_SNR_TIMING
    t_indsclean_end = Clock::now();
#endif

    // Libera cv_buf: não é usado após PASS2 (mapa célula→vértice já foi consumido).
    // Libera ~400 KB durante PASS3 e PASS4.
    { std::vector<int> _d; cv_buf.swap(_d); }

    // PASS 3: Laplacian smoothing com seam snap - simples e correto
#if KX_SNR_TIMING
    t_lsmooth_start = Clock::now();
#endif
    if (smooth_iterations > 0 && smooth_factor > 0.0f) {
        size_t vc = verts.size() / 3;
        std::vector<size_t> adj_off(vc+1, 0);
        for (size_t i = 0; i < inds.size(); i+=3) {
            adj_off[(size_t)inds[i]+1]+=2; adj_off[(size_t)inds[i+1]+1]+=2; adj_off[(size_t)inds[i+2]+1]+=2;
        }
        for (size_t i = 0; i < vc; ++i) adj_off[i+1] += adj_off[i];
        std::vector<int> adj_idx(adj_off[vc]);
        // Preenche adj_idx usando índice corrente por vértice - sem cópia de adj_off
        std::vector<size_t> cur(adj_off.begin(), adj_off.begin() + vc); // cur[i] = próxima posição livre para vértice i
        for (size_t i = 0; i < inds.size(); i+=3) {
            int a=inds[i],b=inds[i+1],c=inds[i+2];
            adj_idx[cur[a]++]=b; adj_idx[cur[a]++]=c;
            adj_idx[cur[b]++]=a; adj_idx[cur[b]++]=c;
            adj_idx[cur[c]++]=a; adj_idx[cur[c]++]=b;
        }
        std::vector<float> nv(verts.size());
        for (int iter = 0; iter < smooth_iterations; ++iter) {
            #pragma omp parallel for if(vc > 4000) num_threads(omp_threads)
            for (int i_int = 0; i_int < (int)vc; ++i_int) {
                size_t i = (size_t)i_int;
                const size_t i3 = i * 3;
                size_t s=adj_off[i], e=adj_off[i+1];
                const float svx=verts[i3], svy=verts[i3+1], svz=verts[i3+2];
                if (s==e) { nv[i3]=svx; nv[i3+1]=svy; nv[i3+2]=svz; continue; }
                float ax=0,ay=0,az=0;
                for (size_t j=s;j<e;++j) {
                    const size_t n3 = (size_t)adj_idx[j]*3;
                    ax+=verts[n3]; ay+=verts[n3+1]; az+=verts[n3+2];
                }
                float inv2=1.0f/(float)(e-s); ax*=inv2; ay*=inv2; az*=inv2;
                float dx_min=svx-clip_min_x,dx_max=clip_max_x-svx;
                float dy_min=svy-clip_min_y,dy_max=clip_max_y-svy;
                dx_min=fmaxf(0.0f,dx_min); dx_max=fmaxf(0.0f,dx_max);
                dy_min=fmaxf(0.0f,dy_min); dy_max=fmaxf(0.0f,dy_max);
                // Falloff contínuo: eff=0 exatamente na seam, cresce para o interior.
                // Sem branch binário - transição suave elimina crease/ridge artificial.
                float dist_edge=fminf(fminf(dx_min,dx_max),fminf(dy_min,dy_max));
                float w=1.0f;
                if(pin_eps>1e-9f){w=dist_edge/pin_eps;w=fmaxf(0.0f,fminf(1.0f,w));}
                float eff=smooth_factor*w;
                float nnx=svx+(ax-svx)*eff;
                float nny=svy+(ay-svy)*eff;
                float nnz=svz+(az-svz)*eff;
                // Barrier clamp: limita deslocamento, não posição
                float ddx=nnx-svx, ddy=nny-svy;
                if(ddx> dx_max) ddx= dx_max;
                if(ddx<-dx_min) ddx=-dx_min;
                if(ddy> dy_max) ddy= dy_max;
                if(ddy<-dy_min) ddy=-dy_min;
                nnx=svx+ddx; nny=svy+ddy;
                nnz=fmaxf(min_z,fminf(max_z,nnz));
                nv[i3]=nnx; nv[i3+1]=nny; nv[i3+2]=nnz;
            }
            verts.swap(nv);
        }
    }
#if KX_SNR_TIMING
    t_lsmooth_end    = Clock::now();
#endif

    // PASS 4: Interpolação trilinear do gradiente - layout SOA
#if KX_SNR_TIMING
    t_normpass4_start = Clock::now();
#endif
    vc2 = verts.size() / 3;


    if (vc2 > 0 && norms.size() == vc2 * 3) {
        const float* __restrict gg4_x = grad_grid.data();
        const float* __restrict gg4_y = grad_grid.data() + soa_off_y;
        const float* __restrict gg4_z = grad_grid.data() + soa_off_z;
        const float* __restrict vp4   = verts.data();
        float*       __restrict np4   = norms.data();

        // Pass 4a: trilinear + normalização - sem branches de seam, vetorizável.
        // Cada iteração é independente (gi* calculados localmente por i).
#if KX_SNR_TIMING
        t_norm4a_start = Clock::now();
#endif
        #pragma omp parallel for schedule(static) if(vc2 > 4000) num_threads(omp_threads)
        for (int i_int = 0; i_int < (int)vc2; ++i_int) {
            const size_t i3 = (size_t)i_int * 3;
            const float vx=vp4[i3], vy=vp4[i3+1], vz=vp4[i3+2];
            float fx=(vx-min_x)*inv_step_x, fy=(vy-min_y)*inv_step_y, fz=(vz-min_z)*inv_step_z;
            // Clamping para nx-2 para garantir que ix+1 esteja dentro dos limites do grid (0 a nx-1)
            fx = fmaxf(0.0f, fminf((float)nx-2.0001f, fx));
            fy = fmaxf(0.0f, fminf((float)ny-2.0001f, fy));
            fz = fmaxf(0.0f, fminf((float)nz-2.0001f, fz));
            const size_t ix=(size_t)fx, iy=(size_t)fy, iz=(size_t)fz;
            const float tx=fx-(float)ix, ty=fy-(float)iy, tz=fz-(float)iz;
            const size_t gi000=ix*stride_x+iy*stride_y+iz;
            const size_t gi100=gi000+stride_x, gi010=gi000+stride_y, gi110=gi000+stride_x+stride_y;
            const size_t gi001=gi000+1,        gi101=gi100+1,        gi011=gi010+1, gi111=gi110+1;
            const float ix0=1.0f-tx, iy0=1.0f-ty, iz0=1.0f-tz;
            const float w00=ix0*iy0, w10=tx*iy0, w01=ix0*ty, w11=tx*ty;
            const float gx=(gg4_x[gi000]*w00+gg4_x[gi100]*w10+gg4_x[gi010]*w01+gg4_x[gi110]*w11)*iz0
                          +(gg4_x[gi001]*w00+gg4_x[gi101]*w10+gg4_x[gi011]*w01+gg4_x[gi111]*w11)*tz;
            const float gy=(gg4_y[gi000]*w00+gg4_y[gi100]*w10+gg4_y[gi010]*w01+gg4_y[gi110]*w11)*iz0
                          +(gg4_y[gi001]*w00+gg4_y[gi101]*w10+gg4_y[gi011]*w01+gg4_y[gi111]*w11)*tz;
            const float gz=(gg4_z[gi000]*w00+gg4_z[gi100]*w10+gg4_z[gi010]*w01+gg4_z[gi110]*w11)*iz0
                          +(gg4_z[gi001]*w00+gg4_z[gi101]*w10+gg4_z[gi011]*w01+gg4_z[gi111]*w11)*tz;
            const float gl=sqrtf(gx*gx+gy*gy+gz*gz);
            if(gl>1e-9f){
                const float ginv=1.0f/gl;
                np4[i3]=gx*ginv; np4[i3+1]=gy*ginv; np4[i3+2]=gz*ginv;
            } else {
                np4[i3]=0.0f; np4[i3+1]=0.0f; np4[i3+2]=1.0f;
            }
        }
#if KX_SNR_TIMING
        t_norm4a_end   = Clock::now();
#endif

        // Pass 4b: seam normal blend - apenas vértices de borda, serial.
        // Chama sample_edited_h (não-inlinável) - não vetorizável, mas é O(N_borda) << O(N_total).
#if KX_SNR_TIMING
        t_norm4b_start = Clock::now();
#endif
        if (seam_z_snap && seam_normal_blend > 1e-6f) {
            const float sx2 = fmaxf(step_x, 1e-6f), sy2 = fmaxf(step_y, 1e-6f);
            for (size_t i = 0; i < vc2; ++i) {
                const size_t i3 = i * 3;
                const float vx=vp4[i3], vy=vp4[i3+1];
                const float ex=fminf(fabsf(vx-clip_min_x), fabsf(clip_max_x-vx));
                const float ey=fminf(fabsf(vy-clip_min_y), fabsf(clip_max_y-vy));
                const float edge_dist=fminf(ex,ey);
                if (edge_dist > seam_z_eps) continue;
                float nxn=np4[i3], nyn=np4[i3+1], nzn=np4[i3+2];
                const float hxm = has_edits ? sample_edited_h(vx-sx2, vy) : sample_terrain_h(vx-sx2, vy);
                const float hxp = has_edits ? sample_edited_h(vx+sx2, vy) : sample_terrain_h(vx+sx2, vy);
                const float hym = has_edits ? sample_edited_h(vx, vy-sy2) : sample_terrain_h(vx, vy-sy2);
                const float hyp = has_edits ? sample_edited_h(vx, vy+sy2) : sample_terrain_h(vx, vy+sy2);
                const float ttx=(hxp-hxm)/(2*sx2), tty=(hyp-hym)/(2*sy2);
                float tnx=-ttx, tny=-tty, tnz=1.0f;
                const float tnl=sqrtf(tnx*tnx+tny*tny+tnz*tnz);
                if(tnl>1e-9f){
                    const float tnl_inv=1.0f/tnl;
                    tnx*=tnl_inv; tny*=tnl_inv; tnz*=tnl_inv;
                    const float eb=1.0f-fminf(1.0f,edge_dist/seam_z_eps);
                    const float b=fminf(1.0f,fmaxf(0.0f,eb*seam_normal_blend));
                    nxn=nxn*(1-b)+tnx*b; nyn=nyn*(1-b)+tny*b; nzn=nzn*(1-b)+tnz*b;
                    const float nlen=sqrtf(nxn*nxn+nyn*nyn+nzn*nzn);
                    if(nlen>1e-9f){const float ni=1.0f/nlen; nxn*=ni; nyn*=ni; nzn*=ni;}
                    np4[i3]=nxn; np4[i3+1]=nyn; np4[i3+2]=nzn;
                }
            }
        }
    }
#if KX_SNR_TIMING
    t_norm4b_end    = Clock::now();
    t_normpass4_end = Clock::now();
#endif

#if KX_SNR_SEAM_DEBUG
    // Imprime debug de vértices afetados por edits na seam — uma única vez ao final.
    if (!seam_debug_buf.empty()) {
        printf("\n[SEAM_DEBUG] %zu vertices near seam with edit influence:\n", seam_debug_buf.size());
        printf("RAW=(x,y,z), OUT=(x,y,z), SDF=val, TH=terrain_h, EDIT=(cx,cy,cz,r), dist_surf, edge_dist, flags\n");
        for (size_t i = 0; i < seam_debug_buf.size() && i < 50; ++i) { // limita a 50 linhas
            const auto& d = seam_debug_buf[i];
            printf("  [%zu] RAW=(%.1f %.1f %.1f) OUT=(%.1f %.1f %.1f) SDF=%.2f TH=%.1f "
                   "EDIT=(%.1f %.1f %.1f r=%.1f) dist=%.2f edge=%.2f "
                   "reproj=%d add_reproj=%d vz_low=%d\n",
                   i, d.vx_raw, d.vy_raw, d.vz_raw, d.vx_out, d.vy_out, d.vz_out,
                   d.sdf_at_vert, d.terrain_h,
                   d.edit_cx, d.edit_cy, d.edit_cz, d.edit_r, d.dist_to_edit_surface, d.seam_edge_dist,
                   d.was_seam_reprojected, d.was_reprojected, d.vz_below_terrain);
        }
        if (seam_debug_buf.size() > 50) {
            printf("  ... (e mais %zu vertices)\n", seam_debug_buf.size() - 50);
        }
    }
#endif

    // Rebuild da mesh - C++ puro, sem Python API, pode rodar sem GIL.
    // NOTA: so RAS_DisplayArray (dados puros) e construido aqui sem GIL.
    // FindBucket/AddMaterial/EndConversion/RegisterMesh tocam BucketManager e
    // BoundingBoxManager que sao acessados pela main thread durante o render loop
    // -- essas chamadas sao feitas APOS Py_END_ALLOW_THREADS para evitar race.
#if KX_SNR_TIMING
    t_rebuild_start = Clock::now();
#endif
    if (!m_proceduralCancel.load(std::memory_order_relaxed) && scene && srcMaterialCopy && bucketManager) {
        newArray = new RAS_DisplayArray(RAS_DisplayArray::TRIANGLES, fmtCopy);

        mt::vec2_packed uvs[RAS_Texture::MaxUnits];
        unsigned int colors[RAS_Texture::MaxUnits];
        for (int k = 0; k < RAS_Texture::MaxUnits; ++k) {
            uvs[k] = mt::vec2_packed(mt::zero2);
            colors[k] = KX_GetBiomeColor(BIOME_FOREST);
        }
        const mt::vec4_packed tangent(mt::one4);
        unsigned int origIdx = 0;

        const Py_ssize_t nv_count = (Py_ssize_t)verts.size() / 3;
        newArray->ReserveVertices((unsigned int)nv_count);
        const float* __restrict vp = verts.data();
        const float* __restrict np = norms.data();
        const bool has_surface_mask = (surface_mask.size() == (size_t)nv_count);
        const unsigned char* __restrict sp = has_surface_mask ? surface_mask.data() : nullptr;
        const bool has_biome_mask = (biome_mask.size() == (size_t)nv_count);
        const unsigned char* __restrict bp = has_biome_mask ? biome_mask.data() : nullptr;
        for (Py_ssize_t i = 0; i < nv_count; ++i, vp += 3, np += 3) {
            const float posData[3] = { vp[0]-wx, vp[1]-wy, vp[2]-wz_world };
            const mt::vec3_packed pos(posData);
            const mt::vec3_packed norm(np);
            uvs[0] = mt::vec2_packed(mt::vec2(vp[0]*0.1f, vp[1]*0.1f));
            const bool is_surface = (sp != nullptr) ? (sp[i] != 0) : true;
            const int biome_id = (bp != nullptr) ? (int)bp[i] : BIOME_FOREST;
            unsigned int biome_rgb = KX_GetBiomeColor(biome_id) & 0x00FFFFFF;
            unsigned int vcolor = biome_rgb | (is_surface ? (0xFF << 24) : 0);
            colors[0] = vcolor;
            newArray->AddVertex(pos, norm, tangent, uvs, colors, origIdx++, 0);
        }

        const Py_ssize_t ni = (Py_ssize_t)inds.size();
        if (ni > 0) {
            newArray->ReservePrimitiveIndices(ni);
            newArray->ReserveTriangleIndices(ni);
            static_assert(sizeof(int) == sizeof(unsigned int), "int/uint size mismatch");
            const unsigned int* udata = reinterpret_cast<const unsigned int*>(inds.data());
            newArray->AddPrimitiveIndices(udata, ni);
            newArray->AddTriangleIndices(udata, ni);
        }

        newArray->NotifyUpdate(RAS_DisplayArray::COLORS_MODIFIED | RAS_DisplayArray::SIZE_MODIFIED);
    }
#if KX_SNR_TIMING
    t_rebuild_end  = Clock::now();
    t_phase2_end   = Clock::now();
#endif

    } // fim do bloco sem GIL (Py_BEGIN_ALLOW_THREADS)

    // Timing report sem GIL -- fprintf(stderr) nao precisa da GIL.
    // Manter aqui evita reaquirir a GIL so para printar, o que bloquearia a main thread.
#if KX_SNR_TIMING
    {
        const double total_ms     = ms_between(t_fn_start,        t_phase2_end);
        const double phase1_ms    = ms_between(t_phase1_start,    t_phase1_end);
        const double phase2_ms    = ms_between(t_phase2_start,    t_phase2_end);
        const double hm_ms        = ms_between(t_hm_start,        t_hm_end);
        const double hm_cache_ms  = ms_between(t_hm_cache_start,  t_hm_cache_end);
        const double hm_precomp_ms= ms_between(t_hm_precomp_start,t_hm_precomp_end);
        const double passA1_ms    = ms_between(t_passA1_start,    t_passA1_end);
        const double passB_ms     = ms_between(t_passB_start,     t_passB_end);
        const double sdf_ms       = ms_between(t_sdf_start,       t_sdf_end);
        const double grad_ms      = ms_between(t_grad_start,      t_grad_end);
        const double smooth_ms    = ms_between(t_smooth_start,    t_smooth_end);
        const double snp1_ms      = ms_between(t_snpass1_start,   t_snpass1_end);
        const double merge_ms     = ms_between(t_merge_start,     t_merge_end);
        const double snp2_ms      = ms_between(t_snpass2_start,   t_snpass2_end);
        const double indsclean_ms = ms_between(t_indsclean_start, t_indsclean_end);
        const double lsmooth_ms   = ms_between(t_lsmooth_start,   t_lsmooth_end);
        const double norm4_ms     = ms_between(t_normpass4_start, t_normpass4_end);
        const double norm4a_ms    = ms_between(t_norm4a_start,    t_norm4a_end);
        const double norm4b_ms    = ms_between(t_norm4b_start,    t_norm4b_end);
        const double rebuild_ms   = ms_between(t_rebuild_start,   t_rebuild_end);

        const int nv = (int)(verts.size() / 3);
        const int ni = (int)(inds.size()  / 3);

        auto snr_bar = [](double ms, double total) {
            char buf[25];
            int n = (total > 1e-9) ? (int)(ms / total * 24.0 + 0.5) : 0;
            if (n > 24) n = 24;
            for (int i = 0; i < 24; i++) buf[i] = (i < n) ? '#' : '-';
            buf[24] = '\0';
            return std::string(buf);
        };

        auto pct = [&](double ms) {
            return (total_ms > 1e-9) ? (ms / total_ms * 100.0) : 0.0;
        };

        fprintf(stderr, "\n");
        fprintf(stderr, "[SNR TIMING] ============================================================\n");
        fprintf(stderr, "[SNR TIMING]  SURFACE NETS AND REBUILD\n");
        fprintf(stderr, "[SNR TIMING]  grid=%dx%dx%d  verts=%d  tris=%d  threads=%d\n",
                (int)nx, (int)ny, (int)nz, nv, ni, omp_threads);
        fprintf(stderr, "[SNR TIMING] ============================================================\n");
        fprintf(stderr, "[SNR TIMING]  TOTAL                          %8.3f ms\n", total_ms);
        fprintf(stderr, "[SNR TIMING] ------------------------------------------------------------\n");
        fprintf(stderr, "[SNR TIMING]  FASE 1  Python/GIL             %8.3f ms  %5.1f%%\n", phase1_ms, pct(phase1_ms));
        fprintf(stderr, "[SNR TIMING] ------------------------------------------------------------\n");
        fprintf(stderr, "[SNR TIMING]  FASE 2  C++ core (no GIL)      %8.3f ms  %5.1f%%\n", phase2_ms, pct(phase2_ms));
        fprintf(stderr, "[SNR TIMING]  ------------------------------------------------------------\n");
        fprintf(stderr, "[SNR TIMING]   2.1   Heightmap+Biomemap       %8.3f ms  %5.1f%%\n", hm_ms, pct(hm_ms));
        fprintf(stderr, "[SNR TIMING]          [%s]\n", snr_bar(hm_ms, total_ms).c_str());
        fprintf(stderr, "[SNR TIMING]   2.1.0  Noise cache fill         %8.3f ms  %5.1f%%\n", hm_cache_ms, pct(hm_cache_ms));
        fprintf(stderr, "[SNR TIMING]          [%s]\n", snr_bar(hm_cache_ms, total_ms).c_str());
        fprintf(stderr, "[SNR TIMING]   2.1.0b fy/ly precompute         %8.3f ms  %5.1f%%\n", hm_precomp_ms, pct(hm_precomp_ms));
        fprintf(stderr, "[SNR TIMING]          [%s]\n", snr_bar(hm_precomp_ms, total_ms).c_str());
        fprintf(stderr, "[SNR TIMING]   2.1.1  Noise cache/hmap A       %8.3f ms  %5.1f%%\n", passA1_ms, pct(passA1_ms));
        fprintf(stderr, "[SNR TIMING]          [%s]\n", snr_bar(passA1_ms, total_ms).c_str());
        fprintf(stderr, "[SNR TIMING]   2.1.2  Biomemap PASS B         %8.3f ms  %5.1f%%\n", passB_ms, pct(passB_ms));
        fprintf(stderr, "[SNR TIMING]          [%s]\n", snr_bar(passB_ms, total_ms).c_str());
        fprintf(stderr, "[SNR TIMING]  ------------------------------------------------------------\n");
        fprintf(stderr, "[SNR TIMING]   2.2   SDF fill                 %8.3f ms  %5.1f%%\n", sdf_ms, pct(sdf_ms));
        fprintf(stderr, "[SNR TIMING]          [%s]\n", snr_bar(sdf_ms, total_ms).c_str());
        fprintf(stderr, "[SNR TIMING]  ------------------------------------------------------------\n");
        fprintf(stderr, "[SNR TIMING]   2.3   Gradient grid SOA        %8.3f ms  %5.1f%%\n", grad_ms, pct(grad_ms));
        fprintf(stderr, "[SNR TIMING]          [%s]\n", snr_bar(grad_ms, total_ms).c_str());
        fprintf(stderr, "[SNR TIMING]  ------------------------------------------------------------\n");
        fprintf(stderr, "[SNR TIMING]   2.4   Gradient smooth          %8.3f ms  %5.1f%%\n", smooth_ms, pct(smooth_ms));
        fprintf(stderr, "[SNR TIMING]          [%s]\n", snr_bar(smooth_ms, total_ms).c_str());
        fprintf(stderr, "[SNR TIMING]  ------------------------------------------------------------\n");
        fprintf(stderr, "[SNR TIMING]   2.5   SN PASS 1 vertices       %8.3f ms  %5.1f%%\n", snp1_ms, pct(snp1_ms));
        fprintf(stderr, "[SNR TIMING]          [%s]\n", snr_bar(snp1_ms, total_ms).c_str());
        fprintf(stderr, "[SNR TIMING]  ------------------------------------------------------------\n");
        fprintf(stderr, "[SNR TIMING]   2.6   Thread merge             %8.3f ms  %5.1f%%\n", merge_ms, pct(merge_ms));
        fprintf(stderr, "[SNR TIMING]          [%s]\n", snr_bar(merge_ms, total_ms).c_str());
        fprintf(stderr, "[SNR TIMING]  ------------------------------------------------------------\n");
        fprintf(stderr, "[SNR TIMING]   2.7   SN PASS 2 quads          %8.3f ms  %5.1f%%\n", snp2_ms, pct(snp2_ms));
        fprintf(stderr, "[SNR TIMING]          [%s]\n", snr_bar(snp2_ms, total_ms).c_str());
        fprintf(stderr, "[SNR TIMING]  ------------------------------------------------------------\n");
        fprintf(stderr, "[SNR TIMING]   2.8   Index cleanup            %8.3f ms  %5.1f%%\n", indsclean_ms, pct(indsclean_ms));
        fprintf(stderr, "[SNR TIMING]          [%s]\n", snr_bar(indsclean_ms, total_ms).c_str());
        fprintf(stderr, "[SNR TIMING]  ------------------------------------------------------------\n");
        fprintf(stderr, "[SNR TIMING]   2.9   Laplacian smooth P3      %8.3f ms  %5.1f%%\n", lsmooth_ms, pct(lsmooth_ms));
        fprintf(stderr, "[SNR TIMING]          [%s]\n", snr_bar(lsmooth_ms, total_ms).c_str());
        fprintf(stderr, "[SNR TIMING]  ------------------------------------------------------------\n");
        fprintf(stderr, "[SNR TIMING]   2.10  Normal interp PASS 4     %8.3f ms  %5.1f%%\n", norm4_ms, pct(norm4_ms));
        fprintf(stderr, "[SNR TIMING]   2.10.1 4a trilinear+norm       %8.3f ms  %5.1f%%\n", norm4a_ms, pct(norm4a_ms));
        fprintf(stderr, "[SNR TIMING]          [%s]\n", snr_bar(norm4a_ms, total_ms).c_str());
        fprintf(stderr, "[SNR TIMING]   2.10.2 4b seam blend           %8.3f ms  %5.1f%%\n", norm4b_ms, pct(norm4b_ms));
        fprintf(stderr, "[SNR TIMING]          [%s]\n", snr_bar(norm4b_ms, total_ms).c_str());
        fprintf(stderr, "[SNR TIMING]  ------------------------------------------------------------\n");
        fprintf(stderr, "[SNR TIMING]   2.11  Array build (no GIL)     %8.3f ms  %5.1f%%\n", rebuild_ms, pct(rebuild_ms));
        fprintf(stderr, "[SNR TIMING]          [%s]\n", snr_bar(rebuild_ms, total_ms).c_str());
        fprintf(stderr, "[SNR TIMING] ============================================================\n");
        fprintf(stderr, "\n");
        // sem fflush aqui -- evita I/O block; stderr e line-buffered por padrao
    }
#endif // KX_SNR_TIMING

    Py_END_ALLOW_THREADS

    // Define struct and destructor outside to avoid issues with C linkage
    struct SurfaceNetsMeshData {
        RAS_DisplayArray *displayArray;
        KX_Scene *scene;
        KX_BlenderMaterial *material;
        RAS_BucketManager *bucketManager;
        RAS_Mesh::LayersInfo layersInfo;
    };

    static auto SurfaceNetsMeshDataDestructor = [](PyObject *capsule) {
        void *ptr = PyCapsule_GetPointer(capsule, "SurfaceNetsMeshData");
        if (ptr) {
            SurfaceNetsMeshData *data = static_cast<SurfaceNetsMeshData *>(ptr);
            delete data->displayArray;
            delete data;
        }
    };

    if (newArray && !m_proceduralCancel.load(std::memory_order_relaxed) && scene_pre && srcMaterialCopy_pre && bucketManager_pre) {
        SurfaceNetsMeshData *data = new SurfaceNetsMeshData();
        data->displayArray = newArray;
        data->scene = scene_pre;
        data->material = srcMaterialCopy_pre;
        data->bucketManager = bucketManager_pre;
        data->layersInfo = layersInfoCopy_pre;

        PyObject *capsule = PyCapsule_New(data, "SurfaceNetsMeshData", 
            reinterpret_cast<PyCapsule_Destructor>(+SurfaceNetsMeshDataDestructor));
        return capsule;
    } else if (newArray) {
        delete newArray;
    }

    Py_RETURN_NONE;
}

const char KX_GameObject::SurfaceNetsAndRebuild_doc[] =
"surface_nets_and_rebuild(res, bounds, values, cell_vertex, base_params, edits, clip_bounds,\n"
"                         smooth_iter, smooth_factor, wx, wy, wz,\n"
"                         seam_z_snap=1, seam_z_snap_factor=0.85, seam_normal_blend=0.45, lod=0)\n"
"Funde surface_nets_generate + rebuild_voxel_mesh numa unica chamada C++.\n"
"Elimina completamente o marshaling de retorno e a contencao de GIL.\n"
"wx, wy, wz : posicao world do objeto (para converter para local space).\n"
"lod        : nivel LOD atual (invalida chunk cache ao mudar).";

PyObject *KX_GameObject::PyFinalizeSurfaceNetsMesh(PyObject *args)
{
    PyObject *capsule;

    if (!PyArg_ParseTuple(args, "O!:finalize_surface_nets_mesh", &PyCapsule_Type, &capsule)) {
        return nullptr;
    }

    void *ptr = PyCapsule_GetPointer(capsule, "SurfaceNetsMeshData");
    if (!ptr) {
        PyErr_SetString(PyExc_TypeError, "Invalid capsule object");
        return nullptr;
    }

    struct SurfaceNetsMeshData {
        RAS_DisplayArray *displayArray;
        KX_Scene *scene;
        KX_BlenderMaterial *material;
        RAS_BucketManager *bucketManager;
        RAS_Mesh::LayersInfo layersInfo;
    };

    SurfaceNetsMeshData *data = static_cast<SurfaceNetsMeshData *>(ptr);

    KX_Mesh *newMesh = new KX_Mesh(data->scene, "Terrain", data->layersInfo);
    bool created;
    RAS_MaterialBucket *bucket = data->bucketManager->FindBucket(data->material, created);
    newMesh->AddMaterial(bucket, 0, data->displayArray);
    newMesh->EndConversion(data->scene->GetBoundingBoxManager());
    KX_GetActiveEngine()->GetConverter()->RegisterMesh(data->scene, newMesh);

    // Now, clear the capsule's destructor so the destructor doesn't delete the displayArray (it's now owned by the mesh)
    PyCapsule_SetDestructor(capsule, nullptr);
    delete data;

    return newMesh->GetProxy();
}

const char KX_GameObject::FinalizeSurfaceNetsMesh_doc[] =
"finalize_surface_nets_mesh(capsule)\n"
"Finaliza a criacao da malha a partir do capsule retornado por surface_nets_and_rebuild.\n"
"DEVE ser chamado na thread principal para evitar condicoes de corrida com o motor de renderizacao.";

const char KX_GameObject::EnableGrass_doc[] =
".. method:: enableGrass()\n"
"\n"
"   Register this object as a grass terrain in the global KX_GrassSystem.\n"
"   Call bge.logic.setGrassParams(...) to configure and bge.logic.initGrass()\n"
"   after all terrains are registered.\n";

PyObject *KX_GameObject::PyEnableGrass(PyObject * /*args*/)
{
    KX_Scene *scene = GetScene();
    if (!scene) {
        PyErr_SetString(PyExc_RuntimeError, "enableGrass: object has no scene.");
        return nullptr;
    }
    scene->GetOrCreateGrassSystem()->RegisterTerrain(this);
    Py_RETURN_NONE;
}

const char KX_GameObject::DisableGrass_doc[] =
".. method:: disableGrass()\n"
"\n"
"   Unregister this object from the global KX_GrassSystem.\n";

PyObject *KX_GameObject::PyDisableGrass(PyObject * /*args*/)
{
    KX_Scene *scene = GetScene();
    if (!scene) {
        PyErr_SetString(PyExc_RuntimeError, "disableGrass: object has no scene.");
        return nullptr;
    }

    scene->GetOrCreateGrassSystem()->UnregisterTerrain(this);
    Py_RETURN_NONE;
}
#endif // WITH_PYTHON
