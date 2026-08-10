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
 * Ketsji scene. Holds references to all scene data.
 */

/** \file gameengine/Ketsji/KX_Scene.cpp
 *  \ingroup ketsji
 */


#ifdef _MSC_VER
#  pragma warning (disable:4786)
#endif


#include <vector> 
#include <algorithm>
#include <tbb/parallel_for.h>
#include <tbb/concurrent_vector.h>
#include <tbb/blocked_range.h> 
#include <array>
#include <cstdint>
#include <functional>
#include <future>
#include <unordered_map> 
#include <unordered_set>
#include <map>
#include <thread>
#include <cmath>
#include <cstdio> 
#include <cstring>
#include "KX_Scene.h"
#include "KX_Globals.h"
#include "BLI_utildefines.h"
#include "KX_KetsjiEngine.h"
#include "KX_BlenderMaterial.h"
#include "KX_TextMaterial.h"
#include "KX_FontObject.h"
#include "RAS_IMaterial.h"
#include "EXP_ListValue.h"
#include "SCA_LogicManager.h"
#include "SCA_TimeEventManager.h"
#include "SCA_2DFilterActuator.h"
#include "SCA_PythonController.h"
#include "KX_CollisionEventManager.h"
#include "SCA_KeyboardManager.h"
#include "SCA_MouseManager.h"
#include "SCA_ActuatorEventManager.h"
#include "SCA_BasicEventManager.h"
#include "KX_Camera.h"
#include "KX_NavMeshObject.h"
#include "SCA_JoystickManager.h"
#include "KX_PyMath.h"
#include "KX_Mesh.h"
#include "SCA_IScene.h"
#include "KX_LodManager.h"
#include "KX_CullingHandler.h"
#include "KX_Camera.h"

#include "RAS_Rasterizer.h"
#include "RAS_ICanvas.h"
#include "RAS_LightManager.h"
#include "RAS_2DFilterData.h"
#include "KX_2DFilterManager.h"
#include "RAS_BoundingBoxManager.h"
#include "RAS_BucketManager.h"
#include "RAS_MeshUser.h"
#include "RAS_BatchGroup.h"
#include "RAS_Deformer.h"
#include "KX_GrassSystem.h"

#include "EXP_FloatValue.h"
#include "SCA_IController.h"
#include "SCA_IActuator.h"
#include "SG_Node.h"
#include "SG_Controller.h"
#include "SG_Node.h"
#include "DNA_group_types.h"
#include "DNA_scene_types.h"
#include "DNA_property_types.h"

#include "KX_NodeRelationships.h"

#include "KX_NetworkMessageScene.h"
#include "PHY_IPhysicsEnvironment.h"
#include "PHY_IGraphicController.h"
#include "PHY_IPhysicsController.h"
#ifdef WITH_BULLET
#  include "CcdPhysicsEnvironment.h"
#endif
#include "BL_Converter.h"
#include "BL_ArmatureObject.h"
#include "KX_MotionState.h"
#include "KX_ObstacleSimulation.h"

#ifdef WITH_PYTHON
#  include "EXP_PythonCallBack.h"
#  include "KX_PythonComponent.h"
#endif

#include "KX_LightObject.h"

#include "BLI_task.h"

#include "CM_Message.h"
#include "CM_List.h"

#include "MDEI_Renderer.h"

static void *KX_SceneReplicationFunc(SG_Node *node, void *gameobj, void *scene)
{
	KX_GameObject *replica = ((KX_Scene *)scene)->AddNodeReplicaObject(node, (KX_GameObject *)gameobj);

	if (replica) {
		replica->Release();
	}

	return (void *)replica;
}

static void *KX_SceneDestructionFunc(SG_Node *node, void *gameobj, void *scene)
{
	((KX_Scene *)scene)->RemoveNodeDestructObject((KX_GameObject *)gameobj);

	return nullptr;
}

bool KX_Scene::KX_ScenegraphUpdateFunc(SG_Node *node, void *gameobj, void *scene)
{
	return node->Schedule(((KX_Scene *)scene)->m_sghead);
}

bool KX_Scene::KX_ScenegraphRescheduleFunc(SG_Node *node, void *gameobj, void *scene)
{
	return node->Reschedule(((KX_Scene *)scene)->m_sghead);
}

SG_Callbacks KX_Scene::m_callbacks = SG_Callbacks(
	KX_SceneReplicationFunc,
	KX_SceneDestructionFunc,
	KX_GameObject::UpdateTransformFunc,
	KX_Scene::KX_ScenegraphUpdateFunc,
	KX_Scene::KX_ScenegraphRescheduleFunc);

KX_Scene::KX_Scene(SCA_IInputDevice *inputDevice,
                   const std::string& sceneName,
                   Scene *scene,
                   RAS_ICanvas *canvas,
                   KX_NetworkMessageManager *messageManager) :
	m_keyboardmgr(nullptr),
	m_mousemgr(nullptr),
	m_physicsEnvironment(0),
	m_sceneName(sceneName),
	m_activeCamera(nullptr),
	m_overrideCullingCamera(nullptr),
	m_ueberExecutionPriority(0),
	m_suspend(false),
	m_suspendedDelta(0.0),
	m_activityCulling(false),
	m_activityCullingIndex(0),
	m_activityCullingMaxCellDist(8),
	m_activityCullingMaxCellDistSq(64.0f),
	m_dbvtCulling(true),
	m_dbvtOcclusionRes(0),
	m_blenderScene(scene),
	m_previousAnimTime(0.0f),
	m_isActivedHysteresis(false),
	m_lodHysteresisValue(0),
	m_mdeiRenderer(nullptr)
{

	m_objectlist = new EXP_ListValue<KX_GameObject>();
	m_parentlist = new EXP_ListValue<KX_GameObject>();
	m_lightlist = new EXP_ListValue<KX_LightObject>();
	m_inactivelist = new EXP_ListValue<KX_GameObject>();
	m_cameralist = new EXP_ListValue<KX_Camera>();
	m_fontlist = new EXP_ListValue<KX_FontObject>();
	m_renderlist = new EXP_ListValue<KX_GameObject>();

	bool useFxaa = (scene->gm.aasamples == 1);
	m_filterManager = new KX_2DFilterManager(useFxaa);
	m_logicmgr = new SCA_LogicManager();

	m_timemgr = new SCA_TimeEventManager(m_logicmgr);
	m_keyboardmgr = new SCA_KeyboardManager(m_logicmgr, inputDevice);
	m_mousemgr = new SCA_MouseManager(m_logicmgr, inputDevice);

	SCA_ActuatorEventManager *actmgr = new SCA_ActuatorEventManager(m_logicmgr);
	SCA_BasicEventManager *basicmgr = new SCA_BasicEventManager(m_logicmgr);

	m_logicmgr->RegisterEventManager(actmgr);
	m_logicmgr->RegisterEventManager(m_keyboardmgr);
	m_logicmgr->RegisterEventManager(m_mousemgr);
	m_logicmgr->RegisterEventManager(m_timemgr);
	m_logicmgr->RegisterEventManager(basicmgr);

	SCA_JoystickManager *joymgr = new SCA_JoystickManager(m_logicmgr);
	m_logicmgr->RegisterEventManager(joymgr);

	m_networkScene = new KX_NetworkMessageScene(messageManager);

	m_rendererManager = new KX_TextureRendererManager(this);
	m_bucketmanager = new RAS_BucketManager(KX_TextMaterial::GetSingleton());
	m_boundingBoxManager = new RAS_BoundingBoxManager();
	m_mdeiRenderer = new MDEI_Renderer(this);

	m_animationPool = BLI_task_pool_create(KX_GetActiveEngine()->GetTaskScheduler(), &m_animationPoolData);

#ifdef WITH_PYTHON
	m_attrDict = nullptr;
	m_removeCallbacks = nullptr;

	for (unsigned short i = 0; i < MAX_DRAW_CALLBACK; ++i) {
		m_drawCallbacks[i] = nullptr;
	}
#endif
}

KX_Scene::~KX_Scene()
{
	/* The release of debug properties used to be in SCA_IScene::~SCA_IScene
	 * It's still there but we remove all properties here otherwise some
	 * reference might be hanging and causing late release of objects
	 */
	RemoveAllDebugProperties();

#ifdef WITH_PYTHON
	// Dispose ALL components (active and inactive) before any proxy invalidation or object deletion.
	// This ensures dispose() is called with valid references regardless of how the game ends.
	auto disposeObjectComponents = [](KX_GameObject *obj) {
		if (EXP_ListValue<KX_PythonComponent> *comps = obj->GetComponents()) {
			for (KX_PythonComponent *comp : *comps) {
				comp->Dispose();
			}
		}
	};

	// Shutdown do worker de grama ANTES de destruir qualquer objeto da cena.
	// O worker pode estar segurando ponteiros para dados de KX_GameObject/RAS_Mesh —
	// parar o worker aqui garante que nenhum acesso acontece depois desta linha.
	if (m_grassSystem) {
		m_grassSystem->Shutdown();
	}

	// Active objects (parentlist is root of active hierarchy)
	if (m_parentlist) {
		for (int i = 0, n = m_parentlist->GetCount(); i < n; ++i) {
			disposeObjectComponents(static_cast<KX_GameObject *>(m_parentlist->GetValue(i)));
		}
	}
	if (m_objectlist) {
		for (int i = 0, n = m_objectlist->GetCount(); i < n; ++i) {
			disposeObjectComponents(static_cast<KX_GameObject *>(m_objectlist->GetValue(i)));
		}
	}
	// Inactive objects -- these never go through NewRemoveObject during scene teardown
	if (m_inactivelist) {
		for (int i = 0, n = m_inactivelist->GetCount(); i < n; ++i) {
			disposeObjectComponents(static_cast<KX_GameObject *>(m_inactivelist->GetValue(i)));
		}
	}
#endif

	while (GetRootParentList()->GetCount() > 0) {
		KX_GameObject *parentobj = GetRootParentList()->GetValue(0);
		this->RemoveObject(parentobj);
	}

	if (m_obstacleSimulation) {
		delete m_obstacleSimulation;
	}

	if (m_grassSystem) {
		delete m_grassSystem;
		m_grassSystem = nullptr;
	}

	if (m_animationPool) {
		BLI_task_pool_free(m_animationPool);
	}

	if (m_objectlist) {
		m_objectlist->Release();
	}

	if (m_parentlist) {
		m_parentlist->Release();
	}

	if (m_inactivelist) {
		m_inactivelist->Release();
	}

	if (m_lightlist) {
		m_lightlist->Release();
	}

	if (m_cameralist) {
		m_cameralist->Release();
	}

	if (m_fontlist) {
		m_fontlist->Release();
	}

	if (m_renderlist) {
		m_renderlist->Release();
	}

	if (m_filterManager) {
		delete m_filterManager;
	}

	if (m_logicmgr) {
		delete m_logicmgr;
	}

	if (m_physicsEnvironment) {
		delete m_physicsEnvironment;
	}

	if (m_networkScene) {
		delete m_networkScene;
	}

	if (m_rendererManager) {
		delete m_rendererManager;
	}

	if (m_bucketmanager) {
		delete m_bucketmanager;
	}

	if (m_boundingBoxManager) {
		delete m_boundingBoxManager;
	}

	if (m_mdeiRenderer) {
		delete m_mdeiRenderer;
	}

	if (m_worldinfo) {
		delete m_worldinfo;
	}

#ifdef WITH_PYTHON
	if (m_attrDict) {
		PyDict_Clear(m_attrDict);
		Py_CLEAR(m_attrDict);
	}

	// These may be nullptr but the macro checks.
	Py_CLEAR(m_removeCallbacks);
	for (unsigned short i = 0; i < MAX_DRAW_CALLBACK; ++i) {
		Py_CLEAR(m_drawCallbacks[i]);
	}
#endif
}

std::string KX_Scene::GetName()
{
	return m_sceneName;
}

void KX_Scene::SetName(const std::string& name)
{
	m_sceneName = name;
}

RAS_BucketManager *KX_Scene::GetBucketManager() const
{
	return m_bucketmanager;
}

KX_TextureRendererManager *KX_Scene::GetTextureRendererManager() const
{
	return m_rendererManager;
}

RAS_BoundingBoxManager *KX_Scene::GetBoundingBoxManager() const
{
	return m_boundingBoxManager;
}

MDEI_Renderer *KX_Scene::GetMdeiRenderer() const
{
	return m_mdeiRenderer;
}

EXP_ListValue<KX_GameObject> *KX_Scene::GetObjectList() const
{
	return m_objectlist;
}

EXP_ListValue<KX_GameObject> *KX_Scene::GetRootParentList() const
{
	return m_parentlist;
}

EXP_ListValue<KX_GameObject> *KX_Scene::GetInactiveList() const
{
	return m_inactivelist;
}

EXP_ListValue<KX_LightObject> *KX_Scene::GetLightList() const
{
	return m_lightlist;
}

EXP_ListValue<KX_Camera> *KX_Scene::GetCameraList() const
{
	return m_cameralist;
}

EXP_ListValue<KX_FontObject> *KX_Scene::GetFontList() const
{
	return m_fontlist;
}

EXP_ListValue<KX_GameObject> *KX_Scene::GetRenderList() const
{
	return m_renderlist;
}

SCA_LogicManager *KX_Scene::GetLogicManager() const
{
	return m_logicmgr;
}

SCA_TimeEventManager *KX_Scene::GetTimeEventManager() const
{
	return m_timemgr;
}

KX_PythonComponentManager& KX_Scene::GetPythonComponentManager()
{
	return m_componentManager;
}

void KX_Scene::SetFramingType(const RAS_FrameSettings& frameSettings)
{
	m_frameSettings = frameSettings;
}

const RAS_FrameSettings& KX_Scene::GetFramingType() const
{
	return m_frameSettings;
}

void KX_Scene::SetWorldInfo(KX_WorldInfo *worldinfo)
{
	m_worldinfo = worldinfo;
}

KX_WorldInfo *KX_Scene::GetWorldInfo() const
{
	return m_worldinfo;
}

void KX_Scene::Suspend()
{
    m_suspend = true;

    //SCA_LogicManager* logicMgr = GetLogicManager();
    //if (logicMgr) {
        //SCA_EventManager* evmgr = logicMgr->FindEventManager(SCA_EventManager::KEYBOARD_EVENTMGR);
        //if (evmgr) {
            //SCA_KeyboardManager* kbdMgr = static_cast<SCA_KeyboardManager*>(evmgr);
            //    kbdMgr->ResetInputs();
            //}
        //}

}


void KX_Scene::Resume()
{
    
    m_suspend = false;

    SCA_LogicManager* logicMgr = GetLogicManager();
    if (logicMgr) {
        SCA_EventManager* evmgr = logicMgr->FindEventManager(SCA_EventManager::KEYBOARD_EVENTMGR);
        if (evmgr) {
            SCA_KeyboardManager* kbdMgr = static_cast<SCA_KeyboardManager*>(evmgr);
			    kbdMgr->ResetInputs();
            }
        }
}



void KX_Scene::SetActivityCulling(bool b)
{
	m_activityCulling = b;
}

bool KX_Scene::IsSuspended() const
{
	return m_suspend;
}

void KX_Scene::SetDbvtCulling(bool b)
{
	m_dbvtCulling = b;
}

bool KX_Scene::GetDbvtCulling() const
{
	return m_dbvtCulling;
}

void KX_Scene::SetDbvtOcclusionRes(int i)
{
	m_dbvtOcclusionRes = i;
}

int KX_Scene::GetDbvtOcclusionRes() const
{
	return m_dbvtOcclusionRes;
}

void KX_Scene::AddObjectDebugProperties(KX_GameObject *gameobj)
{
	Object *blenderobject = gameobj->GetBlenderObject();
	if (!blenderobject) {
		return;
	}

	for (bProperty *prop = (bProperty *)blenderobject->prop.first; prop; prop = prop->next) {
		if (prop->flag & PROP_DEBUG) {
			AddDebugProperty(gameobj, prop->name);
		}
	}

	if (blenderobject->scaflag & OB_DEBUGSTATE) {
		AddDebugProperty(gameobj, "__state__");
	}
}

void KX_Scene::RemoveNodeDestructObject(KX_GameObject *gameobj)
{
	if (NewRemoveObject(gameobj)) {
		/* Object is not yet deleted because a reference is hanging somewhere.
		 * This should not happen anymore since we use proxy object for Python. */
		CM_Error("zombie object! name=" << gameobj->GetName());
		BLI_assert(false);
	}
}

KX_GameObject *KX_Scene::AddNodeReplicaObject(SG_Node *node, KX_GameObject *gameobj)
{
	/* For group duplication, limit the duplication of the hierarchy to the
	 * objects that are part of the group. */
	if (!IsObjectInGroup(gameobj)) {
		return nullptr;
	}

	KX_GameObject *newobj = static_cast<KX_GameObject *>(gameobj->GetReplica());
	m_map_gameobject_to_replica[gameobj] = newobj;

	// Also register 'timers' (time properties) of the replica.
	for (unsigned short i = 0, numprops = newobj->GetPropertyCount(); i < numprops; ++i) {
		EXP_Value *prop = newobj->GetProperty(i);

		if (prop->GetProperty("timer")) {
			m_timemgr->AddTimeProperty(prop);
		}
	}

	if (node) {
		newobj->SetNode(node);
	}
	else {
		SG_Node *rootnode = new SG_Node(newobj, this, KX_Scene::m_callbacks);

		// This fixes part of the scaling-added object bug.
		SG_Node *orgnode = gameobj->GetNode();
		rootnode->SetLocalScale(orgnode->GetLocalScale());

		rootnode->SetLocalPosition(orgnode->GetLocalPosition());
		rootnode->SetLocalOrientation(orgnode->GetLocalOrientation());

		// Define the relationship between this node and it's parent.
		KX_NormalParentRelation *parent_relation = new KX_NormalParentRelation();
		rootnode->SetParentRelation(parent_relation);

		newobj->SetNode(rootnode);
	}

	SG_Node *replicanode = newobj->GetNode();

	// Add the object in the obstacle simulation if needed.
	if (m_obstacleSimulation && gameobj->GetBlenderObject()->gameflag & OB_HASOBSTACLE) {
		m_obstacleSimulation->AddObstacleForObj(newobj);
	}
	// Reconstruct nav mesh.
	if (gameobj->GetGameObjectType() == SCA_IObject::OBJ_NAVMESH) {
		static_cast<KX_NavMeshObject *>(newobj)->BuildNavMesh();
	}

	// Register object for component update.
	if (gameobj->GetComponents()) {
		m_componentManager.RegisterObject(newobj);
	}

	replicanode->SetClientObject(newobj);

	// This is the list of object that are send to the graphics pipeline.
	m_objectlist->Add(CM_AddRef(newobj));
	/* Register in the batch-update registry so bge.logic.batchUpdate can find it */
	newobj->SetObjectId(m_objectRegistry.Register(newobj));

	const auto& cullingInfo = newobj->GetActivityCullingInfo();
	if (cullingInfo.m_flags != KX_GameObject::ActivityCullingInfo::ACTIVITY_NONE) {
		m_activityCullingList.push_back(newobj);
	}

	if (gameobj->GetVisible()) {
		m_renderlist->Add(CM_AddRef(newobj));
	}

	switch (newobj->GetGameObjectType()) {
		case SCA_IObject::OBJ_LIGHT:
		{
			m_lightlist->Add(CM_AddRef(static_cast<KX_LightObject *>(newobj)));
			// Mark light UBO as dirty when adding a new light
			RAS_LightManager::GetInstance()->MarkDirty();
			break;
		}
		case SCA_IObject::OBJ_TEXT:
		{
			m_fontlist->Add(CM_AddRef(static_cast<KX_FontObject *>(newobj)));
			break;
		}
		case SCA_IObject::OBJ_CAMERA:
		{
			m_cameralist->Add(CM_AddRef(static_cast<KX_Camera *>(newobj)));
			break;
		}
		case SCA_IObject::OBJ_ARMATURE:
		{
			AddAnimatedObject(newobj);
			break;
		}
	}

	// Logic cannot be replicated, until the whole hierarchy is replicated.
	m_logicHierarchicalGameObjects.push_back(newobj);

	// Replicate graphic controller.
	if (gameobj->GetGraphicController()) {
		PHY_IMotionState *motionstate = new KX_MotionState(newobj->GetNode());
		PHY_IGraphicController *newctrl = gameobj->GetGraphicController()->GetReplica(motionstate);
		newctrl->SetNewClientInfo(&newobj->GetClientInfo());
		newobj->SetGraphicController(newctrl);
	}

	// Replicate physics controller.
	if (gameobj->GetPhysicsController()) {
		PHY_IMotionState *motionstate = new KX_MotionState(newobj->GetNode());
		PHY_IPhysicsController *newctrl = gameobj->GetPhysicsController()->GetReplica();

		KX_GameObject *parent = newobj->GetParent();
		PHY_IPhysicsController *parentctrl = (parent) ? parent->GetPhysicsController() : nullptr;

		newctrl->SetNewClientInfo(&newobj->GetClientInfo());
		newobj->SetPhysicsController(newctrl);
		newctrl->PostProcessReplica(motionstate, parentctrl);

		// Child objects must be static.
		if (parent) {
			newctrl->SuspendDynamics();
		}
	}

	return newobj;
}

/*
 * Before calling this method KX_Scene::ReplicateLogic(), make sure to
 * have called 'GameObject::ReParentLogic' for each object this
 * hierarchy that's because first ALL bricks must exist in the new
 * replica of the hierarchy in order to make cross-links work properly.
 *
 * It is VERY important that the order of sensors and actuators in
 * the replicated object is preserved: it is used to reconnect the logic.
 * This method is more robust then using the bricks name in case of complex
 * group replication. The replication of logic bricks is done in
 * SCA_IObject::ReParentLogic(), make sure it preserves the order of the bricks.
 */
void KX_Scene::ReplicateLogic(KX_GameObject *newobj)
{
	// Add properties to debug list, for added objects and DupliGroups.
	if (KX_GetActiveEngine()->GetFlag(KX_KetsjiEngine::AUTO_ADD_DEBUG_PROPERTIES)) {
		AddObjectDebugProperties(newobj);
	}
	// Also relink the controller to sensors/actuators.
	const SCA_ControllerList controllers = newobj->GetControllers();

	for (SCA_IController *cont : controllers) {
		cont->SetUeberExecutePriority(m_ueberExecutionPriority);
		const SCA_SensorList linkedsensors = cont->GetLinkedSensors();
		const SCA_ActuatorList linkedactuators = cont->GetLinkedActuators();

		/* Disconnect the sensors and actuators
		 * do it directly on the list at this controller is not connected to anything at this stage. */
		cont->GetLinkedSensors().clear();
		cont->GetLinkedActuators().clear();

		// Now relink each sensor.
		for (SCA_ISensor *oldsensor : linkedsensors) {
			SCA_IObject *oldsensorobj = oldsensor->GetParent();
			// The original owner of the sensor has been replicated?
			SCA_IObject *newsensorobj = m_map_gameobject_to_replica[oldsensorobj];

			if (!newsensorobj) {
				// No, then the sensor points outside the hierarchy, keep it the same.
				if (m_objectlist->SearchValue(static_cast<KX_GameObject *>(oldsensorobj))) {
					// Only replicate links that points to active objects.
					m_logicmgr->RegisterToSensor(cont, oldsensor);
				}
			}
			else {
				// Yes, then the new sensor has the same position.
				SCA_SensorList& sensorlist = oldsensorobj->GetSensors();
				SCA_SensorList::iterator sit;
				SCA_ISensor *newsensor = nullptr;
				int sensorpos;

				for (sensorpos = 0, sit = sensorlist.begin(); sit != sensorlist.end(); sit++, sensorpos++) {
					if ((*sit) == oldsensor) {
						newsensor = newsensorobj->GetSensors().at(sensorpos);
						break;
					}
				}

				BLI_assert(newsensor != nullptr);
				m_logicmgr->RegisterToSensor(cont, newsensor);
			}
		}

		// Now relink each actuator.
		for (SCA_IActuator *oldactuator : linkedactuators) {
			SCA_IObject *oldactuatorobj = oldactuator->GetParent();
			SCA_IObject *newactuatorobj = m_map_gameobject_to_replica[oldactuatorobj];

			if (!newactuatorobj) {
				// No, then the sensor points outside the hierarchy, keep it the same.
				if (m_objectlist->SearchValue(static_cast<KX_GameObject *>(oldactuatorobj))) {
					// Only replicate links that points to active objects
					m_logicmgr->RegisterToActuator(cont, oldactuator);
				}
			}
			else {
				// Yes, then the new sensor has the same position
				SCA_ActuatorList& actuatorlist = oldactuatorobj->GetActuators();
				SCA_ActuatorList::iterator ait;
				SCA_IActuator *newactuator = nullptr;
				int actuatorpos;

				for (actuatorpos = 0, ait = actuatorlist.begin(); ait != actuatorlist.end(); ait++, actuatorpos++) {
					if ((*ait) == oldactuator) {
						newactuator = newactuatorobj->GetActuators().at(actuatorpos);
						break;
					}
				}
				BLI_assert(newactuator != nullptr);
				m_logicmgr->RegisterToActuator(cont, newactuator);
				newactuator->SetUeberExecutePriority(m_ueberExecutionPriority);
			}
		}
	}
	// Ready to set initial state.
	newobj->ResetState();
}

void KX_Scene::DupliGroupRecurse(KX_GameObject *groupobj, int level)
{
	Object *blgroupobj = groupobj->GetBlenderObject();
	std::vector<KX_GameObject *> duplilist;

	if (!groupobj->GetNode() || !groupobj->IsDupliGroup() || level > MAX_DUPLI_RECUR) {
		return;
	}

	// We will add one group at a time.
	m_logicHierarchicalGameObjects.clear();
	m_map_gameobject_to_replica.clear();
	m_ueberExecutionPriority++;

	/* For groups will do something special:
	 * we will force the creation of objects to those in the group only
	 * Again, this is match what Blender is doing (it doesn't care of parent relationship)
	 */
	m_groupGameObjects.clear();

	Group *group = blgroupobj->dup_group;
	// Use cached membership if available to avoid recomputation on mass spawns.
	auto cacheIt = m_groupCache.find((void*)group);
	if (cacheIt == m_groupCache.end()) {
		std::set<KX_GameObject *> members;
		for (GroupObject *go = (GroupObject *)group->gobject.first; go; go = (GroupObject *)go->next) {
			Object *blenderobj = go->ob;
			if (blgroupobj == blenderobj) {
				continue;
			}
			KX_GameObject *gameobj = (KX_GameObject *)m_logicmgr->FindGameObjByBlendObj(blenderobj);
			if (gameobj == nullptr) {
				continue;
			}
			if ((blenderobj->lay & group->layer) == 0) {
				continue;
			}
			members.insert(gameobj);
		}
		m_groupCache[(void*)group] = members;
		m_groupGameObjects = members;
	}
	else {
		m_groupGameObjects = cacheIt->second;
	}

	for (KX_GameObject *gameobj : m_groupGameObjects) {
		KX_GameObject *parent = gameobj->GetParent();
		if (parent != nullptr) {
			/* This object is not a top parent. Either it is the child of another
			 * object in the group and it will be added automatically when the parent
			 * is added. Or it is the child of an object outside the group and the group
			 * is inconsistent, skip it anyway.
			 */
			continue;
		}
		KX_GameObject *replica = AddNodeReplicaObject(nullptr, gameobj);
		// Add to 'rootparent' list (this is the list of top hierarchy objects, updated each frame).
		m_parentlist->Add(CM_AddRef(replica));

		// Recurse replication into children nodes.
		const NodeList& children = gameobj->GetNode()->GetChildren();

		replica->GetNode()->ClearSGChildren();
		for (SG_Node *orgnode : children) {
			SG_Node *childreplicanode = orgnode->GetReplica();
			if (childreplicanode) {
				replica->GetNode()->AddChild(childreplicanode);
			}
		}
		/* Don't replicate logic now: we assume that the objects in the group can have
		 * logic relationship, even outside parent relationship
		 * In order to match 3D view, the position of groupobj is used as a
		 * transformation matrix instead of the new position. This means that
		 * the group reference point is 0,0,0.
		 */

		// Get the rootnode's scale.
		const mt::vec3& newscale = groupobj->NodeGetWorldScaling();
		// Set the replica's relative scale with the rootnode's scale.
		replica->NodeSetRelativeScale(newscale);

		const mt::vec3 offset(group->dupli_ofs);
		const mt::vec3 newpos = groupobj->NodeGetWorldPosition() +
		                        newscale * (groupobj->NodeGetWorldOrientation() * (gameobj->NodeGetWorldPosition() - offset));
		replica->NodeSetLocalPosition(newpos);
		// Set the orientation after position for softbody.
		const mt::mat3 newori = groupobj->NodeGetWorldOrientation() * gameobj->NodeGetWorldOrientation();
		replica->NodeSetLocalOrientation(newori);
		// Update scenegraph for entire tree of children.
		replica->GetNode()->UpdateWorldData();
		// We can now add the graphic controller to the physic engine.
		replica->ActivateGraphicController(true);

		// Done with replica.
		replica->Release();
	}

	// Pass 1: Membership registration.
	// Objects must be part of the group before any logic processing.
	for (KX_GameObject *gameobj : m_logicHierarchicalGameObjects) {
		groupobj->AddInstanceObjects(gameobj);
		gameobj->SetDupliGroupObject(groupobj);
	}

	// Pass 2: Logic brick creation (ReParentLogic).
	// Bricks must exist on all replicas before cross-linking can occur.
	for (KX_GameObject *gameobj : m_logicHierarchicalGameObjects) {
		gameobj->ReParentLogic();
	}

	// Pass 3: Linking, Resource Setup, and ReplicateLogic.
	// We merge resource setup and logic linking as they are independent at this stage.
	for (KX_GameObject *gameobj : m_logicHierarchicalGameObjects) {
		gameobj->Relink(m_map_gameobject_to_replica);
		gameobj->AddMeshUser();
		gameobj->UpdateBounds(true);
		gameobj->SetLayer(groupobj->GetLayer());
		ReplicateLogic(gameobj);
	}

	// Pass 4: Physics Constraints and Recursion collection.
	// Physics depends on stable logic and resource setup. 
	// Recursion must wait until the parent group is fully stabilized.
	for (KX_GameObject *gameobj : m_logicHierarchicalGameObjects) {
		gameobj->ReplicateConstraints(m_physicsEnvironment, m_logicHierarchicalGameObjects);

		if (gameobj != groupobj && gameobj->IsDupliGroup()) {
			duplilist.push_back(gameobj);
		}
	}

	for (KX_GameObject *gameobj : duplilist) {
		DupliGroupRecurse(gameobj, level + 1);
	}
}

bool KX_Scene::IsObjectInGroup(KX_GameObject *gameobj) const
{
	return (m_groupGameObjects.empty() || m_groupGameObjects.find(gameobj) != m_groupGameObjects.end());
}

KX_GameObject *KX_Scene::AddReplicaObject(KX_GameObject *originalobj,
                                         KX_GameObject *referenceobj,
                                         float lifespan)
{
	m_logicHierarchicalGameObjects.clear();
	m_map_gameobject_to_replica.clear();
	m_groupGameObjects.clear();

	m_ueberExecutionPriority++;

	KX_GameObject *replica = AddNodeReplicaObject(nullptr, originalobj);

	if (lifespan > 0.0f) {
		m_tempObjectList.push_back(replica);
		replica->SetProperty("::timebomb",
		                     new EXP_FloatValue(lifespan * 0.02f));
	}

	m_parentlist->Add(CM_AddRef(replica));

	// Replicate children
	SG_Node *origNode = originalobj->GetNode();
	SG_Node *replicaNode = replica->GetNode();

	const NodeList &children = origNode->GetChildren();
	replicaNode->ClearSGChildren();
	for (SG_Node *orgnode : children) {
		if (SG_Node *child = orgnode->GetReplica()) {
			replicaNode->AddChild(child);
		}
	}

	if (referenceobj) {
		const mt::vec3 &pos = referenceobj->NodeGetWorldPosition();
		const mt::mat3 &ori = referenceobj->NodeGetWorldOrientation();
		const mt::vec3 &scale = referenceobj->GetNode()->GetRootSGParent()->GetLocalScale();

		replica->NodeSetLocalPosition(pos);
		replica->NodeSetLocalOrientation(ori);
		replica->NodeSetRelativeScale(scale);
	}

	replicaNode->UpdateWorldData();
	replica->ActivateGraphicController(true);

	std::vector<KX_GameObject *> duplilist;
	duplilist.reserve(m_logicHierarchicalGameObjects.size());

	const int layer = referenceobj ? referenceobj->GetLayer() : m_blenderScene->lay;

	for (KX_GameObject *gameobj : m_logicHierarchicalGameObjects) {
		gameobj->ReParentLogic();
		gameobj->Relink(m_map_gameobject_to_replica);
		gameobj->AddMeshUser(); /* no-op for MDEI objects (guard in KX_GameObject) */

		/* If this replica comes from an MDEI original, register it on the fast-path renderer */
		if (!gameobj->HasFastRenderFlag()) {
			for (auto& pair : m_map_gameobject_to_replica) {
				if (pair.second == gameobj) {
					KX_GameObject *original = static_cast<KX_GameObject *>(pair.first);
					if (original->HasFastRenderFlag()) {
						m_mdeiRenderer->RegisterReplica(gameobj, original);
					}
					break;
				}
			}
		}

		gameobj->UpdateBounds(true);

		gameobj->SetLayer(layer);

		ReplicateLogic(gameobj);

		if (gameobj->IsDupliGroup()) {
			duplilist.push_back(gameobj);
		}
	}

	for (KX_GameObject *gameobj : duplilist) {
		DupliGroupRecurse(gameobj, 0);
	}

	return replica;
}


void KX_Scene::RemoveObject(KX_GameObject *gameobj)
{
	// Mark light UBO as dirty if removing a light
	if (gameobj->GetGameObjectType() == SCA_IObject::OBJ_LIGHT) {
		RAS_LightManager::GetInstance()->MarkDirty();
	}
	
	// Disconnect child from parent.
	SG_Node *node = gameobj->GetNode();

	if (node) {
		node->DisconnectFromParent();

		// Recursively destruct.
		node->Destruct();
	}
}

void KX_Scene::RemoveDupliGroup(KX_GameObject *gameobj)
{
	if (gameobj->GetInstanceObjects()) {
		for (KX_GameObject *instance : gameobj->GetInstanceObjects()) {
			DelayedRemoveObject(instance);
		}
	}
}

void KX_Scene::DelayedRemoveObject(KX_GameObject *gameobj)
{
	RemoveDupliGroup(gameobj);

	m_euthanasyobjects.insert(gameobj);
}

void KX_Scene::RemoveEuthanasyObjects()
{
	/* Don't remove the objects from the euthanasy list here as the child objects of a deleted
	 * parent object are destructed directly from the sgnode in the same time the parent
	 * object is destructed. These child objects must be removed automatically from the
	 * euthanasy list to avoid double deletion in case the user ask to delete the child object
	 * explicitly. NewRemoveObject is the place to do it.
	 */
	while (!m_euthanasyobjects.empty()) {
		RemoveObject(*m_euthanasyobjects.begin());
	}
}

void KX_Scene::RemoveAllMeshes()
{
	for (KX_GameObject *gameobj : m_objectlist) {
		if (gameobj) {
			gameobj->RemoveMeshes();
		}
	}
	for (KX_GameObject *gameobj : m_inactivelist) {
		if (gameobj) {
			gameobj->RemoveMeshes();
		}
	}
	for (KX_GameObject *gameobj : m_renderlist) {
		if (gameobj) {
			gameobj->RemoveMeshes();
		}
	}
}

bool KX_Scene::NewRemoveObject(KX_GameObject *gameobj)
{
	// Remove property from debug list.
	RemoveObjectDebugProperties(gameobj);

#ifdef WITH_PYTHON
	// Dispose component BEFORE invalidating the proxy, so that Python
	// code in dispose() still has valid references to self.object and its attributes.
	// Guard against double-dispose is in KX_PythonComponent::Dispose() via m_disposeFinished.
	if (EXP_ListValue<KX_PythonComponent> *components = gameobj->GetComponents()) {
		for (KX_PythonComponent *comp : *components) {
			comp->Dispose();
		}
	}
#endif

	/* Invalidate the python reference, since the object may exist in script lists
	 * its possible that it wont be automatically invalidated, so do it manually here,
	 *
	 * if for some reason the object is added back into the scene python can always get a new Proxy
	 */
	gameobj->InvalidateProxy();

	/* Keep the blender->game object association up to date
	 * note that all the replicas of an object will have the same
	 * blender object, that's why we need to check the game object
	 * as only the deletion of the original object must be recorded.
	 */
	if (gameobj->GetBlenderObject()) {
		// In some case the game object can contains a nullptr blender object e.g default camera.
		m_logicmgr->UnregisterGameObj(gameobj->GetBlenderObject(), gameobj);
	}

	// Remove all sensors/controllers/actuators from logicsystem.

	SCA_SensorList& sensors = gameobj->GetSensors();
	for (SCA_ISensor *sensor : sensors) {
		m_logicmgr->RemoveSensor(sensor);
	}

	SCA_ControllerList& controllers = gameobj->GetControllers();
	for (SCA_IController *controller : controllers) {
		m_logicmgr->RemoveController(controller);
		controller->ReParent(nullptr);
	}

	SCA_ActuatorList& actuators = gameobj->GetActuators();
	for (SCA_IActuator *actuator : actuators) {
		m_logicmgr->RemoveActuator(actuator);
	}
	// The sensors/controllers/actuators must also be released, this is done in ~SCA_IObject.

	// Now remove the timer properties from the time manager.
	for (unsigned short i = 0, numprops = gameobj->GetPropertyCount(); i < numprops; ++i) {
		EXP_Value *propval = gameobj->GetProperty(i);
		if (propval->GetProperty("timer")) {
			m_timemgr->RemoveTimeProperty(propval);
		}
	}

	/* If the object is the dupligroup proxy, you have to cleanup all m_dupliGroupObject's in all
	 * instances refering to this group. */
	if (gameobj->GetInstanceObjects()) {
		for (KX_GameObject *instance : gameobj->GetInstanceObjects()) {
			instance->RemoveDupliGroupObject();
		}
	}

	// If this object was part of a group, make sure to remove it from that group's instance list.
	KX_GameObject *group = gameobj->GetDupliGroupObject();
	if (group) {
		group->RemoveInstanceObject(gameobj);
	}

	if (m_obstacleSimulation) {
		m_obstacleSimulation->DestroyObstacleForObj(gameobj);
	}

	// Remove from activity culling list if present.
	if (!m_activityCullingList.empty()) {
		auto it = std::find(m_activityCullingList.begin(), m_activityCullingList.end(), gameobj);
		if (it != m_activityCullingList.end()) {
			// Swap with last element and pop back for O(1) removal (order doesn't matter).
			*it = m_activityCullingList.back();
			m_activityCullingList.pop_back();
			// Adjust index if we swapped the element currently being processed or before it.
			// However, since we are just iterating cyclically, it might skip one object or re-process one.
			// This is acceptable for culling logic.
		}
	}

	m_componentManager.UnregisterObject(gameobj);

	/* Batch-update registry: free the slot so the id lookup returns nullptr */
	if (gameobj->GetObjectId() != 0) {
		m_objectRegistry.Unregister(gameobj->GetObjectId());
		gameobj->SetObjectId(0);
	}

	/* MDEI fast-path cleanup: unregister proxy before removing RAS meshes */
	if (gameobj->HasFastRenderFlag()) {
		m_mdeiRenderer->UnregisterObject(gameobj);
	}

	gameobj->RemoveMeshes();

	m_rendererManager->InvalidateViewpoint(gameobj);

	bool ret = true;
	if (m_lightlist->RemoveValue(gameobj)) {
		ret = (gameobj->Release() != nullptr);
	}
	if (m_objectlist->RemoveValue(gameobj)) {
		ret = (gameobj->Release() != nullptr);
	}
	if (m_parentlist->RemoveValue(gameobj)) {
		ret = (gameobj->Release() != nullptr);
	}
	if (m_inactivelist->RemoveValue(gameobj)) {
		ret = (gameobj->Release() != nullptr);
	}
	if (m_fontlist->RemoveValue(gameobj)) {
		ret = (gameobj->Release() != nullptr);
	}
	if (m_cameralist->RemoveValue(gameobj)) {
		ret = (gameobj->Release() != nullptr);
	}
	if (m_renderlist->RemoveValue(gameobj)) {
		ret = (gameobj->Release() != nullptr);
	}

	CM_ListRemoveIfFound(m_activityCullingList, gameobj);

	// WARNING: 'gameobj' maybe be freed now, only compare, don't access.
	CM_ListRemoveIfFound(m_animatedlist, gameobj);
	m_euthanasyobjects.erase(gameobj);
	CM_ListRemoveIfFound(m_tempObjectList, gameobj);

	if (gameobj == m_activeCamera) {
		m_activeCamera = nullptr;
	}

	if (gameobj == m_overrideCullingCamera) {
		m_overrideCullingCamera = nullptr;
	}

	// Return value will be nullptr if the object is actually deleted (all reference gone)
	return ret;
}

KX_Camera *KX_Scene::GetActiveCamera()
{
	// nullptr if not defined.
	return m_activeCamera;
}

void KX_Scene::SetActiveCamera(KX_Camera *cam)
{
	m_activeCamera = cam;
}

KX_Camera *KX_Scene::GetOverrideCullingCamera() const
{
	return m_overrideCullingCamera;
}

void KX_Scene::SetOverrideCullingCamera(KX_Camera *cam)
{
	m_overrideCullingCamera = cam;
}

void KX_Scene::SetCameraOnTop(KX_Camera *cam)
{
	// No release and addref just change camera place.
	m_cameralist->RemoveValue(cam);
	m_cameralist->Add(cam);
}

void KX_Scene::PhysicsCullingCallback(KX_ClientObjectInfo *objectInfo, void *cullingInfo)
{
	CullingInfo *info = static_cast<CullingInfo *>(cullingInfo);
	KX_GameObject *gameobj = objectInfo->m_gameobject;

	if (!gameobj->Renderable(info->m_layer)) {
		return;
	}

	// Make object visible.
	if (info->m_updateState) {
		gameobj->GetCullingNode().SetCulled(false);
	}
	info->m_objects.push_back(gameobj);
}
bool g_useMultithreadCulling = true;

std::vector<KX_GameObject *> KX_Scene::CalculateVisibleMeshes(KX_Camera *cam, RAS_Rasterizer::StereoEye eye, int layer)
{
	std::vector<KX_GameObject *> objects;
	if (!cam->GetFrustumCulling()) {
		objects.reserve(m_objectlist->GetCount());
		for (KX_GameObject *gameobj : m_objectlist) {
			gameobj->GetCullingNode().SetCulled(false);
			objects.push_back(gameobj);
		}
		return objects;
	}

	return CalculateVisibleMeshes(cam->GetFrustum(eye), layer);
}

std::vector<KX_GameObject *> KX_Scene::CalculateVisibleMeshes(const SG_Frustum& frustum, int layer, bool update_state)
{
	bool relaxedDbvtThisSecond = false;
	if (update_state) {
		if (KX_KetsjiEngine *engine = KX_GetActiveEngine()) {
			const std::int64_t second = (std::int64_t)engine->GetRealTime();
			const std::uint64_t frameCounter = engine->GetFrameCounter();
			if (m_cullingBudgetSecond != second) {
				m_cullingBudgetSecond = second;
				m_cullingBudgetLastFrameCounter = frameCounter;
				m_cullingBudgetFramesThisSecond = 0;
				m_cullingBudgetRelaxedThisSecond = false;
			}
			if (m_cullingBudgetLastFrameCounter != frameCounter) {
				m_cullingBudgetLastFrameCounter = frameCounter;
				++m_cullingBudgetFramesThisSecond;
				if (m_cullingBudgetFramesThisSecond > 60) {
					m_cullingBudgetRelaxedThisSecond = true;
				}
			}
			relaxedDbvtThisSecond = m_cullingBudgetRelaxedThisSecond;
		}
	}

	if (update_state && m_dynamicObjects.empty() && m_objectlist->GetCount() > 0) {
		const int count = m_objectlist->GetCount();
		for (int i = 0; i < count; ++i) {
			KX_GameObject *gameobj = m_objectlist->GetValue(i);
			if (gameobj && gameobj->GetAutoUpdateBounds()) {
				m_dynamicObjects.push_back(gameobj);
			}
		}
	}

	bool anyBBoxModified = false;
	if (update_state) {
		RAS_CPU_PROFILE_SCOPE(RAS_CPU_SCENE_CVM_SCAN_BB_MODIFIED);
		m_boundingBoxManager->Update(false);
		for (RAS_BoundingBox *boundingBox : m_boundingBoxManager->GetActiveBoundingBoxes()) {
			if (boundingBox && boundingBox->GetModified()) {
				anyBBoxModified = true;
				break;
			}
		}
	}


	const bool hasDynamicObjects = !m_dynamicObjects.empty();
	const bool canCacheAcrossFrames = update_state && !anyBBoxModified && !hasDynamicObjects;
	const int objectCount = m_objectlist->GetCount();
	VisibilityCacheKey cacheKey{layer, reinterpret_cast<std::uintptr_t>(&frustum)};
	std::array<std::uint32_t, 16> matrixBits;
	if (canCacheAcrossFrames) {
		auto it = m_visibilityCache.end();
		{
			RAS_CPU_PROFILE_SCOPE(RAS_CPU_SCENE_CVM_CACHE_LOOKUP);
			const float *matData = &frustum.GetMatrix()[0];
			std::memcpy(matrixBits.data(), matData, sizeof(std::uint32_t) * 16);
			it = m_visibilityCache.find(cacheKey);
		}
		if (it != m_visibilityCache.end()) {
			const VisibilityCacheEntry& entry = it->second;
			if (entry.valid && entry.objectCount == objectCount && entry.matrixBits == matrixBits) {
				{
					RAS_CPU_PROFILE_SCOPE(RAS_CPU_SCENE_CVM_CACHE_APPLY);
					for (KX_GameObject *gameobj : entry.objects) {
						gameobj->GetCullingNode().SetCulled(false);
					}
				}
				return entry.objects;
			}
		}
	}

	// Reutiliza vetor membro — evita alocação de heap por frame
	m_visibleObjects.clear();
	m_visibleObjects.reserve(objectCount);

	bool dbvt_culling = false;
	if (m_dbvtCulling) {
		{
			RAS_CPU_PROFILE_SCOPE(RAS_CPU_SCENE_CVM_UPDATEDBVT);
			UpdateDbvt(update_state, anyBBoxModified);
		}

		const std::array<mt::vec4, 6>& planes = frustum.GetPlanes();
		const mt::mat4& matrix = frustum.GetMatrix();
		const int *viewport = KX_GetActiveEngine()->GetCanvas()->GetViewPort();

		{
			RAS_CPU_PROFILE_SCOPE(RAS_CPU_SCENE_CVM_DBVT_CULLINGTEST);
#ifdef WITH_BULLET
			if (CcdPhysicsEnvironment *bulletEnv = dynamic_cast<CcdPhysicsEnvironment *>(m_physicsEnvironment)) {
				const int planeCount = (relaxedDbvtThisSecond && m_dbvtOcclusionRes == 0) ? 4 : 6;
				dbvt_culling = bulletEnv->CullingTestKX(m_visibleObjects, layer, update_state, planes, planeCount, m_dbvtOcclusionRes, viewport, matrix);
			}
			else
#endif
			{
				CullingInfo info(layer, m_visibleObjects, update_state);
				dbvt_culling = m_physicsEnvironment->CullingTest(PhysicsCullingCallback, &info, planes, m_dbvtOcclusionRes, viewport, matrix);
			}
		}
	}

	if (!dbvt_culling) {
		RAS_CPU_PROFILE_SCOPE(RAS_CPU_SCENE_CVM_FALLBACK_HANDLER);
		m_boundingBoxManager->Update(false);
		KX_CullingHandler handler(m_objectlist, frustum, layer);
		m_visibleObjects = handler.Process();
	}

	{
		RAS_CPU_PROFILE_SCOPE(RAS_CPU_SCENE_CVM_CLEAR_BB_MODIFIED);
		m_boundingBoxManager->ClearModified();
	}

	if (canCacheAcrossFrames) {
		RAS_CPU_PROFILE_SCOPE(RAS_CPU_SCENE_CVM_CACHE_STORE);
		VisibilityCacheEntry& entry = m_visibilityCache[cacheKey];
		entry.matrixBits = matrixBits;
		entry.objectCount = objectCount;
		entry.objects = m_visibleObjects;
		entry.valid = true;
	}

	return m_visibleObjects;
}

void KX_Scene::UpdateDbvt(bool update_state, bool update_bounds)
{
	// Lazy init da lista de objetos dinâmicos — feito uma única vez
	if (m_dynamicObjects.empty() && m_objectlist->GetCount() > 0) {
		const int count = m_objectlist->GetCount();
		for (int i = 0; i < count; ++i) {
			KX_GameObject *gameobj = m_objectlist->GetValue(i);
			if (gameobj && gameobj->GetAutoUpdateBounds())
				m_dynamicObjects.push_back(gameobj);
		}
	}

	// Fix 3: só itera objetos dinâmicos para reset de culling e update de bounds.
	// Objetos estáticos (AutoUpdateBounds=false) nunca mudam de AABB.
	if (update_state) {
		{
			RAS_CPU_PROFILE_SCOPE(RAS_CPU_SCENE_UPDATEDBVT_SET_CULLED);
			for (KX_GameObject *gameobj : m_dynamicObjects) {
				if (gameobj) {
					gameobj->GetCullingNode().SetCulled(true);
				}
			}
		}
		if (update_bounds) {
			RAS_CPU_PROFILE_SCOPE(RAS_CPU_SCENE_UPDATEDBVT_UPDATE_BOUNDS);
			for (KX_GameObject *gameobj : m_dynamicObjects) {
				if (gameobj) {
					gameobj->UpdateBounds(false);
				}
			}
		}
	}
	else {
		// Sem update_state: não mexe no estado de culling dos objetos.
		if (update_bounds) {
			RAS_CPU_PROFILE_SCOPE(RAS_CPU_SCENE_UPDATEDBVT_UPDATE_BOUNDS);
			for (KX_GameObject *gameobj : m_dynamicObjects) {
				if (gameobj) {
					gameobj->UpdateBounds(false);
				}
			}
		}
	}
}


RAS_DebugDraw& KX_Scene::GetDebugDraw()
{
	return m_debugDraw;
}

void KX_Scene::DrawDebug(const std::vector<KX_GameObject *>& objects,
                         KX_DebugOption showBoundingBox, KX_DebugOption showArmatures)
{
	if (showBoundingBox != KX_DebugOption::DISABLE) {
		for (KX_GameObject *gameobj : objects) {
			const mt::vec3& scale = gameobj->NodeGetWorldScaling();
			const mt::vec3& position = gameobj->NodeGetWorldPosition();
			const mt::mat3& orientation = gameobj->NodeGetWorldOrientation();
			const SG_BBox& box = gameobj->GetCullingNode().GetAabb();
			const mt::vec3& center = box.GetCenter();

			m_debugDraw.DrawAabb(position, orientation, box.GetMin() * scale, box.GetMax() * scale,
			                   mt::vec4(1.0f, 0.0f, 1.0f, 1.0f));

			static const mt::vec3 axes[] = {mt::axisX3, mt::axisY3, mt::axisZ3};
			static const mt::vec4 colors[] = {mt::vec4(1.0f, 0.0f, 0.0f, 1.0f), mt::vec4(0.0f, 1.0f, 0.0f, 1.0f), mt::vec4(0.0f, 0.0f, 1.0f, 1.0f)};
			// Render center in red, green and blue.
			for (unsigned short i = 0; i < 3; ++i) {
				m_debugDraw.DrawLine(orientation * (center * scale) + position,
						orientation * ((center + axes[i]) * scale) + position, colors[i]);
			}
		}
	}

	if (showArmatures != KX_DebugOption::DISABLE) {
		// The side effect of a armature is that it was added in the animated object list.
		for (KX_GameObject *gameobj : m_animatedlist) {
			if (gameobj->GetGameObjectType() == SCA_IObject::OBJ_ARMATURE) {
				BL_ArmatureObject *armature = static_cast<BL_ArmatureObject *>(gameobj);
				if (showArmatures == KX_DebugOption::FORCE || armature->GetDrawDebug()) {
					armature->DrawDebug(m_debugDraw);
				}
			}
		}
	}
}

void KX_Scene::RenderDebugProperties(RAS_DebugDraw& debugDraw, int xindent, int ysize, int& xcoord, int& ycoord, unsigned short propsMax)
{
	static const mt::vec4 white(1.0f, 1.0f, 1.0f, 1.0f);

	// The 'normal' debug props.
	const std::vector<SCA_DebugProp>& debugproplist = GetDebugProperties();

	unsigned short numprop = debugproplist.size();
	if (numprop > propsMax) {
		numprop = propsMax;
	}

	for (unsigned short i = 0; i < numprop; ++i) {
		const SCA_DebugProp& debugProp = debugproplist[i];
		SCA_IObject *gameobj = debugProp.m_obj;
		const std::string objname = gameobj->GetName();
		const std::string& propname = debugProp.m_name;
		if (propname == "__state__") {
			// reserve name for object state
			unsigned int state = gameobj->GetState();
			std::string debugtxt = objname + "." + propname + " = ";
			bool first = true;
			for (int statenum = 1; state; state >>= 1, statenum++) {
				if (state & 1) {
					if (!first) {
						debugtxt += ",";
					}
					debugtxt += std::to_string(statenum);
					first = false;
				}
			}
			debugDraw.RenderText2d(debugtxt, mt::vec2(xcoord + xindent, ycoord), white);
			ycoord += ysize;
		}
		else {
			EXP_Value *propval = gameobj->GetProperty(propname);
			if (propval) {
				const std::string text = propval->GetText();
				const std::string debugtxt = objname + ": '" + propname + "' = " + text;
				debugDraw.RenderText2d(debugtxt, mt::vec2(xcoord + xindent, ycoord), white);
				ycoord += ysize;
			}
		}
	}
}

void KX_Scene::FlushDebugDraw(RAS_Rasterizer *rasty, RAS_ICanvas *canvas)
{
	m_debugDraw.Flush(rasty, canvas);
}

void KX_Scene::LogicBeginFrame(double curtime, double framestep)
{

	// Have a look at temp objects.
	for (KX_GameObject *gameobj : m_tempObjectList) {
		EXP_FloatValue *propval = static_cast<EXP_FloatValue *>(gameobj->GetProperty("::timebomb"));

		if (propval) {
			const float timeleft = propval->GetNumber() - framestep;

			if (timeleft > 0) {
				propval->SetFloat(timeleft);
			}
			else {
				// Remove obj, remove the object from tempObjectList in NewRemoveObject only.
				DelayedRemoveObject(gameobj);
			}
		}
		else {
			// All object is the tempObjectList should have a clock.
			BLI_assert(false);
		}
	}
	m_logicmgr->BeginFrame(curtime, framestep);
}

void KX_Scene::AddAnimatedObject(KX_GameObject *gameobj)
{
	CM_ListAddIfNotFound(m_animatedlist, gameobj);
}


static void update_anim_thread_func(TaskPool *pool, void *taskdata, int UNUSED(threadid))
{
	KX_Scene::AnimationPoolData *data = (KX_Scene::AnimationPoolData *)BLI_task_pool_userdata(pool);
	double curtime = data->curtime;

	KX_GameObject *gameobj = (KX_GameObject *)taskdata;

	// Non-armature updates are fast enough, so just update them
	bool needs_update = gameobj->GetGameObjectType() != SCA_IObject::OBJ_ARMATURE;

	if (!needs_update) {
		// If we got here, we're looking to update an armature, so check its children meshes
		// to see if we need to bother with a more expensive pose update
		const std::vector<KX_GameObject *> children = gameobj->GetChildren();

		bool has_mesh = false, has_non_mesh = false;

		// Check for meshes that haven't been culled
		for (KX_GameObject *child : children) {
			// MDEI objects manage their own visibility outside the RAS culling
			// pipeline, so they never appear in the DBVT broadphase and
			// GetCulled() always returns true for them.  Treat them as visible
			// so that the armature pose update runs when they are present.
			if (!child->GetCullingNode().GetCulled() || child->HasFastRenderFlag()) {
				needs_update = true;
				break;
			}

			if (child->GetMeshList().empty()) {
				has_non_mesh = true;
			}
			else {
				has_mesh = true;
			}
		}

		// If we didn't find a non-culled mesh, check to see
		// if we even have any meshes, and update if this
		// armature has only non-mesh children.
		if (!needs_update && !has_mesh && has_non_mesh) {
			needs_update = true;
		}
	}

	// If the object is a culled armature, then we manage only the animation time and end of its animations.
	gameobj->UpdateActionManager(curtime, needs_update);

	if (needs_update) {
		const std::vector<KX_GameObject *> children = gameobj->GetChildren();
		KX_GameObject *parent = gameobj->GetParent();

		// Only do deformers here if they are not parented to an armature, otherwise the armature will
		// handle updating its children
		if (gameobj->GetDeformer() && (!parent || parent->GetGameObjectType() != SCA_IObject::OBJ_ARMATURE)) {
			gameobj->GetDeformer()->Update();
		}

		for (KX_GameObject *child : children) {
			if (child->GetDeformer()) {
				child->GetDeformer()->Update();
			}
		}

		/* MDEI fast-path: update skin deformers for MDEI children of this
		 * armature while obmat and chan_mat are still in their current-frame
		 * state (before ApplyPoseLocked restores the bind-pose obmat).
		 * We do this per-armature so each armature drives only its own MDEI
		 * children — no global flush needed here. */
		if (data->mdeiRenderer && gameobj->GetGameObjectType() == SCA_IObject::OBJ_ARMATURE) {
			for (KX_GameObject *child : children) {
				if (child->HasFastRenderFlag()) {
					data->mdeiRenderer->UpdateDeformerForObject(child);
				}
			}
		}
	}
}


void KX_Scene::UpdateAnimations(double curtime, bool restrict)
{
	if (restrict) {
		const double animTimeStep = 1.0 / m_blenderScene->r.frs_sec;

		/* Don't update if the time step is too small and if we are not asking for redundant
		 * updates like for different culling passes. */
		if ((curtime - m_previousAnimTime) < animTimeStep && curtime != m_previousAnimTime) {
			return;
		}

		// Sanity/debug print to make sure we're actually going at the fps we want (should be close to animTimeStep)
		// CM_Debug("Anim fps: " << 1.0 / (curtime - m_previousAnimTime));
		m_previousAnimTime = curtime;
	}

	m_animationPoolData.curtime       = curtime;
	m_animationPoolData.mdeiRenderer  = m_mdeiRenderer;

	std::vector<KX_GameObject *> animated;
	animated.reserve(m_animatedlist.size());
	for (KX_GameObject *gameobj : m_animatedlist) {
		if (!gameobj->IsActionsSuspended()) {
			animated.push_back(CM_AddRef(gameobj));
		}
	}

	for (KX_GameObject *gameobj : animated) {
		BLI_task_pool_push(m_animationPool, update_anim_thread_func, gameobj, false, TASK_PRIORITY_LOW);
	}

	BLI_task_pool_work_and_wait(m_animationPool);

	for (KX_GameObject *gameobj : animated) {
		gameobj->Release();
	}
	/* MDEI skin deformers are now updated inside update_anim_thread_func
	 * per-armature, before ApplyPoseLocked restores the bind-pose obmat.
	 * No global flush is needed here. */
}

void KX_Scene::LogicUpdateFrame(double curtime)
{
	m_componentManager.UpdateComponents();

	m_logicmgr->UpdateFrame(curtime);
}

void KX_Scene::LogicEndFrame()
{
	m_logicmgr->EndFrame();

	RemoveEuthanasyObjects();

	//prepare obstacle simulation for new frame
	if (m_obstacleSimulation) {
		m_obstacleSimulation->UpdateObstacles();
	}

	for (KX_FontObject *font : m_fontlist) {
		font->UpdateTextFromProperty();
	}
}

void KX_Scene::UpdateParents()
{
	// We use the SG dynamic list
	SG_Node *node;

	// Collect all scheduled nodes first
	std::vector<SG_Node*> scheduledNodes;
	// Heuristic reserve to avoid reallocations
	scheduledNodes.reserve(128);

	// Loop until no more nodes are scheduled (handles recursive scheduling during updates)
	do {
		scheduledNodes.clear();
		while ((node = SG_Node::GetNextScheduled(m_sghead))) {
			scheduledNodes.push_back(node);
		}

		if (scheduledNodes.empty()) {
			break;
		}

		if (scheduledNodes.size() < 100) {
			// Serial for small count to avoid TBB overhead
			for (SG_Node* n : scheduledNodes) {
				n->UpdateWorldData();
			}
		}
		else {
			// Parallel for large count
			// Use tbb::parallel_for to process independent root nodes in parallel.
			// SG_Node::UpdateWorldDataThread handles family locking internally.
			tbb::parallel_for(tbb::blocked_range<size_t>(0, scheduledNodes.size()),
				[&](const tbb::blocked_range<size_t>& r) {
					for (size_t i = r.begin(); i != r.end(); ++i) {
						scheduledNodes[i]->UpdateWorldDataThread();
					}
				}
			);
		}
	} while (!m_sghead.Empty());

	// The list must be empty here
	BLI_assert(m_sghead.Empty());

	// Some nodes may be ready for reschedule, move them to schedule list for next time.
	while ((node = SG_Node::GetNextRescheduled(m_sghead))) {
		node->Schedule(m_sghead);
	}
}

void KX_Scene::RenderBuckets(const std::vector<KX_GameObject *>& objects, RAS_Rasterizer::DrawType drawingMode, const mt::mat3x4& cameratransform,
                             RAS_Rasterizer *rasty, RAS_OffScreen *offScreen)
{
	/* ── MDEI fast-path: must run even when the RAS objects list is empty,
	 * because MDEI objects are not in the physics DBVT / shadow filter. ── */
	if (m_mdeiRenderer) {
		if (drawingMode == RAS_Rasterizer::RAS_SHADOW) {
			m_mdeiRenderer->RenderShadow(objects, rasty);
		}
		else {
			m_mdeiRenderer->RenderSolid(objects, rasty);
		}
	}

	/* Early-out for the standard RAS pipeline (safe after MDEI draw above). */
	if (objects.empty()) {
		return;
	}

	if (drawingMode == RAS_Rasterizer::RAS_SHADOW) {
		if (objects.size() == 1) {
			KX_GameObject *gameobj = objects.front();
			RAS_MeshUser *meshUser = gameobj->GetMeshUser();
			RAS_BatchGroup *group = meshUser ? meshUser->GetBatchGroup() : nullptr;
			
			if (group) {
				if (group->CastsShadows()) {
					group->ActivateShadowForGroup();
				}
			}
			else {
				if (gameobj->HasShadowCasterMaterial()) {
					gameobj->UpdateShadowBuckets();
				}
			}
		}
		else {
			m_shadowVisitedGroups.clear();
			if (m_shadowVisitedGroups.bucket_count() < objects.size()) {
				m_shadowVisitedGroups.reserve(objects.size());
			}
			
			for (KX_GameObject *gameobj : objects) {
				RAS_MeshUser *meshUser = gameobj->GetMeshUser();
				RAS_BatchGroup *group = meshUser ? meshUser->GetBatchGroup() : nullptr;
				
				if (group) {
					if (m_shadowVisitedGroups.insert(group).second) {
						if (group->CastsShadows()) {
							group->ActivateShadowForGroup();
						}
					}
				}
				else {
					if (gameobj->HasShadowCasterMaterial()) {
						gameobj->UpdateShadowBuckets();
					}
				}
			}
		}
	}
	else {
		for (KX_GameObject *gameobj : objects) {
			gameobj->UpdateBucketsNoOnlyShadow();
		}
	}

	m_bucketmanager->Renderbuckets(drawingMode, cameratransform, rasty, offScreen);
	KX_BlenderMaterial::EndFrame(rasty);
}

void KX_Scene::RenderSolidBuckets(const std::vector<KX_GameObject *>& objects,
                                   RAS_Rasterizer::DrawType drawingMode,
                                   const mt::mat3x4& cameratransform,
                                   RAS_Rasterizer *rasty)
{
	/* MDEI must run before the early-out — objects list never contains MDEI
	 * objects (they are invisible to DBVT culling). */
	if (m_mdeiRenderer)
		m_mdeiRenderer->RenderSolid(objects, rasty);

	if (objects.empty()) return;
	for (KX_GameObject *gameobj : objects)
		gameobj->UpdateBucketsNoOnlyShadow();
	m_bucketmanager->RenderbucketsSolids(drawingMode, cameratransform, rasty);
	// Não chama EndFrame — alphas ainda vão usar os buckets
}

void KX_Scene::RenderAlphaBuckets(RAS_Rasterizer::DrawType drawingMode,
                                   const mt::mat3x4& cameratransform,
                                   RAS_Rasterizer *rasty,
                                   RAS_OffScreen *offScreen)
{
	m_bucketmanager->RenderbucketsAlphas(drawingMode, cameratransform, rasty, offScreen);
	KX_BlenderMaterial::EndFrame(rasty);
}

void KX_Scene::RenderTextureRenderers(KX_TextureRendererManager::RendererCategory category, RAS_Rasterizer *rasty,
                                      RAS_OffScreen *offScreen, KX_Camera *camera, const RAS_Rect& viewport, const RAS_Rect& area)
{
	m_rendererManager->Render(category, rasty, offScreen, camera, viewport, area);
}

void KX_Scene::UpdateObjectLods(KX_Camera *cam, const std::vector<KX_GameObject *>& objects)
{
	const mt::vec3& cam_pos = cam->NodeGetWorldPosition();
	const float lodfactor = cam->GetLodDistanceFactor();

	// Only iterate objects that actually have a LOD manager — O(lod_count) instead of O(all_visible).
	for (KX_GameObject *gameobj : m_lodObjects) {
		gameobj->UpdateLod(this, cam_pos, lodfactor);
	}
}

void KX_Scene::AddLodObject(KX_GameObject *gameobj)
{
	m_lodObjects.push_back(gameobj);
}

void KX_Scene::RemoveLodObject(KX_GameObject *gameobj)
{
	auto it = std::find(m_lodObjects.begin(), m_lodObjects.end(), gameobj);
	if (it != m_lodObjects.end()) {
		m_lodObjects.erase(it);
	}
}

void KX_Scene::AddDynamicObject(KX_GameObject *gameobj)
{
	if (gameobj && gameobj->GetAutoUpdateBounds())
		m_dynamicObjects.push_back(gameobj);
}

void KX_Scene::RemoveDynamicObject(KX_GameObject *gameobj)
{
	auto it = std::find(m_dynamicObjects.begin(), m_dynamicObjects.end(), gameobj);
	if (it != m_dynamicObjects.end())
		m_dynamicObjects.erase(it);
}

void KX_Scene::InvalidateAllShadowCaches()
{
	int count = m_objectlist->GetCount();
	for (int i = 0; i < count; ++i) {
		KX_GameObject *gameobj = m_objectlist->GetValue(i);
		if (gameobj) {
			gameobj->InvalidateShadowCache();
		}
	}
}

void KX_Scene::SetLodHysteresis(bool active)
{
	m_isActivedHysteresis = active;
}

bool KX_Scene::IsActivedLodHysteresis() const
{
	return m_isActivedHysteresis;
}

void KX_Scene::SetLodHysteresisValue(int hysteresisvalue)
{
	m_lodHysteresisValue = hysteresisvalue;
}

int KX_Scene::GetLodHysteresisValue() const
{
	return m_lodHysteresisValue;
}

void KX_Scene::UpdateObjectActivity()
{
	if (!m_activityCulling) {
		return;
	}

	size_t totalObjects = m_activityCullingList.size();
	if (totalObjects == 0) {
		m_activityCullingIndex = 0;
		return;
	}

	m_activityCullingCamPositions.clear();
	m_activityCullingCamPositions.reserve(m_cameralist->GetCount());
	m_activityCullingCamCells.clear();
	m_activityCullingCamCells.reserve(m_cameralist->GetCount());

	const float gridSize = 64.0f;
	const float invGridSize = 1.0f / gridSize;
	const int maxCellDist = m_activityCullingMaxCellDist;
	const float maxCellDistSq = m_activityCullingMaxCellDistSq;

	for (KX_Camera *cam : m_cameralist) {
		if (cam->GetActivityCulling()) {
			const mt::vec3& campos = cam->NodeGetWorldPosition();
			m_activityCullingCamPositions.push_back(campos);

			const float gcx = campos.x * invGridSize;
			const float gcy = campos.y * invGridSize;
			m_activityCullingCamCells.push_back({int(std::floor(gcx)), int(std::floor(gcy))});
		}
	}

	const size_t camCount = m_activityCullingCamPositions.size();
	if (camCount == 0) {
		return;
	}

	size_t processCount = (totalObjects > 550) ? 550 : totalObjects;
	if (processCount == 0) {
		return;
	}

	// Safety: handle list size changes between frames
	m_activityCullingIndex %= totalObjects;

	for (size_t i = 0; i < processCount; ++i) {
		KX_GameObject *gameobj = m_activityCullingList[m_activityCullingIndex];
		const mt::vec3& obpos = gameobj->NodeGetWorldPosition();

		const float gx = obpos.x * invGridSize;
		const float gy = obpos.y * invGridSize;

		int ox = int(std::floor(gx));
		int oy = int(std::floor(gy));

		float dist = FLT_MAX;

		if (camCount == 1) {
			int cx = m_activityCullingCamCells[0].first;
			int cy = m_activityCullingCamCells[0].second;

			int dx = ox - cx;
			int dy = oy - cy;

			if (dx*dx + dy*dy <= maxCellDistSq) {
				dist = (obpos - m_activityCullingCamPositions[0]).LengthSquared();
			}
		}
		else {
			for (size_t j = 0; j < camCount; ++j) {
				int cx = m_activityCullingCamCells[j].first;
				int cy = m_activityCullingCamCells[j].second;

				int dx = ox - cx;
				int dy = oy - cy;

				if (dx*dx + dy*dy > maxCellDistSq) {
					continue;
				}

				float d = (obpos - m_activityCullingCamPositions[j]).LengthSquared();
				if (d < dist) {
					dist = d;
				}
			}
		}

		gameobj->UpdateActivity(dist);

		m_activityCullingIndex++;
		if (m_activityCullingIndex >= totalObjects) {
			m_activityCullingIndex = 0;
		}
	}
}


void KX_Scene::SetActivityCullingMaxDistance(float distance)
{
	const float gridSize = 64.0f;
	m_activityCullingMaxCellDist = (int)std::ceil(distance / gridSize);
	m_activityCullingMaxCellDistSq = (float)(m_activityCullingMaxCellDist * m_activityCullingMaxCellDist);
}

KX_NetworkMessageScene *KX_Scene::GetNetworkMessageScene() const
{
	return m_networkScene;
}

void KX_Scene::SetNetworkMessageScene(KX_NetworkMessageScene *netScene)
{
	m_networkScene = netScene;
}

PHY_IPhysicsEnvironment *KX_Scene::GetPhysicsEnvironment() const
{
	return m_physicsEnvironment;
}

void KX_Scene::SetPhysicsEnvironment(PHY_IPhysicsEnvironment *physEnv)
{
	m_physicsEnvironment = physEnv;
	if (m_physicsEnvironment) {
		KX_CollisionEventManager *collisionmgr = new KX_CollisionEventManager(m_logicmgr, physEnv);
		m_logicmgr->RegisterEventManager(collisionmgr);
	}
}

void KX_Scene::SetGravity(const mt::vec3& gravity)
{
	m_physicsEnvironment->SetGravity(gravity[0], gravity[1], gravity[2]);
}

mt::vec3 KX_Scene::GetGravity() const
{
	return m_physicsEnvironment->GetGravity();
}

void KX_Scene::SetSuspendedDelta(double suspendeddelta)
{
	m_suspendedDelta = suspendeddelta;
}

double KX_Scene::GetSuspendedDelta() const
{
	return m_suspendedDelta;
}

Scene *KX_Scene::GetBlenderScene() const
{
	return m_blenderScene;
}

static void MergeScene_LogicBrick(SCA_ILogicBrick *brick, KX_Scene *from, KX_Scene *to)
{
	SCA_LogicManager *logicmgr = to->GetLogicManager();

	brick->Replace_IScene(to);
	brick->Replace_NetworkScene(to->GetNetworkMessageScene());
	brick->SetLogicManager(to->GetLogicManager());

	/* If we end up replacing a KX_CollisionEventManager, we need to make sure
	 * physics controllers are properly in place. In other words, do this
	 * after merging physics controllers.
	 */
	SCA_ISensor *sensor = dynamic_cast<SCA_ISensor *>(brick);
	if (sensor) {
		sensor->Replace_EventManager(logicmgr);
	}

	SCA_2DFilterActuator *filter_actuator = dynamic_cast<SCA_2DFilterActuator *>(brick);
	if (filter_actuator) {
		filter_actuator->SetScene(to, to->Get2DFilterManager());
	}
}

static void MergeScene_GameObject(KX_GameObject *gameobj, KX_Scene *to, KX_Scene *from)
{
	const SCA_ActuatorList& actuators = gameobj->GetActuators();
	for (SCA_IActuator *actuator : actuators) {
		MergeScene_LogicBrick(actuator, from, to);
	}

	const SCA_SensorList& sensors = gameobj->GetSensors();
	for (SCA_ISensor *sensor : sensors) {
		MergeScene_LogicBrick(sensor, from, to);
	}

	const SCA_ControllerList& controllers = gameobj->GetControllers();
	for (SCA_IController *controller : controllers) {
		MergeScene_LogicBrick(controller, from, to);
	}

	// Graphics controller.
	PHY_IGraphicController *graphicCtrl = gameobj->GetGraphicController();
	if (graphicCtrl) {
		// Should update the m_cullingTree.
		graphicCtrl->SetPhysicsEnvironment(to->GetPhysicsEnvironment());
	}

	PHY_IPhysicsController *physicsCtrl = gameobj->GetPhysicsController();
	if (physicsCtrl) {
		physicsCtrl->SetPhysicsEnvironment(to->GetPhysicsEnvironment());
	}

	// SG_Node can hold a scene reference.
	SG_Node *sg = gameobj->GetNode();
	if (sg) {
		if (sg->GetClientInfo() == from) {
			sg->SetClientInfo(to);

			// Make sure to grab the children too since they might not be tied to a game object.
			const NodeList& children = sg->GetChildren();
			for (SG_Node *child : children) {
				child->SetClientInfo(to);
			}
		}
	}
	switch (gameobj->GetGameObjectType()) {
		// If the object is a light, update it's scene.
		case SCA_IObject::OBJ_LIGHT:
		{
			static_cast<KX_LightObject *>(gameobj)->UpdateScene(to);
			break;
		}
		// All armatures should be in the animated object list to be umpdated.
		case SCA_IObject::OBJ_ARMATURE:
		{
			to->AddAnimatedObject(gameobj);
			break;
		}
		// Force recreation of text users to link them to the merged scene text material.
		case SCA_IObject::OBJ_TEXT:
		{
			gameobj->RemoveMeshes();
			gameobj->AddMeshUser();
			break;
		}
	}

	// Add the object to the scene's logic manager.
	to->GetLogicManager()->RegisterGameObjectName(gameobj->GetName(), gameobj);
	to->GetLogicManager()->RegisterGameObj(gameobj->GetBlenderObject(), gameobj);

	for (KX_Mesh *meshobj : gameobj->GetMeshList()) {
		// Register the mesh object by name and blender object.
		to->GetLogicManager()->RegisterGameMeshName(meshobj->GetName(), gameobj->GetBlenderObject());
		to->GetLogicManager()->RegisterMeshName(meshobj->GetName(), meshobj);
	}
}

bool KX_Scene::MergeScene(KX_Scene *other)
{
	PHY_IPhysicsEnvironment *env = this->GetPhysicsEnvironment();
	PHY_IPhysicsEnvironment *env_other = other->GetPhysicsEnvironment();

	if ((env == nullptr) != (env_other == nullptr)) {
		// TODO - even when both scenes have NONE physics, the other is loaded with bullet enabled, ???
		CM_FunctionError("physics scenes type differ, aborting\n\tsource " << (int)(env != nullptr) << ", target " << (int)(env_other != nullptr));
		return false;
	}

	m_bucketmanager->Merge(other->GetBucketManager(), this);
	m_boundingBoxManager->Merge(other->GetBoundingBoxManager());
	m_rendererManager->Merge(other->GetTextureRendererManager());
	m_componentManager.Merge(other->GetPythonComponentManager());

	for (KX_GameObject *gameobj : *other->GetObjectList()) {
		MergeScene_GameObject(gameobj, this, other);

		// Add properties to debug list for LibLoad objects.
		if (KX_GetActiveEngine()->GetFlag(KX_KetsjiEngine::AUTO_ADD_DEBUG_PROPERTIES)) {
			AddObjectDebugProperties(gameobj);
		}
	}

	for (KX_GameObject *gameobj : *other->GetInactiveList()) {
		MergeScene_GameObject(gameobj, this, other);
	}

	if (env) {
		env->MergeEnvironment(env_other);
		EXP_ListValue<KX_GameObject> *otherObjects = other->GetObjectList();

		// List of all physics objects to merge (needed by ReplicateConstraints).
		std::vector<KX_GameObject *> physicsObjects;
		for (KX_GameObject *gameobj : otherObjects) {
			if (gameobj->GetPhysicsController()) {
				physicsObjects.push_back(gameobj);
			}
		}

		for (KX_GameObject *gameobj : physicsObjects) {
			// Replicate all constraints in the right physics environment.
			gameobj->ReplicateConstraints(m_physicsEnvironment, physicsObjects);
		}
	}

	/* Register all newly-merged objects in the batch-update registry.
	 * These are the objects from the initial scene conversion (not replicas)
	 * which bypass AddNodeReplicaObject and land here via MergeList. */
	{
		EXP_ListValue<KX_GameObject> *incoming = other->GetObjectList();
		for (KX_GameObject *gameobj : *incoming) {
			if (gameobj && gameobj->GetObjectId() == 0) {
				gameobj->SetObjectId(m_objectRegistry.Register(gameobj));
			}
		}
	}
	m_objectlist->MergeList(other->GetObjectList());
	other->GetObjectList()->ReleaseAndRemoveAll();

	m_inactivelist->MergeList(other->GetInactiveList());
	other->GetInactiveList()->ReleaseAndRemoveAll();

	m_parentlist->MergeList(other->GetRootParentList());
	other->GetRootParentList()->ReleaseAndRemoveAll();

	m_lightlist->MergeList(other->GetLightList());
	other->GetLightList()->ReleaseAndRemoveAll();

	m_cameralist->MergeList(other->GetCameraList());
	other->GetCameraList()->ReleaseAndRemoveAll();

	m_fontlist->MergeList(other->GetFontList());
	other->GetFontList()->ReleaseAndRemoveAll();

	m_renderlist->MergeList(other->GetRenderList());
	other->GetRenderList()->ReleaseAndRemoveAll();

	// Grab any timer properties from the other scene.
	SCA_TimeEventManager *timemgr_other = other->GetTimeEventManager();
	std::vector<EXP_Value *> times = timemgr_other->GetTimeValues();

	for (EXP_Value *time : times) {
		m_timemgr->AddTimeProperty(time);
	}

	return true;
}

KX_2DFilterManager *KX_Scene::Get2DFilterManager() const
{
	return m_filterManager;
}

RAS_OffScreen *KX_Scene::Render2DFilters(RAS_Rasterizer *rasty, RAS_ICanvas *canvas, RAS_OffScreen *inputofs, RAS_OffScreen *targetofs)
{
	return m_filterManager->RenderFilters(rasty, canvas, inputofs, targetofs);
}

KX_ObstacleSimulation *KX_Scene::GetObstacleSimulation()
{
	return m_obstacleSimulation;
}

void KX_Scene::SetObstacleSimulation(KX_ObstacleSimulation *obstacleSimulation)
{
	m_obstacleSimulation = obstacleSimulation;
}

// --- Grass system ---

KX_GrassSystem *KX_Scene::GetOrCreateGrassSystem()
{
	if (!m_grassSystem)
		m_grassSystem = new KX_GrassSystem(this);
	return m_grassSystem;
}

void KX_Scene::DrawGrassSystem(RAS_Rasterizer *rasty)
{
	if (m_grassSystem)
		m_grassSystem->Draw(rasty);
}

#ifdef WITH_PYTHON

void KX_Scene::RunDrawingCallbacks(DrawingCallbackType callbackType, KX_Camera *camera)
{
	PyObject *list = m_drawCallbacks[callbackType];
	if (!list || PyList_GET_SIZE(list) == 0) {
		return;
	}

	if (camera) {
		PyObject *args[1] = {camera->GetProxy()};
		EXP_RunPythonCallBackList(list, args, 0, 1);
	}
	else {
		EXP_RunPythonCallBackList(list, nullptr, 0, 0);
	}
}

void KX_Scene::RunOnRemoveCallbacks()
{
	PyObject *list = m_removeCallbacks;
	if (!list || PyList_GET_SIZE(list) == 0) {
		return;
	}

	PyObject *args[1] = { GetProxy() };
	EXP_RunPythonCallBackList(list, args, 0, 1);
}

PyTypeObject KX_Scene::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_Scene",
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
	0, 0, 0, 0, 0, 0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&EXP_Value::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef KX_Scene::Methods[] = {
	EXP_PYMETHODTABLE(KX_Scene, addObject),
	EXP_PYMETHODTABLE(KX_Scene, end),
	EXP_PYMETHODTABLE(KX_Scene, restart),
	EXP_PYMETHODTABLE(KX_Scene, replace),
	EXP_PYMETHODTABLE(KX_Scene, suspend),
	EXP_PYMETHODTABLE(KX_Scene, resume),
	EXP_PYMETHODTABLE(KX_Scene, drawObstacleSimulation),

	// Sict style access.
	EXP_PYMETHODTABLE(KX_Scene, get),

	{nullptr, nullptr} // Sentinel
};
static PyObject *Map_GetItem(PyObject *self_v, PyObject *item)
{
	KX_Scene *self = static_cast<KX_Scene *>EXP_PROXY_REF(self_v);
	const char *attr_str = _PyUnicode_AsString(item);
	PyObject *pyconvert;

	if (!self) {
		PyErr_SetString(PyExc_SystemError, "val = scene[key]: KX_Scene, " EXP_PROXY_ERROR_MSG);
		return nullptr;
	}

	if (!self->m_attrDict) {
		self->m_attrDict = PyDict_New();
	}

	if (self->m_attrDict && (pyconvert = PyDict_GetItem(self->m_attrDict, item))) {

		if (attr_str) {
			PyErr_Clear();
		}
		Py_INCREF(pyconvert);
		return pyconvert;
	}
	else {
		if (attr_str) {
			PyErr_Format(PyExc_KeyError, "value = scene[key]: KX_Scene, key \"%s\" does not exist", attr_str);
		}
		else {
			PyErr_SetString(PyExc_KeyError, "value = scene[key]: KX_Scene, key does not exist");
		}
		return nullptr;
	}

}

static int Map_SetItem(PyObject *self_v, PyObject *key, PyObject *val)
{
	KX_Scene *self = static_cast<KX_Scene *>EXP_PROXY_REF(self_v);
	const char *attr_str = _PyUnicode_AsString(key);
	if (!attr_str) {
		PyErr_Clear();
	}

	if (!self) {
		PyErr_SetString(PyExc_SystemError, "scene[key] = value: KX_Scene, " EXP_PROXY_ERROR_MSG);
		return -1;
	}

	if (!self->m_attrDict) {
		self->m_attrDict = PyDict_New();
	}

	if (!val) {
		// del ob["key"]
		int del = 0;

		if (self->m_attrDict) {
			del |= (PyDict_DelItem(self->m_attrDict, key) == 0) ? 1 : 0;
		}

		if (del == 0) {
			if (attr_str) {
				PyErr_Format(PyExc_KeyError, "scene[key] = value: KX_Scene, key \"%s\" could not be set", attr_str);
			}
			else {
				PyErr_SetString(PyExc_KeyError, "del scene[key]: KX_Scene, key could not be deleted");
			}
			return -1;
		}
		else if (self->m_attrDict) {
			// PyDict_DelItem sets an error when it fails.
			PyErr_Clear();
		}
	}
	else {
		// ob["key"] = value
		int set = 0;

		// Lazy init.
		if (!self->m_attrDict) {
			self->m_attrDict = PyDict_New();
		}

		if (PyDict_SetItem(self->m_attrDict, key, val) == 0) {
			set = 1;
		}
		else {
			PyErr_SetString(PyExc_KeyError, "scene[key] = value: KX_Scene, key not be added to internal dictionary");
		}

		if (set == 0) {
			// Pythons error value.
			return -1;

		}
	}

	// Success.
	return 0;
}

static int Seq_Contains(PyObject *self_v, PyObject *value)
{
	KX_Scene *self = static_cast<KX_Scene *>EXP_PROXY_REF(self_v);

	if (!self) {
		PyErr_SetString(PyExc_SystemError, "val in scene: KX_Scene, " EXP_PROXY_ERROR_MSG);
		return -1;
	}

	if (!self->m_attrDict) {
		self->m_attrDict = PyDict_New();
	}

	if (self->m_attrDict && PyDict_GetItem(self->m_attrDict, value)) {
		return 1;
	}

	return 0;
}

PyMappingMethods KX_Scene::Mapping = {
	(lenfunc)nullptr, // inquiry mp_length
	(binaryfunc)Map_GetItem, // binaryfunc mp_subscript
	(objobjargproc)Map_SetItem, // objobjargproc mp_ass_subscript
};

PySequenceMethods KX_Scene::Sequence = {
	nullptr, // Cant set the len otherwise it can evaluate as false.
	nullptr, // sq_concat
	nullptr, // sq_repeat
	nullptr, // sq_item
	nullptr, // sq_slice
	nullptr, // sq_ass_item
	nullptr, // sq_ass_slice
	(objobjproc)Seq_Contains, // sq_contains
	(binaryfunc)nullptr, // sq_inplace_concat
	(ssizeargfunc)nullptr, // sq_inplace_repeat
};

PyObject *KX_Scene::pyattr_get_name(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Scene *self = static_cast<KX_Scene *>(self_v);
	return PyUnicode_FromStdString(self->GetName());
}

PyObject *KX_Scene::pyattr_get_objects(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Scene *self = static_cast<KX_Scene *>(self_v);
	return self->GetObjectList()->GetProxy();
}

PyObject *KX_Scene::pyattr_get_objects_inactive(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Scene *self = static_cast<KX_Scene *>(self_v);
	return self->GetInactiveList()->GetProxy();
}

PyObject *KX_Scene::pyattr_get_lights(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Scene *self = static_cast<KX_Scene *>(self_v);
	return self->GetLightList()->GetProxy();
}

PyObject *KX_Scene::pyattr_get_filter_manager(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Scene *self = static_cast<KX_Scene *>(self_v);
	KX_2DFilterManager *filterManager = self->Get2DFilterManager();

	return filterManager->GetProxy();
}

PyObject *KX_Scene::pyattr_get_world(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Scene *self = static_cast<KX_Scene *>(self_v);
	KX_WorldInfo *world = self->GetWorldInfo();

	if (world->GetName().empty()) {
		Py_RETURN_NONE;
	}
	else {
		return world->GetProxy();
	}
}

PyObject *KX_Scene::pyattr_get_texts(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Scene *self = static_cast<KX_Scene *>(self_v);
	return self->GetFontList()->GetProxy();
}

PyObject *KX_Scene::pyattr_get_cameras(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Scene *self = static_cast<KX_Scene *>(self_v);
	return self->GetCameraList()->GetProxy();
}

PyObject *KX_Scene::pyattr_get_active_camera(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Scene *self = static_cast<KX_Scene *>(self_v);
	KX_Camera *cam = self->GetActiveCamera();
	if (cam) {
		return cam->GetProxy();
	}
	else {
		Py_RETURN_NONE;
	}
}

int KX_Scene::pyattr_set_active_camera(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_Scene *self = static_cast<KX_Scene *>(self_v);
	KX_Camera *camOb;

	if (!ConvertPythonToCamera(self, value, &camOb, false, "scene.active_camera = value: KX_Scene")) {
		return PY_SET_ATTR_FAIL;
	}

	self->SetActiveCamera(camOb);
	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_Scene::pyattr_get_overrideCullingCamera(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Scene *self = static_cast<KX_Scene *>(self_v);
	KX_Camera *cam = self->GetOverrideCullingCamera();
	if (cam) {
		return cam->GetProxy();
	}
	else {
		Py_RETURN_NONE;
	}
}

int KX_Scene::pyattr_set_overrideCullingCamera(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_Scene *self = static_cast<KX_Scene *>(self_v);
	KX_Camera *cam;

	if (!ConvertPythonToCamera(self, value, &cam, true, "scene.active_camera = value: KX_Scene")) {
		return PY_SET_ATTR_FAIL;
	}

	self->SetOverrideCullingCamera(cam);
	return PY_SET_ATTR_SUCCESS;
}

static std::map<const std::string, KX_Scene::DrawingCallbackType> callbacksTable = {
	{"pre_draw", KX_Scene::PRE_DRAW},
	{"pre_draw_setup", KX_Scene::PRE_DRAW_SETUP},
	{"post_draw", KX_Scene::POST_DRAW}
};

PyObject *KX_Scene::pyattr_get_drawing_callback(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Scene *self = static_cast<KX_Scene *>(self_v);

	const DrawingCallbackType type = callbacksTable[attrdef->m_name];
	if (!self->m_drawCallbacks[type]) {
		self->m_drawCallbacks[type] = PyList_New(0);
	}

	Py_INCREF(self->m_drawCallbacks[type]);

	return self->m_drawCallbacks[type];
}

int KX_Scene::pyattr_set_drawing_callback(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_Scene *self = static_cast<KX_Scene *>(self_v);

	if (!PyList_CheckExact(value)) {
		PyErr_SetString(PyExc_ValueError, "Expected a list");
		return PY_SET_ATTR_FAIL;
	}

	const DrawingCallbackType type = callbacksTable[attrdef->m_name];

	Py_XDECREF(self->m_drawCallbacks[type]);

	Py_INCREF(value);
	self->m_drawCallbacks[type] = value;

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_Scene::pyattr_get_remove_callback(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Scene *self = static_cast<KX_Scene *>(self_v);

	if (!self->m_removeCallbacks) {
		self->m_removeCallbacks = PyList_New(0);
	}

	Py_INCREF(self->m_removeCallbacks);

	return self->m_removeCallbacks;
}

int KX_Scene::pyattr_set_remove_callback(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_Scene *self = static_cast<KX_Scene *>(self_v);

	if (!PyList_CheckExact(value)) {
		PyErr_SetString(PyExc_ValueError, "Expected a list");
		return PY_SET_ATTR_FAIL;
	}

	Py_XDECREF(self->m_removeCallbacks);

	Py_INCREF(value);
	self->m_removeCallbacks = value;

	return PY_SET_ATTR_SUCCESS;
}

PyObject *KX_Scene::pyattr_get_gravity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef)
{
	KX_Scene *self = static_cast<KX_Scene *>(self_v);

	return PyObjectFrom(self->GetGravity());
}

int KX_Scene::pyattr_set_gravity(EXP_PyObjectPlus *self_v, const EXP_PYATTRIBUTE_DEF *attrdef, PyObject *value)
{
	KX_Scene *self = static_cast<KX_Scene *>(self_v);

	mt::vec3 vec;
	if (!PyVecTo(value, vec)) {
		return PY_SET_ATTR_FAIL;
	}

	self->SetGravity(vec);
	return PY_SET_ATTR_SUCCESS;
}

PyAttributeDef KX_Scene::Attributes[] = {
	EXP_PYATTRIBUTE_RO_FUNCTION("name", KX_Scene, pyattr_get_name),
	EXP_PYATTRIBUTE_RO_FUNCTION("objects", KX_Scene, pyattr_get_objects),
	EXP_PYATTRIBUTE_RO_FUNCTION("objectsInactive", KX_Scene, pyattr_get_objects_inactive),
	EXP_PYATTRIBUTE_RO_FUNCTION("lights", KX_Scene, pyattr_get_lights),
	EXP_PYATTRIBUTE_RO_FUNCTION("texts", KX_Scene, pyattr_get_texts),
	EXP_PYATTRIBUTE_RO_FUNCTION("cameras", KX_Scene, pyattr_get_cameras),
	EXP_PYATTRIBUTE_RO_FUNCTION("filterManager", KX_Scene, pyattr_get_filter_manager),
	EXP_PYATTRIBUTE_RO_FUNCTION("world", KX_Scene, pyattr_get_world),
	EXP_PYATTRIBUTE_RW_FUNCTION("active_camera", KX_Scene, pyattr_get_active_camera, pyattr_set_active_camera),
	EXP_PYATTRIBUTE_RW_FUNCTION("overrideCullingCamera", KX_Scene, pyattr_get_overrideCullingCamera, pyattr_set_overrideCullingCamera),
	EXP_PYATTRIBUTE_RW_FUNCTION("pre_draw", KX_Scene, pyattr_get_drawing_callback, pyattr_set_drawing_callback),
	EXP_PYATTRIBUTE_RW_FUNCTION("post_draw", KX_Scene, pyattr_get_drawing_callback, pyattr_set_drawing_callback),
	EXP_PYATTRIBUTE_RW_FUNCTION("pre_draw_setup", KX_Scene, pyattr_get_drawing_callback, pyattr_set_drawing_callback),
	EXP_PYATTRIBUTE_RW_FUNCTION("onRemove", KX_Scene, pyattr_get_remove_callback, pyattr_set_remove_callback),
	EXP_PYATTRIBUTE_RW_FUNCTION("gravity", KX_Scene, pyattr_get_gravity, pyattr_set_gravity),
	EXP_PYATTRIBUTE_BOOL_RO("suspended", KX_Scene, m_suspend),
	EXP_PYATTRIBUTE_BOOL_RO("activityCulling", KX_Scene, m_activityCulling),
	EXP_PYATTRIBUTE_BOOL_RO("dbvt_culling", KX_Scene, m_dbvtCulling),
	EXP_PYATTRIBUTE_NULL // Sentinel
};

EXP_PYMETHODDEF_DOC(KX_Scene, addObject,
                    "addObject(object, other, time=0)\n"
                    "Returns the added object.\n")
{
	PyObject *pyob, *pyreference = Py_None;
	KX_GameObject *ob, *reference;

	float time = 0.0f;

	if (!PyArg_ParseTuple(args, "O|Of:addObject", &pyob, &pyreference, &time)) {
		return nullptr;
	}

	if (!ConvertPythonToGameObject(m_logicmgr, pyob, &ob, false, "scene.addObject(object, reference, time): KX_Scene (first argument)") ||
	    !ConvertPythonToGameObject(m_logicmgr, pyreference, &reference, true, "scene.addObject(object, reference, time): KX_Scene (second argument)")) {
		return nullptr;
	}

	if (!m_inactivelist->SearchValue(ob)) {
		PyErr_Format(PyExc_ValueError, "scene.addObject(object, reference, time): KX_Scene (first argument): object must be in an inactive layer");
		return nullptr;
	}
	KX_GameObject *replica = AddReplicaObject(ob, reference, time);

	/* Release here because AddReplicaObject AddRef's
	 * the object is added to the scene so we don't want python to own a reference. */
	replica->Release();
	return replica->GetProxy();
}

EXP_PYMETHODDEF_DOC(KX_Scene, end,
                    "end()\n"
                    "Removes this scene from the game.\n")
{

	KX_GetActiveEngine()->RemoveScene(m_sceneName);

	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_Scene, restart,
                    "restart()\n"
                    "Restarts this scene.\n")
{
	KX_GetActiveEngine()->ReplaceScene(m_sceneName, m_sceneName);

	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_Scene, replace,
                    "replace(newScene)\n"
                    "Replaces this scene with another one.\n"
                    "Return True if the new scene exists and scheduled for replacement, False otherwise.\n")
{
	char *name;

	if (!PyArg_ParseTuple(args, "s:replace", &name)) {
		return nullptr;
	}

	if (KX_GetActiveEngine()->ReplaceScene(m_sceneName, name)) {
		Py_RETURN_TRUE;
	}

	Py_RETURN_FALSE;
}

EXP_PYMETHODDEF_DOC(KX_Scene, suspend,
                    "suspend()\n"
                    "Suspends this scene.\n")
{
	Suspend();

	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_Scene, resume,
                    "resume()\n"
                    "Resumes this scene.\n")
{
	Resume();

	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_Scene, drawObstacleSimulation,
                    "drawObstacleSimulation()\n"
                    "Draw debug visualization of obstacle simulation.\n")
{
	if (GetObstacleSimulation()) {
		GetObstacleSimulation()->DrawObstacles();
	}

	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_Scene, get, "")
{
	PyObject *key;
	PyObject *def = Py_None;
	PyObject *ret;

	if (!PyArg_ParseTuple(args, "O|O:get", &key, &def)) {
		return nullptr;
	}

	if (m_attrDict && (ret = PyDict_GetItem(m_attrDict, key))) {
		Py_INCREF(ret);
		return ret;
	}

	Py_INCREF(def);
	return def;
}

bool ConvertPythonToScene(PyObject *value, KX_Scene **scene, bool py_none_ok, const char *error_prefix)
{
	if (value == nullptr) {
		PyErr_Format(PyExc_TypeError, "%s, python pointer nullptr, should never happen", error_prefix);
		*scene = nullptr;
		return false;
	}

	if (value == Py_None) {
		*scene = nullptr;

		if (py_none_ok) {
			return true;
		}
		else {
			PyErr_Format(PyExc_TypeError, "%s, expected KX_Scene or a KX_Scene name, None is invalid", error_prefix);
			return false;
		}
	}

	if (PyUnicode_Check(value)) {
		*scene = KX_GetActiveEngine()->CurrentScenes()->FindValue(std::string(_PyUnicode_AsString(value)));

		if (*scene) {
			return true;
		}
		else {
			PyErr_Format(PyExc_ValueError, "%s, requested name \"%s\" did not match any in game", error_prefix, _PyUnicode_AsString(value));
			return false;
		}
	}

	if (PyObject_TypeCheck(value, &KX_Scene::Type)) {
		*scene = static_cast<KX_Scene *>EXP_PROXY_REF(value);

		// Sets the error.
		if (*scene == nullptr) {
			PyErr_Format(PyExc_SystemError, "%s, " EXP_PROXY_ERROR_MSG, error_prefix);
			return false;
		}

		return true;
	}

	*scene = nullptr;

	if (py_none_ok) {
		PyErr_Format(PyExc_TypeError, "%s, expect a KX_Scene, a string or None", error_prefix);
	}
	else {
		PyErr_Format(PyExc_TypeError, "%s, expect a KX_Scene or a string", error_prefix);
	}

	return false;
}

#endif  // WITH_PYTHON
