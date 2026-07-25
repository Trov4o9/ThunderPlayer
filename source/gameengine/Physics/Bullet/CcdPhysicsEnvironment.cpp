/** \file gameengine/Physics/Bullet/CcdPhysicsEnvironment.cpp
 *  \ingroup physbullet
 */
/*
   Bullet Continuous Collision Detection and Physics Library
   Copyright (c) 2003-2006 Erwin Coumans  http://continuousphysics.com/Bullet/

   This software is provided 'as-is', without any express or implied warranty.
   In no event will the authors be held liable for any damages arising from the use of this software.
   Permission is granted to anyone to use this software for any purpose,
   including commercial applications, and to alter it and redistribute it freely,
   subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
   2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
   3. This notice may not be removed or altered from any source distribution.
 */

#include "CcdPhysicsEnvironment.h"
#include "CcdPhysicsController.h"
#include "CcdGraphicController.h"
#include "CcdConstraint.h"
#include "CcdMathUtils.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include "btBulletDynamicsCommon.h"
#include "LinearMath/btIDebugDraw.h"
#include "BulletCollision/CollisionDispatch/btGhostObject.h"
#include "BulletCollision/CollisionDispatch/btSimulationIslandManager.h"
#include "BulletCollision/BroadphaseCollision/btDbvt.h"
#include "BulletSoftBody/btSoftRigidDynamicsWorld.h"
#include "BulletSoftBody/btSoftBodyRigidBodyCollisionConfiguration.h"
#include "BulletCollision/Gimpact/btGImpactCollisionAlgorithm.h"
#include "BulletCollision/NarrowPhaseCollision/btRaycastCallback.h"
#include "BulletDynamics/ConstraintSolver/btNNCGConstraintSolver.h"
#include "BulletDynamics/MLCPSolvers/btMLCPSolver.h"
#include "BulletDynamics/MLCPSolvers/btDantzigSolver.h"
#include "BulletDynamics/MLCPSolvers/btLemkeSolver.h"
#include "BulletDynamics/MLCPSolvers/btPATHSolver.h"

//profiling/timings
#include "LinearMath/btQuickprof.h"


#include "PHY_IMotionState.h"
#include "PHY_ICharacter.h"
#include "KX_GameObject.h"
#include "KX_Globals.h" // for KX_RasterizerDrawDebugLine
#include "KX_Mesh.h"
#include "BL_SceneConverter.h"
#include "RAS_DisplayArray.h"
#include "RAS_Deformer.h"
#include "RAS_MaterialBucket.h"
#include "RAS_IMaterial.h"

#include "DNA_scene_types.h"
#include "DNA_world_types.h"
#include "DNA_object_types.h" // for OB_MAX_COL_MASKS
#include "DNA_object_force_types.h"

#ifdef WITH_PYTHON
extern "C" {
#	include "Python.h"
}
#include "readerwriterqueue.h"
#include "BulletCollision/CollisionShapes/btTriangleMesh.h"
extern "C" {
#	include "E:/Mundo Aberto Game Dev/ThunderPLayer/ThunderPLayer/ThunderPLayer/upbge-0.2.5b/intern/glew-mx/glew-mx.h"
}
#endif

extern "C" {
	#include "BLI_utildefines.h"
	#include "BKE_object.h"
}

#define CCD_CONSTRAINT_DISABLE_LINKED_COLLISION 0x80

#include "BulletDynamics/Vehicle/btRaycastVehicle.h"
#include "BulletDynamics/Vehicle/btVehicleRaycaster.h"
#include "BulletDynamics/Vehicle/btWheelInfo.h"
#include "PHY_IVehicle.h"
static btRaycastVehicle::btVehicleTuning gTuning;

#ifdef WITH_PYTHON
namespace {
struct ReinstanceSubMeshSnapshot
{
	std::vector<mt::vec3_packed> positions;
	std::vector<unsigned int> triIndices;
	std::vector<unsigned int> origIndices;
	std::vector<CcdShapeConstructionInfo::UVco> triUv;
	unsigned int polygonStartIndex = 0;
};

struct ReinstanceJob
{
	CcdPhysicsEnvironment *env = nullptr;
	uint64_t envGeneration = 0;

	CcdShapeConstructionInfo *shapeInfo = nullptr;
	std::vector<CcdPhysicsController *> controllers;

	RAS_Mesh *meshobj = nullptr;
	RAS_Deformer *deformer = nullptr;

	bool useGimpact = false;
	bool useBvh = true;
	float margin = 0.06f;

	uint64_t epoch = 0;
	CcdPhysicsController *requester = nullptr;

	std::vector<ReinstanceSubMeshSnapshot> submeshes;

	btTriangleMesh *triangleMesh = nullptr;
	std::vector<int> polygonIndexArray;
	std::vector<CcdShapeConstructionInfo::UVco> triFaceUVcoArray;
	std::vector<btCollisionShape *> newShapes;

	PyObject *callback = nullptr;
	bool success = false;
};

static void DeleteBuiltShape(btCollisionShape *shape)
{
	if (!shape) {
		return;
	}
	if (shape->getShapeType() == SCALED_TRIANGLE_MESH_SHAPE_PROXYTYPE) {
		btTriangleMeshShape *meshShape = ((btScaledBvhTriangleMeshShape *)shape)->getChildShape();
		if (meshShape) {
			delete meshShape;
		}
	}
	delete shape;
}
}  // namespace

struct CcdPhysicsEnvironment::ReinstanceAsyncState
{
	moodycamel::ReaderWriterQueue<ReinstanceJob *> inQueue {4096};
	moodycamel::ReaderWriterQueue<ReinstanceJob *> outQueue {4096};
	ankerl::unordered_dense::map<CcdPhysicsController *, uint64_t> epochs;
	std::mutex cvMutex;
	std::condition_variable cv;
	std::atomic<uint32_t> wakeups {0};
	std::vector<btStridingMeshInterface *> retiredMeshInterfaces;
	std::atomic<bool> running {false};
	std::atomic<bool> shutdown {false};
	std::thread worker;
};
#endif

#include "LinearMath/btAabbUtil2.h"

#ifdef WIN32
void DrawRasterizerLine(const float *from, const float *to, int color);
#endif


#include "BulletDynamics/ConstraintSolver/btContactConstraint.h"

#include "CM_Message.h"
#include "CM_List.h"

// This was copied from the old KX_ConvertPhysicsObjects
#ifdef WIN32
#ifdef _MSC_VER
//only use SIMD Hull code under Win32
//#define TEST_HULL 1
#ifdef TEST_HULL
#define USE_HULL 1
//#define TEST_SIMD_HULL 1

#include "NarrowPhaseCollision/Hull.h"
#endif //#ifdef TEST_HULL

#endif //_MSC_VER
#endif //WIN32

class VehicleClosestRayResultCallback : public btCollisionWorld::ClosestRayResultCallback
{
private:
	const btCollisionShape *m_hitTriangleShape;
	unsigned short m_mask;

public:
	VehicleClosestRayResultCallback(const btVector3& rayFrom, const btVector3& rayTo, unsigned short mask)
		:btCollisionWorld::ClosestRayResultCallback(rayFrom, rayTo),
		m_mask(mask)
	{
	}

	virtual ~VehicleClosestRayResultCallback()
	{
	}

	virtual bool needsCollision(btBroadphaseProxy *proxy0) const
	{
		if (!btCollisionWorld::ClosestRayResultCallback::needsCollision(proxy0)) {
			return false;
		}

		btCollisionObject *object = (btCollisionObject *)proxy0->m_clientObject;
		CcdPhysicsController *phyCtrl = static_cast<CcdPhysicsController *>(object->getUserPointer());

		if (phyCtrl->GetCollisionGroup() & m_mask) {
			return true;
		}

		return false;
	}
};

class BlenderVehicleRaycaster : public btDefaultVehicleRaycaster
{
private:
	btDynamicsWorld *m_dynamicsWorld;
	unsigned short m_mask;

public:
	BlenderVehicleRaycaster(btDynamicsWorld *world)
		:btDefaultVehicleRaycaster(world),
		m_dynamicsWorld(world),
		m_mask((1 << OB_MAX_COL_MASKS) - 1)
	{
	}

	virtual void *castRay(const btVector3& from, const btVector3& to, btVehicleRaycasterResult& result)
	{
		//	RayResultCallback& resultCallback;

		VehicleClosestRayResultCallback rayCallback(from, to, m_mask);

		// We override btDefaultVehicleRaycaster so we can set this flag, otherwise our
		// vehicles go crazy (http://bulletphysics.org/Bullet/phpBB3/viewtopic.php?t=9662)
		rayCallback.m_flags |= btTriangleRaycastCallback::kF_UseSubSimplexConvexCastRaytest;

		m_dynamicsWorld->rayTest(from, to, rayCallback);

		if (rayCallback.hasHit()) {
			const btRigidBody *body = btRigidBody::upcast(rayCallback.m_collisionObject);
			if (body && body->hasContactResponse()) {
				result.m_hitPointInWorld = rayCallback.m_hitPointWorld;
				result.m_hitNormalInWorld = rayCallback.m_hitNormalWorld;
				result.m_hitNormalInWorld.normalize();
				result.m_distFraction = rayCallback.m_closestHitFraction;
				return (void *)body;
			}
		}
		return nullptr;
	}

	unsigned short GetRayCastMask() const
	{
		return m_mask;
	}

	void SetRayCastMask(unsigned short mask)
	{
		m_mask = mask;
	}
};

class WrapperVehicle : public PHY_IVehicle
{
	btRaycastVehicle *m_vehicle;
	BlenderVehicleRaycaster *m_raycaster;
	PHY_IPhysicsController *m_chassis;

public:
	WrapperVehicle(btRaycastVehicle *vehicle, BlenderVehicleRaycaster *raycaster, PHY_IPhysicsController *chassis)
		:m_vehicle(vehicle),
		m_raycaster(raycaster),
		m_chassis(chassis)
	{
	}

	~WrapperVehicle()
	{
		for (unsigned short i = 0, numWheels = GetNumWheels(); i < numWheels; ++i) {
			btWheelInfo& info = m_vehicle->getWheelInfo(i);
			PHY_IMotionState *motionState = (PHY_IMotionState *)info.m_clientInfo;
			delete motionState;
		}

		delete m_vehicle;
		delete m_raycaster;
	}

	btRaycastVehicle *GetVehicle()
	{
		return m_vehicle;
	}

	PHY_IPhysicsController *GetChassis()
	{
		return m_chassis;
	}

	virtual void AddWheel(PHY_IMotionState *motionState,
	                      const mt::vec3 &connectionPoint,
	                      const mt::vec3 &downDirection,
	                      const mt::vec3 &axleDirection,
	                      float suspensionRestLength,
	                      float wheelRadius,
	                      bool hasSteering)
	{
		btWheelInfo& info = m_vehicle->addWheel(ToBullet(connectionPoint), ToBullet(downDirection.Normalized()),
		                                        ToBullet(axleDirection.Normalized()), suspensionRestLength, wheelRadius, gTuning, hasSteering);
		info.m_clientInfo = motionState;
	}

	void SyncWheels()
	{
		int numWheels = GetNumWheels();
		int i;
		for (i = 0; i < numWheels; i++) {
			btWheelInfo& info = m_vehicle->getWheelInfo(i);
			PHY_IMotionState *motionState = (PHY_IMotionState *)info.m_clientInfo;
			m_vehicle->updateWheelTransform(i, false);
			const btTransform trans = m_vehicle->getWheelInfo(i).m_worldTransform;
			motionState->SetWorldOrientation(ToMt(trans.getBasis()));
			motionState->SetWorldPosition(ToMt(trans.getOrigin()));
		}
	}

	virtual int GetNumWheels() const
	{
		return m_vehicle->getNumWheels();
	}

	virtual mt::vec3 GetWheelPosition(int wheelIndex) const
	{
		if ((wheelIndex >= 0) && (wheelIndex < m_vehicle->getNumWheels())) {
			const btVector3 origin = m_vehicle->getWheelTransformWS(wheelIndex).getOrigin();
			return ToMt(origin);
		}
		return mt::zero3;
	}

	virtual mt::quat GetWheelOrientationQuaternion(int wheelIndex) const
	{
		if ((wheelIndex >= 0) && (wheelIndex < m_vehicle->getNumWheels())) {
			const btQuaternion quat = m_vehicle->getWheelTransformWS(wheelIndex).getRotation();
			return ToMt(quat);
		}
		return mt::quat(0.0f, 0.0f, 0.0f, 0.0f);
	}

	virtual float GetWheelRotation(int wheelIndex) const
	{
		float rotation = 0.0f;

		if ((wheelIndex >= 0) && (wheelIndex < m_vehicle->getNumWheels())) {
			btWheelInfo& info = m_vehicle->getWheelInfo(wheelIndex);
			rotation = info.m_rotation;
		}

		return rotation;
	}

	virtual int GetUserConstraintId() const
	{
		return m_vehicle->getUserConstraintId();
	}

	virtual int GetUserConstraintType() const
	{
		return m_vehicle->getUserConstraintType();
	}

	virtual void SetSteeringValue(float steering, int wheelIndex)
	{
		if ((wheelIndex >= 0) && (wheelIndex < m_vehicle->getNumWheels())) {
			m_vehicle->setSteeringValue(steering, wheelIndex);
		}
	}

	virtual void ApplyEngineForce(float force, int wheelIndex)
	{
		if ((wheelIndex >= 0) && (wheelIndex < m_vehicle->getNumWheels())) {
			m_vehicle->applyEngineForce(force, wheelIndex);
		}
	}

	virtual void ApplyBraking(float braking, int wheelIndex)
	{
		if ((wheelIndex >= 0) && (wheelIndex < m_vehicle->getNumWheels())) {
			btWheelInfo& info = m_vehicle->getWheelInfo(wheelIndex);
			info.m_brake = braking;
		}
	}

	virtual void SetWheelFriction(float friction, int wheelIndex)
	{
		if ((wheelIndex >= 0) && (wheelIndex < m_vehicle->getNumWheels())) {
			btWheelInfo& info = m_vehicle->getWheelInfo(wheelIndex);
			info.m_frictionSlip = friction;
		}
	}

	virtual void SetSuspensionStiffness(float suspensionStiffness, int wheelIndex)
	{
		if ((wheelIndex >= 0) && (wheelIndex < m_vehicle->getNumWheels())) {
			btWheelInfo& info = m_vehicle->getWheelInfo(wheelIndex);
			info.m_suspensionStiffness = suspensionStiffness;
		}
	}

	virtual void SetSuspensionDamping(float suspensionDamping, int wheelIndex)
	{
		if ((wheelIndex >= 0) && (wheelIndex < m_vehicle->getNumWheels())) {
			btWheelInfo& info = m_vehicle->getWheelInfo(wheelIndex);
			info.m_wheelsDampingRelaxation = suspensionDamping;
		}
	}

	virtual void SetSuspensionCompression(float suspensionCompression, int wheelIndex)
	{
		if ((wheelIndex >= 0) && (wheelIndex < m_vehicle->getNumWheels())) {
			btWheelInfo& info = m_vehicle->getWheelInfo(wheelIndex);
			info.m_wheelsDampingCompression = suspensionCompression;
		}
	}

	virtual void SetRollInfluence(float rollInfluence, int wheelIndex)
	{
		if ((wheelIndex >= 0) && (wheelIndex < m_vehicle->getNumWheels())) {
			btWheelInfo& info = m_vehicle->getWheelInfo(wheelIndex);
			info.m_rollInfluence = rollInfluence;
		}
	}

	virtual void SetCoordinateSystem(int rightIndex, int upIndex, int forwardIndex)
	{
		m_vehicle->setCoordinateSystem(rightIndex, upIndex, forwardIndex);
	}

	virtual void SetRayCastMask(short mask)
	{
		m_raycaster->SetRayCastMask(mask);
	}
	virtual short GetRayCastMask() const
	{
		return m_raycaster->GetRayCastMask();
	}
};

class CcdOverlapFilterCallBack : public btOverlapFilterCallback
{
private:
	class CcdPhysicsEnvironment *m_physEnv;
public:
	CcdOverlapFilterCallBack(CcdPhysicsEnvironment *env) :
		m_physEnv(env)
	{
	}
	virtual ~CcdOverlapFilterCallBack()
	{
	}
	/// return true when pairs need collision
	virtual bool needBroadphaseCollision(btBroadphaseProxy *proxy0, btBroadphaseProxy *proxy1) const;
};


void CcdPhysicsEnvironment::SetDebugDrawer(btIDebugDraw *debugDrawer)
{
	if (debugDrawer && m_dynamicsWorld) {
		m_dynamicsWorld->setDebugDrawer(debugDrawer);
	}
	m_debugDrawer = debugDrawer;
}

CcdPhysicsEnvironment::CcdPhysicsEnvironment(PHY_SolverType solverType, bool useDbvtCulling)
	:m_debugDrawer(nullptr),
	m_cullingCache(nullptr),
	m_cullingTree(nullptr),
	//m_numIterations(10),
	m_numTimeSubSteps(1),
	m_ccdMode(0),
	m_solverType(PHY_SOLVER_NONE),
	m_deactivationTime(2.0f),
	m_linearDeactivationThreshold(0.8f),
	m_angularDeactivationThreshold(1.0f),
	m_contactBreakingThreshold(0.02f),
	m_solver(nullptr),
	m_filterCallback(nullptr),
	m_ghostPairCallback(nullptr),
	m_ownDispatcher(nullptr)
{
	for (int i = 0; i < PHY_NUM_RESPONSE; i++) {
		m_triggerCallbacks[i] = nullptr;
	}

	m_collisionConfiguration = new btSoftBodyRigidBodyCollisionConfiguration();

	btCollisionDispatcher *dispatcher = new btCollisionDispatcher(m_collisionConfiguration);
	btGImpactCollisionAlgorithm::registerAlgorithm(dispatcher);
	m_ownDispatcher = dispatcher;

	m_broadphase = new btDbvtBroadphase();
	// avoid any collision in the culling tree
	if (useDbvtCulling) {
		m_cullingCache = new btNullPairCache();
		m_cullingTree = new btDbvtBroadphase(m_cullingCache);
	}

	m_filterCallback = new CcdOverlapFilterCallBack(this);
	m_ghostPairCallback = new btGhostPairCallback();
	m_broadphase->getOverlappingPairCache()->setOverlapFilterCallback(m_filterCallback);
	m_broadphase->getOverlappingPairCache()->setInternalGhostPairCallback(m_ghostPairCallback);

	SetSolverType(solverType);

	m_dynamicsWorld = new btSoftRigidDynamicsWorld(dispatcher, m_broadphase, m_solver, m_collisionConfiguration);
	m_dynamicsWorld->setInternalTickCallback(&CcdPhysicsEnvironment::StaticSimulationSubtickCallback, this);

	SetGravity(0.0f, 0.0f, -9.81f);

	m_staticOccluderDbvt = new btDbvt();
}

#ifdef WITH_PYTHON
static bool BuildReinstanceJob(ReinstanceJob *job, const std::atomic<bool>& shutdownFlag)
{
	if (!job || shutdownFlag.load(std::memory_order_acquire)) {
		return false;
	}

	if (!job->shapeInfo || job->shapeInfo->m_shapeType != PHY_SHAPE_MESH) {
		return false;
	}

	if (job->submeshes.empty()) {
		return false;
	}

	btTriangleMesh *triangleMesh = new btTriangleMesh(true, false);
	triangleMesh->m_weldingThreshold = 0.0f;

	std::vector<int> polygonIndexArray;
	std::vector<CcdShapeConstructionInfo::UVco> triFaceUVcoArray;

	size_t totalTriangles = 0;
	size_t totalUv = 0;
	for (const ReinstanceSubMeshSnapshot& sm : job->submeshes) {
		totalTriangles += (sm.triIndices.size() / 3);
		totalUv += sm.triUv.size();
	}
	polygonIndexArray.reserve(totalTriangles);
	triFaceUVcoArray.reserve(totalUv);

	for (const ReinstanceSubMeshSnapshot& sm : job->submeshes) {
		const size_t indexCount = sm.triIndices.size();
		if (indexCount == 0 || (indexCount % 3) != 0) {
			continue;
		}
		if (sm.triUv.size() != indexCount) {
			continue;
		}

		for (size_t j = 0; j < indexCount; j += 3) {
			const unsigned int i0 = sm.triIndices[j];
			const unsigned int i1 = sm.triIndices[j + 1];
			const unsigned int i2 = sm.triIndices[j + 2];

			if (i0 >= sm.positions.size() || i1 >= sm.positions.size() || i2 >= sm.positions.size()) {
				continue;
			}

			const mt::vec3_packed& p0 = sm.positions[i0];
			const mt::vec3_packed& p1 = sm.positions[i1];
			const mt::vec3_packed& p2 = sm.positions[i2];

			triangleMesh->addTriangle(
				btVector3(p0.x, p0.y, p0.z),
				btVector3(p1.x, p1.y, p1.z),
				btVector3(p2.x, p2.y, p2.z),
				false);

			polygonIndexArray.push_back((int)sm.polygonStartIndex + (int)(j / 3));
			triFaceUVcoArray.push_back(sm.triUv[j]);
			triFaceUVcoArray.push_back(sm.triUv[j + 1]);
			triFaceUVcoArray.push_back(sm.triUv[j + 2]);

			if (shutdownFlag.load(std::memory_order_acquire)) {
				delete triangleMesh;
				return false;
			}
		}
	}

	if (triangleMesh->getNumTriangles() == 0) {
		delete triangleMesh;
		return false;
	}

	std::vector<btCollisionShape *> newShapes;
	newShapes.reserve(job->controllers.size());

	for (size_t i = 0; i < job->controllers.size(); ++i) {
		if (shutdownFlag.load(std::memory_order_acquire)) {
			for (btCollisionShape *s : newShapes) {
				DeleteBuiltShape(s);
			}
			delete triangleMesh;
			return false;
		}

		btCollisionShape *shape = nullptr;
		if (job->useGimpact) {
			btGImpactMeshShape *gimpactShape = new btGImpactMeshShape(triangleMesh);
			gimpactShape->setMargin(job->margin);
			gimpactShape->updateBound();
			shape = gimpactShape;
		}
		else {
			btBvhTriangleMeshShape *unscaledShape = new btBvhTriangleMeshShape(triangleMesh, true, job->useBvh);
			unscaledShape->setMargin(job->margin);
			shape = new btScaledBvhTriangleMeshShape(unscaledShape, btVector3(1.0f, 1.0f, 1.0f));
			shape->setMargin(job->margin);
		}
		newShapes.push_back(shape);
	}

	job->triangleMesh = triangleMesh;
	job->polygonIndexArray = std::move(polygonIndexArray);
	job->triFaceUVcoArray = std::move(triFaceUVcoArray);
	job->newShapes = std::move(newShapes);
	job->success = true;
	return true;
}

void CcdPhysicsEnvironment::StartReinstanceAsyncIfNeeded()
{
	if (m_reinstanceAsync && m_reinstanceAsync->running.load(std::memory_order_acquire)) {
		return;
	}

	if (!m_reinstanceAsync) {
		m_reinstanceAsync = new ReinstanceAsyncState();
	}

	bool expected = false;
	if (!m_reinstanceAsync->running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
		return;
	}

	m_reinstanceAsync->shutdown.store(false, std::memory_order_release);
	const uint64_t gen = m_reinstanceGeneration.load(std::memory_order_relaxed);

	m_reinstanceAsync->worker = std::thread([this, gen]() {
		ReinstanceJob *job = nullptr;
		while (!m_reinstanceAsync->shutdown.load(std::memory_order_acquire)) {
			if (!m_reinstanceAsync->inQueue.try_dequeue(job)) {
				const uint32_t seq = m_reinstanceAsync->wakeups.load(std::memory_order_acquire);
				if (m_reinstanceAsync->inQueue.try_dequeue(job)) {
					goto got_job;
				}

				std::unique_lock<std::mutex> lock(m_reinstanceAsync->cvMutex);
				m_reinstanceAsync->cv.wait(lock, [this, seq]() {
					return m_reinstanceAsync->shutdown.load(std::memory_order_acquire) ||
					       (m_reinstanceAsync->wakeups.load(std::memory_order_acquire) != seq);
				});
				continue;
			}

		got_job:
			if (!job) {
				continue;
			}

			if (job->env != this || job->envGeneration != gen) {
				job->success = false;
			}
			else {
				job->success = BuildReinstanceJob(job, m_reinstanceAsync->shutdown);
			}
			while (!m_reinstanceAsync->outQueue.enqueue(job)) {
				if (m_reinstanceAsync->shutdown.load(std::memory_order_acquire)) {
					break;
				}
				std::this_thread::yield();
			}
		}
	});
}

void CcdPhysicsEnvironment::ShutdownReinstanceAsync()
{
	if (!m_reinstanceAsync) {
		return;
	}

	m_reinstanceGeneration.fetch_add(1, std::memory_order_acq_rel);
	m_reinstanceAsync->shutdown.store(true, std::memory_order_release);
	m_reinstanceAsync->wakeups.fetch_add(1, std::memory_order_release);
	m_reinstanceAsync->cv.notify_all();

	if (m_reinstanceAsync->worker.joinable()) {
		m_reinstanceAsync->worker.join();
	}

	ReinstanceJob *job = nullptr;
	while (m_reinstanceAsync->inQueue.try_dequeue(job)) {
		if (job) {
			if (job->callback) {
				Py_DECREF(job->callback);
				job->callback = nullptr;
			}
			for (btCollisionShape *s : job->newShapes) {
				DeleteBuiltShape(s);
			}
			if (job->triangleMesh) {
				delete job->triangleMesh;
				job->triangleMesh = nullptr;
			}
			delete job;
		}
	}
	while (m_reinstanceAsync->outQueue.try_dequeue(job)) {
		if (job) {
			if (job->callback) {
				Py_DECREF(job->callback);
				job->callback = nullptr;
			}
			for (btCollisionShape *s : job->newShapes) {
				DeleteBuiltShape(s);
			}
			if (job->triangleMesh) {
				delete job->triangleMesh;
				job->triangleMesh = nullptr;
			}
			delete job;
		}
	}

	for (btStridingMeshInterface *iface : m_reinstanceAsync->retiredMeshInterfaces) {
		delete iface;
	}
	m_reinstanceAsync->retiredMeshInterfaces.clear();

	delete m_reinstanceAsync;
	m_reinstanceAsync = nullptr;
}

void CcdPhysicsEnvironment::ProcessFinishedReinstanceJobs()
{
	if (!m_reinstanceAsync || m_reinstanceAsync->shutdown.load(std::memory_order_acquire)) {
		return;
	}

	if (!m_reinstanceAsync->retiredMeshInterfaces.empty()) {
		auto meshInterfaceStillUsed = [this](btStridingMeshInterface *iface) -> bool {
			if (!iface) {
				return false;
			}
			auto shapeUses = [iface](const btCollisionShape *shape, auto&& shapeUsesRef) -> bool {
				if (!shape) {
					return false;
				}
				if (shape->isCompound()) {
					const btCompoundShape *compound = static_cast<const btCompoundShape *>(shape);
					const int n = compound->getNumChildShapes();
					for (int i = 0; i < n; ++i) {
						if (shapeUsesRef(compound->getChildShape(i), shapeUsesRef)) {
							return true;
						}
					}
					return false;
				}
				const int t = shape->getShapeType();
				if (t == SCALED_TRIANGLE_MESH_SHAPE_PROXYTYPE) {
					const btTriangleMeshShape *child = static_cast<const btScaledBvhTriangleMeshShape *>(shape)->getChildShape();
					return child && child->getMeshInterface() == iface;
				}
				if (t == TRIANGLE_MESH_SHAPE_PROXYTYPE) {
					const btTriangleMeshShape *meshShape = static_cast<const btTriangleMeshShape *>(shape);
					return meshShape->getMeshInterface() == iface;
				}
				return false;
			};

			for (CcdPhysicsController *ctrl : m_controllers) {
				if (!ctrl) {
					continue;
				}
				const btCollisionShape *shape = ctrl->GetCollisionShape();
				if (shapeUses(shape, shapeUses)) {
					return true;
				}
			}
			return false;
		};

		std::vector<btStridingMeshInterface *> keep;
		keep.reserve(m_reinstanceAsync->retiredMeshInterfaces.size());
		for (btStridingMeshInterface *iface : m_reinstanceAsync->retiredMeshInterfaces) {
			if (meshInterfaceStillUsed(iface)) {
				keep.push_back(iface);
			}
			else {
				delete iface;
			}
		}
		m_reinstanceAsync->retiredMeshInterfaces.swap(keep);
	}

	ReinstanceJob *job = nullptr;
	while (m_reinstanceAsync->outQueue.try_dequeue(job)) {
		bool committed = false;
		bool ok = false;

		if (job && job->env == this &&
		    job->envGeneration == m_reinstanceGeneration.load(std::memory_order_acquire) &&
		    job->success && job->shapeInfo && job->triangleMesh &&
		    !job->controllers.empty() &&
		    !m_reinstanceAsync->shutdown.load(std::memory_order_acquire))
		{
			auto itEpoch = m_reinstanceAsync->epochs.find(job->requester);
			if (itEpoch != m_reinstanceAsync->epochs.end() && itEpoch->second == job->epoch) {
				std::vector<CcdPhysicsController *> current;
				current.reserve(job->controllers.size());
				for (CcdPhysicsController *ctrl : m_controllers) {
					if (ctrl && ctrl->m_shapeInfo == job->shapeInfo) {
						current.push_back(ctrl);
					}
				}

				if (current.size() == job->controllers.size()) {
					bool same = true;
					for (CcdPhysicsController *ctrl : current) {
						if (std::find(job->controllers.begin(), job->controllers.end(), ctrl) == job->controllers.end()) {
							same = false;
							break;
						}
					}
					if (same) {
						btTriangleIndexVertexArray *oldMeshInterface = nullptr;
						ok = job->shapeInfo->ApplyAsyncMesh(
							job->triangleMesh,
							std::move(job->polygonIndexArray),
							std::move(job->triFaceUVcoArray),
							job->meshobj,
							job->deformer,
							&oldMeshInterface);

						if (ok) {
							job->triangleMesh = nullptr;
							for (size_t i = 0; i < job->controllers.size(); ++i) {
								CcdPhysicsController *ctrl = job->controllers[i];
								if (!ctrl) {
									continue;
								}
								ctrl->ReplaceControllerShape(job->newShapes[i]);
								job->newShapes[i] = nullptr;
								RefreshCcdPhysicsController(ctrl);
							}
							if (oldMeshInterface) {
								m_reinstanceAsync->retiredMeshInterfaces.push_back(oldMeshInterface);
								oldMeshInterface = nullptr;
							}
							committed = true;
						}
					}
				}
			}
		}

		if (job && job->callback) {
			PyGILState_STATE gil = PyGILState_Ensure();
			PyObject *arg = (committed && ok) ? Py_True : Py_False;
			Py_INCREF(arg);
			PyObject *res = PyObject_CallFunctionObjArgs(job->callback, arg, nullptr);
			Py_XDECREF(res);
			Py_DECREF(arg);
			PyGILState_Release(gil);
			Py_DECREF(job->callback);
			job->callback = nullptr;
		}

		if (job) {
			for (btCollisionShape *s : job->newShapes) {
				DeleteBuiltShape(s);
			}
			job->newShapes.clear();
			if (job->triangleMesh) {
				delete job->triangleMesh;
				job->triangleMesh = nullptr;
			}
			delete job;
		}
	}
}

bool CcdPhysicsEnvironment::EnqueueReinstancePhysicsShapeAsync(CcdPhysicsController *requester, KX_GameObject *from_gameobj, RAS_Mesh *from_meshobj, bool dupli, PyObject *callback)
{
	if (!requester || !callback || callback == Py_None) {
		return false;
	}
	if (!PyCallable_Check(callback)) {
		return false;
	}

	StartReinstanceAsyncIfNeeded();
	if (!m_reinstanceAsync || m_reinstanceAsync->shutdown.load(std::memory_order_acquire)) {
		return false;
	}

	CcdShapeConstructionInfo *shapeInfo = requester->m_shapeInfo;
	if (!shapeInfo || shapeInfo->m_shapeType != PHY_SHAPE_MESH) {
		return false;
	}

	if (!from_gameobj && !from_meshobj) {
		from_gameobj = KX_GameObject::GetClientObject((KX_ClientObjectInfo *)requester->GetNewClientInfo());
	}

	if (dupli && (shapeInfo->GetRefCount() > 1)) {
		CcdShapeConstructionInfo *newShapeInfo = shapeInfo->GetReplica();
		shapeInfo->Release();
		shapeInfo = newShapeInfo;
		requester->m_shapeInfo = shapeInfo;
	}

	RAS_Deformer *deformer = nullptr;
	RAS_Mesh *meshobj = from_meshobj;
	if (!meshobj) {
		deformer = from_gameobj ? from_gameobj->GetDeformer() : nullptr;
		if (deformer) {
			meshobj = deformer->GetMesh();
		}
		else {
			const std::vector<KX_Mesh *>& meshes = from_gameobj->GetMeshList();
			if (!meshes.empty()) {
				meshobj = meshes.front();
			}
		}
	}
	if (!meshobj) {
		return false;
	}

	std::vector<CcdPhysicsController *> controllers;
	if (!dupli && shapeInfo->GetRefCount() > 1) {
		for (CcdPhysicsController *ctrl : m_controllers) {
			if (ctrl && ctrl->m_shapeInfo == shapeInfo) {
				controllers.push_back(ctrl);
			}
		}
	}
	else {
		controllers.push_back(requester);
	}

	ReinstanceJob *job = new ReinstanceJob();
	job->env = this;
	job->envGeneration = m_reinstanceGeneration.load(std::memory_order_relaxed);
	job->shapeInfo = shapeInfo;
	job->controllers = controllers;
	job->meshobj = meshobj;
	job->deformer = deformer;
	job->requester = requester;
	job->useGimpact = requester->GetConstructionInfo().m_bGimpact;
	job->useBvh = !requester->GetConstructionInfo().m_bSoft;
	job->margin = requester->GetConstructionInfo().m_margin;
	job->epoch = ++m_reinstanceAsync->epochs[requester];
	job->callback = callback;
	Py_INCREF(callback);

	unsigned int curPolygonStartIndex = 0;
	for (unsigned int i = 0, numMat = meshobj->GetNumMaterials(); i < numMat; ++i) {
		RAS_MeshMaterial *meshmat = meshobj->GetMeshMaterial(i);
		if (!meshmat) {
			continue;
		}
		RAS_IMaterial *mat = meshmat->GetBucket()->GetMaterial();
		RAS_DisplayArray *array = (deformer) ? deformer->GetDisplayArray(i) : meshmat->GetDisplayArray();
		if (!array) {
			continue;
		}

		const unsigned int indicesCount = array->GetTriangleIndexCount();
		if (mat && mat->IsCollider()) {
			ReinstanceSubMeshSnapshot sm;
			sm.polygonStartIndex = curPolygonStartIndex;

			const unsigned int vCount = array->GetVertexCount();
			const unsigned int iCount = indicesCount;

			sm.positions.assign(array->GetPositionsData(), array->GetPositionsData() + vCount);
			sm.triIndices.assign(array->GetTriangleIndicesData(), array->GetTriangleIndicesData() + iCount);

			sm.origIndices.resize(vCount);
			for (unsigned int v = 0; v < vCount; ++v) {
				sm.origIndices[v] = array->GetVertexInfo(v).GetOrigIndex();
			}

			sm.triUv.resize(iCount);
			for (unsigned int idx = 0; idx < iCount; ++idx) {
				const unsigned int vi = sm.triIndices[idx];
				const mt::vec2_packed& uv = array->GetUv(vi, 0);
				sm.triUv[idx] = {{uv.x, uv.y}};
			}

			job->submeshes.push_back(std::move(sm));
		}

		curPolygonStartIndex += indicesCount / 3;
	}

	if (job->submeshes.empty()) {
		Py_DECREF(job->callback);
		job->callback = nullptr;
		delete job;
		return false;
	}

	if (!m_reinstanceAsync->inQueue.enqueue(job)) {
		Py_DECREF(job->callback);
		job->callback = nullptr;
		delete job;
		return false;
	}
	m_reinstanceAsync->wakeups.fetch_add(1, std::memory_order_release);
	m_reinstanceAsync->cv.notify_one();

	return true;
}
#endif

void CcdPhysicsEnvironment::AddCcdPhysicsController(CcdPhysicsController *ctrl)
{
	// the controller is already added we do nothing
	if (!m_controllers.insert(ctrl).second) {
		return;
	}

	btRigidBody *body = ctrl->GetRigidBody();
	btCollisionObject *obj = ctrl->GetCollisionObject();

	//this m_userPointer is just used for triggers, see CallbackTriggers
	obj->setUserPointer(ctrl);
	if (body) {
		body->setGravity(m_gravity);
		body->setSleepingThresholds(m_linearDeactivationThreshold, m_angularDeactivationThreshold);
		//use explicit group/filter for finer control over collision in bullet => near/radar sensor
		m_dynamicsWorld->addRigidBody(body, ctrl->GetCollisionFilterGroup(), ctrl->GetCollisionFilterMask());

		// Restore constraints in case of physics restore.
		for (unsigned short i = 0, size = ctrl->getNumCcdConstraintRefs(); i < size; ++i) {
			btTypedConstraint *con = ctrl->getCcdConstraintRef(i);
			RestoreConstraint(ctrl, con);
		}

		// Handle potential vehicle constraints
		for (WrapperVehicle *wrapperVehicle : m_wrapperVehicles) {
			if (wrapperVehicle->GetChassis() == ctrl) {
				btRaycastVehicle *vehicle = wrapperVehicle->GetVehicle();
				m_dynamicsWorld->addVehicle(vehicle);
			}
		}
	}
	else {
		if (ctrl->GetSoftBody()) {
			btSoftBody *softBody = ctrl->GetSoftBody();
			m_dynamicsWorld->addSoftBody(softBody);
		}
		else {
			if (obj->getCollisionShape()) {
				m_dynamicsWorld->addCollisionObject(obj, ctrl->GetCollisionFilterGroup(), ctrl->GetCollisionFilterMask());
			}
			if (ctrl->GetCharacterController()) {
				m_dynamicsWorld->addAction(ctrl->GetCharacterController());
			}
		}
	}
	if (obj->isStaticOrKinematicObject()) {
		obj->setActivationState(ISLAND_SLEEPING);
	}

	BLI_assert(obj->getBroadphaseHandle());
}

void CcdPhysicsEnvironment::RemoveConstraint(btTypedConstraint *con, bool free)
{
	CcdConstraint *userData = (CcdConstraint *)con->getUserConstraintPtr();
	if (!userData->GetActive()) {
		return;
	}

	btRigidBody &rbA = con->getRigidBodyA();
	btRigidBody &rbB = con->getRigidBodyB();
	rbA.activate();
	rbB.activate();

	userData->SetActive(false);
	m_dynamicsWorld->removeConstraint(con);

	if (free) {
		if (rbA.getUserPointer()) {
			((CcdPhysicsController *)rbA.getUserPointer())->removeCcdConstraintRef(con);
		}

		if (rbB.getUserPointer()) {
			((CcdPhysicsController *)rbB.getUserPointer())->removeCcdConstraintRef(con);
		}

		/* Since we remove the constraint in the onwer and the target, we can delete it,
		 * KX_ConstraintWrapper keep the constraint id not the pointer, so no problems. */
		delete userData;
		delete con;
	}
}

void CcdPhysicsEnvironment::RemoveVehicle(WrapperVehicle *vehicle, bool free)
{
	m_dynamicsWorld->removeVehicle(vehicle->GetVehicle());
	if (free) {
		CM_ListRemoveIfFound(m_wrapperVehicles, vehicle);
		delete vehicle;
	}
}

void CcdPhysicsEnvironment::RemoveVehicle(CcdPhysicsController *ctrl, bool free)
{
	for (std::vector<WrapperVehicle *>::iterator it = m_wrapperVehicles.begin(); it != m_wrapperVehicles.end(); ) {
		WrapperVehicle *vehicle = *it;
		if (vehicle->GetChassis() == ctrl) {
			m_dynamicsWorld->removeVehicle(vehicle->GetVehicle());
			if (free) {
				it = m_wrapperVehicles.erase(it);
				delete vehicle;
				continue;
			}
		}
		++it;
	}
}

void CcdPhysicsEnvironment::RestoreConstraint(CcdPhysicsController *ctrl, btTypedConstraint *con)
{
	CcdConstraint *userData = (CcdConstraint *)con->getUserConstraintPtr();
	if (userData->GetActive()) {
		return;
	}

	btRigidBody &rbA = con->getRigidBodyA();
	btRigidBody &rbB = con->getRigidBodyB();

	CcdPhysicsController *other = nullptr;

	if (rbA.getUserPointer() && rbB.getUserPointer()) {
		CcdPhysicsController *ctrl0 = (CcdPhysicsController *)rbA.getUserPointer();
		CcdPhysicsController *ctrl1 = (CcdPhysicsController *)rbB.getUserPointer();
		other = (ctrl0 != ctrl) ? ctrl0 : ctrl1;
	}

	BLI_assert(other != nullptr);

	// Avoid add constraint if one of the objects are not available.
	if (IsActiveCcdPhysicsController(other)) {
		userData->SetActive(true);
		m_dynamicsWorld->addConstraint(con, userData->GetDisableCollision());
	}
}

bool CcdPhysicsEnvironment::RemoveCcdPhysicsController(CcdPhysicsController *ctrl, bool freeConstraints)
{
	// if the physics controller is already removed we do nothing
	if (!m_controllers.erase(ctrl)) {
		return false;
	}

	//also remove constraint
	btRigidBody *body = ctrl->GetRigidBody();
	if (body) {
		btBroadphaseProxy *proxy = ctrl->GetCollisionObject()->getBroadphaseHandle();
		btDispatcher *dispatcher = m_dynamicsWorld->getDispatcher();
		btOverlappingPairCache *pairCache = m_dynamicsWorld->getPairCache();

		CleanPairCallback cleanPairs(proxy, pairCache, dispatcher);
		pairCache->processAllOverlappingPairs(&cleanPairs, dispatcher);

		for (int i = ctrl->getNumCcdConstraintRefs() - 1; i >= 0; i--) {
			btTypedConstraint *con = ctrl->getCcdConstraintRef(i);
			RemoveConstraint(con, freeConstraints);
		}
		m_dynamicsWorld->removeRigidBody(ctrl->GetRigidBody());

		// Handle potential vehicle constraints
		RemoveVehicle(ctrl, freeConstraints);
	}
	else {
		//if a softbody
		if (ctrl->GetSoftBody()) {
			m_dynamicsWorld->removeSoftBody(ctrl->GetSoftBody());
		}
		else {
			m_dynamicsWorld->removeCollisionObject(ctrl->GetCollisionObject());

			if (ctrl->GetCharacterController()) {
				m_dynamicsWorld->removeAction(ctrl->GetCharacterController());
			}
		}
	}

	return true;
}

void CcdPhysicsEnvironment::UpdateCcdPhysicsController(CcdPhysicsController *ctrl, btScalar newMass, int newCollisionFlags, short int newCollisionGroup, short int newCollisionMask)
{
	// this function is used when the collisionning group of a controller is changed
	// remove and add the collistioning object
	btRigidBody *body = ctrl->GetRigidBody();
	btSoftBody *softBody = ctrl->GetSoftBody();
	btCollisionObject *obj = ctrl->GetCollisionObject();
	if (obj) {
		btVector3 inertia(0.0, 0.0, 0.0);
		m_dynamicsWorld->removeCollisionObject(obj);
		obj->setCollisionFlags(newCollisionFlags);
		if (body) {
			if (newMass) {
				body->getCollisionShape()->calculateLocalInertia(newMass, inertia);
			}
			body->setMassProps(newMass, inertia * ctrl->GetInertiaFactor());
			m_dynamicsWorld->addRigidBody(body, newCollisionGroup, newCollisionMask);
		}
		else if (softBody) {
			m_dynamicsWorld->addSoftBody(softBody);
		}
		else {
			m_dynamicsWorld->addCollisionObject(obj, newCollisionGroup, newCollisionMask);
		}
	}
	// to avoid nasty interaction, we must update the property of the controller as well
	ctrl->m_cci.m_mass = newMass;
	ctrl->m_cci.m_collisionFilterGroup = newCollisionGroup;
	ctrl->m_cci.m_collisionFilterMask = newCollisionMask;
	ctrl->m_cci.m_collisionFlags = newCollisionFlags;
}

void CcdPhysicsEnvironment::RefreshCcdPhysicsController(CcdPhysicsController *ctrl)
{
	btCollisionObject *obj = ctrl->GetCollisionObject();
	if (obj) {
		btBroadphaseProxy *proxy = obj->getBroadphaseHandle();
		if (proxy) {
			m_dynamicsWorld->getPairCache()->cleanProxyFromPairs(proxy, m_dynamicsWorld->getDispatcher());
		}
	}
}

bool CcdPhysicsEnvironment::IsActiveCcdPhysicsController(CcdPhysicsController *ctrl)
{
	return (m_controllers.find(ctrl) != m_controllers.end());
}

void CcdPhysicsEnvironment::AddCcdGraphicController(CcdGraphicController *ctrl)
{
	if (m_cullingTree && !ctrl->GetBroadphaseHandle()) {
		btVector3 minAabb;
		btVector3 maxAabb;
		ctrl->GetAabb(minAabb, maxAabb);

		ctrl->SetBroadphaseHandle(m_cullingTree->createProxy(
									  minAabb,
									  maxAabb,
									  INVALID_SHAPE_PROXYTYPE, // this parameter is not used
									  ctrl,
									  0, // this object does not collision with anything
									  0,
									  nullptr, // dispatcher => this parameter is not used
									  0));

		BLI_assert(ctrl->GetBroadphaseHandle());
	}
}

void CcdPhysicsEnvironment::RemoveCcdGraphicController(CcdGraphicController *ctrl)
{
	if (m_cullingTree) {
		btBroadphaseProxy *bp = ctrl->GetBroadphaseHandle();
		if (bp) {
			m_cullingTree->destroyProxy(bp, nullptr);
			ctrl->SetBroadphaseHandle(nullptr);
		}
	}
}

void CcdPhysicsEnvironment::UpdateCcdPhysicsControllerShape(CcdShapeConstructionInfo *shapeInfo)
{
	for (CcdPhysicsController *ctrl : m_controllers) {
		if (ctrl->GetShapeInfo() != shapeInfo) {
			continue;
		}

		ctrl->ReplaceControllerShape(nullptr);
		RefreshCcdPhysicsController(ctrl);
	}
}

void CcdPhysicsEnvironment::DebugDrawWorld()
{
	m_dynamicsWorld->debugDrawWorld();
}

void CcdPhysicsEnvironment::StaticSimulationSubtickCallback(btDynamicsWorld *world, btScalar timeStep)
{
	// Get the pointer to the CcdPhysicsEnvironment associated with this Bullet world.
	CcdPhysicsEnvironment *this_ = static_cast<CcdPhysicsEnvironment *>(world->getWorldUserInfo());
	this_->SimulationSubtickCallback(timeStep);
}

void CcdPhysicsEnvironment::SimulationSubtickCallback(btScalar timeStep)
{
	for (CcdPhysicsController *ctrl : m_controllers) {
		ctrl->SimulationTick(timeStep);
	}
}

bool CcdPhysicsEnvironment::ProceedDeltaTime(double curTime, float timeStep, float interval)
{
	int i;

	// Update Bullet global variables.
	gDeactivationTime = m_deactivationTime;
	gContactBreakingThreshold = m_contactBreakingThreshold;

#ifdef WITH_PYTHON
	ProcessFinishedReinstanceJobs();
#endif

	CachePreviousTransforms();

	float subStep = timeStep / float(m_numTimeSubSteps);
	i = m_dynamicsWorld->stepSimulation(interval, 10, subStep);//perform always a full simulation step
//uncomment next line to see where Bullet spend its time (printf in console)
//CProfileManager::dumpAll();

	ProcessFhSprings(curTime, i * subStep);

	for (CcdPhysicsController *ctrl : m_controllers) {
		ctrl->SynchronizeMotionStates(timeStep);
	}

	for (i = 0; i < m_wrapperVehicles.size(); i++) {
		WrapperVehicle *veh = m_wrapperVehicles[i];
		veh->SyncWheels();
	}

	CallbackTriggers();

	return true;
}

void CcdPhysicsEnvironment::SyncMotionStates(float timeStep)
{
	for (CcdPhysicsController *ctrl : m_controllers) {
		ctrl->SynchronizeMotionStates(timeStep);
	}

	for (int i = 0; i < m_wrapperVehicles.size(); i++) {
		WrapperVehicle *veh = m_wrapperVehicles[i];
		veh->SyncWheels();
	}
}

void CcdPhysicsEnvironment::SyncMotionStatesInterpolated(float timeStep, float alpha)
{
	for (CcdPhysicsController *ctrl : m_controllers) {
		ctrl->SynchronizeMotionStatesInterpolated(timeStep, alpha);
	}
}

void CcdPhysicsEnvironment::CachePreviousTransforms()
{
	for (CcdPhysicsController *ctrl : m_controllers) {
		ctrl->CachePreviousTransform();
	}
}

class ClosestRayResultCallbackNotMe : public btCollisionWorld::ClosestRayResultCallback
{
	btCollisionObject *m_owner;
	btCollisionObject *m_parent;

public:
	ClosestRayResultCallbackNotMe(const btVector3& rayFromWorld, const btVector3& rayToWorld, btCollisionObject *owner, btCollisionObject *parent)
		:btCollisionWorld::ClosestRayResultCallback(rayFromWorld, rayToWorld),
		m_owner(owner),
		m_parent(parent)
	{
	}

	virtual bool needsCollision(btBroadphaseProxy *proxy0) const
	{
		//don't collide with self
		if (proxy0->m_clientObject == m_owner) {
			return false;
		}

		if (proxy0->m_clientObject == m_parent) {
			return false;
		}

		return btCollisionWorld::ClosestRayResultCallback::needsCollision(proxy0);
	}
};

void CcdPhysicsEnvironment::ProcessFhSprings(double curTime, float interval)
{
	const float step = interval * KX_GetActiveEngine()->GetTicRate();

	for (CcdPhysicsController *ctrl : m_controllers) {
		btRigidBody *body = ctrl->GetRigidBody();

		if (body && (ctrl->GetConstructionInfo().m_do_fh || ctrl->GetConstructionInfo().m_do_rot_fh)) {
			//re-implement SM_FhObject.cpp using btCollisionWorld::rayTest and info from ctrl->getConstructionInfo()
			//send a ray from {0.0, 0.0, 0.0} towards {0.0, 0.0, -10.0}, in local coordinates
			CcdPhysicsController *parentCtrl = ctrl->GetParentRoot();
			btRigidBody *parentBody = parentCtrl ? parentCtrl->GetRigidBody() : nullptr;
			btRigidBody *cl_object = parentBody ? parentBody : body;

			if (body->isStaticOrKinematicObject()) {
				continue;
			}

			btVector3 rayDirLocal(0.0f, 0.0f, -10.0f);

			//m_dynamicsWorld
			//ctrl->GetRigidBody();
			btVector3 rayFromWorld = body->getCenterOfMassPosition();
			//btVector3	rayToWorld = rayFromWorld + body->getCenterOfMassTransform().getBasis() * rayDirLocal;
			//ray always points down the z axis in world space...
			btVector3 rayToWorld = rayFromWorld + rayDirLocal;

			ClosestRayResultCallbackNotMe resultCallback(rayFromWorld, rayToWorld, body, parentBody);

			m_dynamicsWorld->rayTest(rayFromWorld, rayToWorld, resultCallback);
			if (resultCallback.hasHit()) {
				//we hit this one: resultCallback.m_collisionObject;
				CcdPhysicsController *controller = static_cast<CcdPhysicsController *>(resultCallback.m_collisionObject->getUserPointer());

				if (controller) {
					if (controller->GetConstructionInfo().m_fh_distance < SIMD_EPSILON) {
						continue;
					}

					btRigidBody *hit_object = controller->GetRigidBody();
					if (!hit_object) {
						continue;
					}

					CcdConstructionInfo& hitObjShapeProps = controller->GetConstructionInfo();

					float distance = resultCallback.m_closestHitFraction * rayDirLocal.length() - ctrl->GetConstructionInfo().m_radius;
					if (distance >= hitObjShapeProps.m_fh_distance) {
						continue;
					}

					//btVector3 ray_dir = cl_object->getCenterOfMassTransform().getBasis()* rayDirLocal.Normalized();
					btVector3 ray_dir = rayDirLocal.normalized();
					btVector3 normal = resultCallback.m_hitNormalWorld;
					normal.normalize();

					if (ctrl->GetConstructionInfo().m_do_fh) {
						btVector3 lspot = cl_object->getCenterOfMassPosition() +
						                  rayDirLocal * resultCallback.m_closestHitFraction;

						lspot -= hit_object->getCenterOfMassPosition();
						btVector3 rel_vel = cl_object->getLinearVelocity() - hit_object->getVelocityInLocalPoint(lspot);
						btScalar rel_vel_ray = ray_dir.dot(rel_vel);
						btScalar spring_extent = 1.0f - distance / hitObjShapeProps.m_fh_distance;

						btScalar i_spring = spring_extent * hitObjShapeProps.m_fh_spring;
						btScalar i_damp =   rel_vel_ray * hitObjShapeProps.m_fh_damping;

						cl_object->setLinearVelocity(cl_object->getLinearVelocity() + (-(i_spring + i_damp) * ray_dir) * step);
						if (hitObjShapeProps.m_fh_normal) {
							cl_object->setLinearVelocity(cl_object->getLinearVelocity() + (i_spring + i_damp) * (normal - normal.dot(ray_dir) * ray_dir) * step);
						}

						btVector3 lateral = rel_vel - rel_vel_ray * ray_dir;

						if (ctrl->GetConstructionInfo().m_do_anisotropic) {
							//Bullet basis contains no scaling/shear etc.
							const btMatrix3x3& lcs = cl_object->getCenterOfMassTransform().getBasis();
							btVector3 loc_lateral = lateral * lcs;
							const btVector3& friction_scaling = cl_object->getAnisotropicFriction();
							loc_lateral *= friction_scaling;
							lateral = lcs * loc_lateral;
						}

						btScalar rel_vel_lateral = lateral.length();

						if (rel_vel_lateral > SIMD_EPSILON) {
							btScalar friction_factor = hit_object->getFriction();//cl_object->getFriction();

							btScalar max_friction = friction_factor * btMax(btScalar(0.0), i_spring);

							btScalar rel_mom_lateral = rel_vel_lateral / cl_object->getInvMass();

							btVector3 friction = (rel_mom_lateral > max_friction) ?
							                     -lateral * (max_friction / rel_vel_lateral) :
							                     -lateral;

							cl_object->applyCentralImpulse(friction * step);
						}
					}


					if (ctrl->GetConstructionInfo().m_do_rot_fh) {
						btVector3 up2 = cl_object->getWorldTransform().getBasis().getColumn(2);

						btVector3 t_spring = up2.cross(normal) * hitObjShapeProps.m_fh_spring;
						btVector3 ang_vel = cl_object->getAngularVelocity();

						// only rotations that tilt relative to the normal are damped
						ang_vel -= ang_vel.dot(normal) * normal;

						btVector3 t_damp = ang_vel * hitObjShapeProps.m_fh_damping;

						cl_object->setAngularVelocity(cl_object->getAngularVelocity() + (t_spring - t_damp) * step);
					}
				}
			}
		}
	}
}

int CcdPhysicsEnvironment::GetDebugMode() const
{
	if (m_debugDrawer) {
		return m_debugDrawer->getDebugMode();
	}
	return 0;
}

void CcdPhysicsEnvironment::SetDebugMode(int debugMode)
{
	if (m_debugDrawer) {
		m_debugDrawer->setDebugMode(debugMode);
	}
}

void CcdPhysicsEnvironment::SetNumIterations(int numIter)
{
	m_dynamicsWorld->getSolverInfo().m_numIterations = numIter;
}
void CcdPhysicsEnvironment::SetLinearSlop(float slop)
{
	m_dynamicsWorld->getSolverInfo().m_linearSlop = slop;
}
void CcdPhysicsEnvironment::SetDeactivationTime(float dTime)
{
	m_deactivationTime = dTime;
}
void CcdPhysicsEnvironment::SetDeactivationLinearTreshold(float linTresh)
{
	m_linearDeactivationThreshold = linTresh;

	// Update from all controllers.
	for (CcdPhysicsController *ctrl : m_controllers) {
		if (ctrl->GetRigidBody()) {
			ctrl->GetRigidBody()->setSleepingThresholds(m_linearDeactivationThreshold, m_angularDeactivationThreshold);
		}
	}
}
void CcdPhysicsEnvironment::SetDeactivationAngularTreshold(float angTresh)
{
	m_angularDeactivationThreshold = angTresh;

	// Update from all controllers.
	for (CcdPhysicsController *ctrl : m_controllers) {
		if (ctrl->GetRigidBody()) {
			ctrl->GetRigidBody()->setSleepingThresholds(m_linearDeactivationThreshold, m_angularDeactivationThreshold);
		}
	}
}

void CcdPhysicsEnvironment::SetContactBreakingTreshold(float contactBreakingTreshold)
{
	m_contactBreakingThreshold = contactBreakingTreshold;
}

void CcdPhysicsEnvironment::SetCcdMode(int ccdMode)
{
	m_ccdMode = ccdMode;
}

void CcdPhysicsEnvironment::SetSolverSorConstant(float sor)
{
	m_dynamicsWorld->getSolverInfo().m_sor = sor;
}

void CcdPhysicsEnvironment::SetSolverTau(float tau)
{
	m_dynamicsWorld->getSolverInfo().m_tau = tau;
}
void CcdPhysicsEnvironment::SetSolverDamping(float damping)
{
	m_dynamicsWorld->getSolverInfo().m_damping = damping;
}

void CcdPhysicsEnvironment::SetLinearAirDamping(float damping)
{
	//gLinearAirDamping = damping;
}

void CcdPhysicsEnvironment::SetUseEpa(bool epa)
{
	//gUseEpa = epa;
}

void CcdPhysicsEnvironment::SetSolverType(PHY_SolverType solverType)
{
	if (m_solverType == solverType) {
		return;
	}

	switch (solverType) {
		case PHY_SOLVER_SEQUENTIAL:
		{
			m_solver = new btSequentialImpulseConstraintSolver();
			break;
		}
		case PHY_SOLVER_NNCG:
		{
			m_solver = new btNNCGConstraintSolver();
			break;
		}
		case PHY_SOLVER_MLCP_DANTZIG:
		{
			m_solver = new btMLCPSolver(new btDantzigSolver());
			break;
		}
		case PHY_SOLVER_MLCP_LEMKE:
		{
			m_solver = new btMLCPSolver(new btLemkeSolver());
			break;
		}
		default:
		{
			BLI_assert(false);
		}
	}
	;
	m_solverType = solverType;
}

mt::vec3 CcdPhysicsEnvironment::GetGravity() const
{
	return ToMt(m_dynamicsWorld->getGravity());
}

void CcdPhysicsEnvironment::SetGravity(float x, float y, float z)
{
	m_gravity = btVector3(x, y, z);
	m_dynamicsWorld->setGravity(m_gravity);
	m_dynamicsWorld->getWorldInfo().m_gravity.setValue(x, y, z);
}

static int gConstraintUid = 1;

void CcdPhysicsEnvironment::RemoveConstraintById(int constraintId, bool free)
{
	// For soft body constraints
	if (constraintId == 0) {
		return;
	}

	int i;
	int numConstraints = m_dynamicsWorld->getNumConstraints();
	for (i = 0; i < numConstraints; i++) {
		btTypedConstraint *constraint = m_dynamicsWorld->getConstraint(i);
		if (constraint->getUserConstraintId() == constraintId) {
			RemoveConstraint(constraint, free);
			break;
		}
	}

	WrapperVehicle *vehicle = static_cast<WrapperVehicle *>(GetVehicleConstraint(constraintId));
	if (vehicle) {
		RemoveVehicle(vehicle, free);
	}
}

struct  FilterClosestRayResultCallback : public btCollisionWorld::ClosestRayResultCallback {
	PHY_IRayCastFilterCallback& m_phyRayFilter;
	const btCollisionShape *m_hitTriangleShape;
	int m_hitTriangleIndex;

	FilterClosestRayResultCallback(PHY_IRayCastFilterCallback& phyRayFilter, const btVector3& rayFrom, const btVector3& rayTo)
		:btCollisionWorld::ClosestRayResultCallback(rayFrom, rayTo),
		m_phyRayFilter(phyRayFilter),
		m_hitTriangleShape(nullptr),
		m_hitTriangleIndex(0)
	{
	}

	virtual ~FilterClosestRayResultCallback()
	{
	}

	virtual bool needsCollision(btBroadphaseProxy *proxy0) const
	{
		if (!(proxy0->m_collisionFilterGroup & m_collisionFilterMask)) {
			return false;
		}
		if (!(m_collisionFilterGroup & proxy0->m_collisionFilterMask)) {
			return false;
		}
		btCollisionObject *object = (btCollisionObject *)proxy0->m_clientObject;
		CcdPhysicsController *phyCtrl = static_cast<CcdPhysicsController *>(object->getUserPointer());
		if (phyCtrl == m_phyRayFilter.m_ignoreController) {
			return false;
		}
		return m_phyRayFilter.needBroadphaseRayCast(phyCtrl);
	}

	virtual btScalar addSingleResult(btCollisionWorld::LocalRayResult& rayResult, bool normalInWorldSpace)
	{
		//CcdPhysicsController* curHit = static_cast<CcdPhysicsController*>(rayResult.m_collisionObject->getUserPointer());
		// save shape information as ClosestRayResultCallback::AddSingleResult() does not do it
		if (rayResult.m_localShapeInfo) {
			m_hitTriangleShape = rayResult.m_collisionObject->getCollisionShape();
			m_hitTriangleIndex = rayResult.m_localShapeInfo->m_triangleIndex;
		}
		else {
			m_hitTriangleShape = nullptr;
			m_hitTriangleIndex = 0;
		}
		return ClosestRayResultCallback::addSingleResult(rayResult, normalInWorldSpace);
	}
};

static bool GetHitTriangle(btCollisionShape *shape, CcdShapeConstructionInfo *shapeInfo, int hitTriangleIndex, btVector3 triangle[])
{
	// this code is copied from Bullet
	const unsigned char *vertexbase;
	int numverts;
	PHY_ScalarType type;
	int stride;
	const unsigned char *indexbase;
	int indexstride;
	int numfaces;
	PHY_ScalarType indicestype;
	btStridingMeshInterface *meshInterface = shapeInfo->GetMeshInterface();

	if (!meshInterface) {
		return false;
	}

	meshInterface->getLockedReadOnlyVertexIndexBase(
		&vertexbase,
		numverts,
		type,
		stride,
		&indexbase,
		indexstride,
		numfaces,
		indicestype,
		0);

	unsigned int *gfxbase = (unsigned int *)(indexbase + hitTriangleIndex * indexstride);
	const btVector3& meshScaling = shape->getLocalScaling();
	for (int j = 2; j >= 0; j--) {
		int graphicsindex = (indicestype == PHY_SHORT) ? ((unsigned short *)gfxbase)[j] : gfxbase[j];

		btScalar *graphicsbase = (btScalar *)(vertexbase + graphicsindex * stride);

		triangle[j] = btVector3(graphicsbase[0] * meshScaling.getX(), graphicsbase[1] * meshScaling.getY(), graphicsbase[2] * meshScaling.getZ());
	}
	meshInterface->unLockReadOnlyVertexBase(0);
	return true;
}

PHY_IPhysicsController *CcdPhysicsEnvironment::RayTest(PHY_IRayCastFilterCallback &filterCallback, float fromX, float fromY, float fromZ, float toX, float toY, float toZ)
{
	btVector3 rayFrom(fromX, fromY, fromZ);
	btVector3 rayTo(toX, toY, toZ);

	btVector3 hitPointWorld, normalWorld;

	//Either Ray Cast with or without filtering

	//btCollisionWorld::ClosestRayResultCallback rayCallback(rayFrom,rayTo);
	FilterClosestRayResultCallback rayCallback(filterCallback, rayFrom, rayTo);

	PHY_RayCastResult result;
	memset(&result, 0, sizeof(result));

	// don't collision with sensor object
	rayCallback.m_collisionFilterMask = CcdConstructionInfo::AllFilter ^ CcdConstructionInfo::SensorFilter;
	// Use GJK convex cast instead of the faster Simplex method.
	// kF_UseSubSimplexConvexCastRaytest is faster but numerically unstable for thin/flat
	// convex shapes (e.g. plane objects): the simplex can diverge depending on the
	// orientation of the shape, causing intermittent misses that worsen over time as
	// Bullet's DBVT tree is rebalanced incrementally (btDbvt::optimizeIncremental).
	// kF_UseGjkConvexCastRaytest is slightly slower but gives deterministic, stable
	// results regardless of shape orientation or broadphase tree state.
	rayCallback.m_flags |= btTriangleRaycastCallback::kF_UseGjkConvexCastRaytest;

	m_dynamicsWorld->rayTest(rayFrom, rayTo, rayCallback);
	if (rayCallback.hasHit()) {
		CcdPhysicsController *controller = static_cast<CcdPhysicsController *>(rayCallback.m_collisionObject->getUserPointer());
		result.m_controller = controller;
		result.m_hitPoint[0] = rayCallback.m_hitPointWorld.getX();
		result.m_hitPoint[1] = rayCallback.m_hitPointWorld.getY();
		result.m_hitPoint[2] = rayCallback.m_hitPointWorld.getZ();

		if (rayCallback.m_hitTriangleShape != nullptr) {
			// identify the mesh polygon
			CcdShapeConstructionInfo *shapeInfo = controller->m_shapeInfo;
			if (shapeInfo) {
				btCollisionShape *shape = controller->GetCollisionObject()->getCollisionShape();
				if (shape->isCompound()) {
					btCompoundShape *compoundShape = (btCompoundShape *)shape;
					CcdShapeConstructionInfo *compoundShapeInfo = shapeInfo;
					// need to search which sub-shape has been hit
					for (int i = 0; i < compoundShape->getNumChildShapes(); i++) {
						shapeInfo = compoundShapeInfo->GetChildShape(i);
						shape = compoundShape->getChildShape(i);
						if (shape == rayCallback.m_hitTriangleShape) {
							break;
						}
					}
				}
				if (shape == rayCallback.m_hitTriangleShape &&
				    rayCallback.m_hitTriangleIndex < shapeInfo->m_polygonIndexArray.size()) {
					// save original collision shape triangle for soft body
					int hitTriangleIndex = rayCallback.m_hitTriangleIndex;

					result.m_meshObject = shapeInfo->GetMesh();
					if (shape->isSoftBody()) {
						// soft body using different face numbering because of randomization
						// hopefully we have stored the original face number in m_tag
						const btSoftBody *softBody = static_cast<const btSoftBody *>(rayCallback.m_collisionObject);
						if (softBody->m_faces[hitTriangleIndex].m_tag != 0) {
							rayCallback.m_hitTriangleIndex = (int)((uintptr_t)(softBody->m_faces[hitTriangleIndex].m_tag) - 1);
						}
					}
					// retrieve the original mesh polygon (in case of quad->tri conversion)
					result.m_polygon = shapeInfo->m_polygonIndexArray[rayCallback.m_hitTriangleIndex];
					// hit triangle in world coordinate, for face normal and UV coordinate
					btVector3 triangle[3];
					bool triangleOK = false;
					if (filterCallback.m_faceUV && (3 * rayCallback.m_hitTriangleIndex) < shapeInfo->m_triFaceUVcoArray.size()) {
						// interpolate the UV coordinate of the hit point
						CcdShapeConstructionInfo::UVco *uvCo = &shapeInfo->m_triFaceUVcoArray[3 * rayCallback.m_hitTriangleIndex];
						// 1. get the 3 coordinate of the triangle in world space
						btVector3 v1, v2, v3;
						if (shape->isSoftBody()) {
							// soft body give points directly in world coordinate
							const btSoftBody *softBody = static_cast<const btSoftBody *>(rayCallback.m_collisionObject);
							v1 = softBody->m_faces[hitTriangleIndex].m_n[0]->m_x;
							v2 = softBody->m_faces[hitTriangleIndex].m_n[1]->m_x;
							v3 = softBody->m_faces[hitTriangleIndex].m_n[2]->m_x;
						}
						else {
							// for rigid body we must apply the world transform
							triangleOK = GetHitTriangle(shape, shapeInfo, hitTriangleIndex, triangle);
							if (!triangleOK) {
								// if we cannot get the triangle, no use to continue
								goto SKIP_UV_NORMAL;
							}
							v1 = rayCallback.m_collisionObject->getWorldTransform()(triangle[0]);
							v2 = rayCallback.m_collisionObject->getWorldTransform()(triangle[1]);
							v3 = rayCallback.m_collisionObject->getWorldTransform()(triangle[2]);
						}
						// 2. compute barycentric coordinate of the hit point
						btVector3 v = v2 - v1;
						btVector3 w = v3 - v1;
						btVector3 u = v.cross(w);
						btScalar A = u.length();

						v = v2 - rayCallback.m_hitPointWorld;
						w = v3 - rayCallback.m_hitPointWorld;
						u = v.cross(w);
						btScalar A1 = u.length();

						v = rayCallback.m_hitPointWorld - v1;
						w = v3 - v1;
						u = v.cross(w);
						btScalar A2 = u.length();

						btVector3 baryCo;
						baryCo.setX(A1 / A);
						baryCo.setY(A2 / A);
						baryCo.setZ(1.0f - baryCo.getX() - baryCo.getY());
						// 3. compute UV coordinate
						result.m_hitUV[0] = baryCo.getX() * uvCo[0].uv[0] + baryCo.getY() * uvCo[1].uv[0] + baryCo.getZ() * uvCo[2].uv[0];
						result.m_hitUV[1] = baryCo.getX() * uvCo[0].uv[1] + baryCo.getY() * uvCo[1].uv[1] + baryCo.getZ() * uvCo[2].uv[1];
						result.m_hitUVOK = 1;
					}

					// Bullet returns the normal from "outside".
					// If the user requests the real normal, compute it now
					if (filterCallback.m_faceNormal) {
						if (shape->isSoftBody()) {
							// we can get the real normal directly from the body
							const btSoftBody *softBody = static_cast<const btSoftBody *>(rayCallback.m_collisionObject);
							rayCallback.m_hitNormalWorld = softBody->m_faces[hitTriangleIndex].m_normal;
						}
						else {
							if (!triangleOK) {
								triangleOK = GetHitTriangle(shape, shapeInfo, hitTriangleIndex, triangle);
							}
							if (triangleOK) {
								btVector3 triangleNormal;
								triangleNormal = (triangle[1] - triangle[0]).cross(triangle[2] - triangle[0]);
								rayCallback.m_hitNormalWorld = rayCallback.m_collisionObject->getWorldTransform().getBasis() * triangleNormal;
							}
						}
					}
SKIP_UV_NORMAL:
					;
				}
			}
		}
		if (rayCallback.m_hitNormalWorld.length2() > (SIMD_EPSILON * SIMD_EPSILON)) {
			rayCallback.m_hitNormalWorld.normalize();
		}
		else {
			rayCallback.m_hitNormalWorld.setValue(1.0f, 0.0f, 0.0f);
		}
		result.m_hitNormal[0] = rayCallback.m_hitNormalWorld.getX();
		result.m_hitNormal[1] = rayCallback.m_hitNormalWorld.getY();
		result.m_hitNormal[2] = rayCallback.m_hitNormalWorld.getZ();
		filterCallback.reportHit(&result);
	}

	return result.m_controller;
}

// Handles occlusion culling.
// The implementation is based on the CDTestFramework
struct OcclusionBuffer {
	struct WriteOCL {
		static inline bool Process(btScalar &q, btScalar v)
		{
			if (q < v) {
				q = v;
			}
			return false;
		}
		static inline void Occlusion(bool &flag)
		{
			flag = true;
		}
	};

	struct QueryOCL {
		static inline bool Process(btScalar &q, btScalar v)
		{
			return (q <= v);
		}
		static inline void Occlusion(bool &flag)
		{
		}
	};

	btScalar *m_buffer;
	size_t m_bufferSize;
	bool m_initialized;
	bool m_occlusion;
	int m_sizes[2];
	btScalar m_scales[2];
	btScalar m_offsets[2];
	btScalar m_wtc[16]; // world to clip transform
	btScalar m_mtc[16]; // model to clip transform
	std::vector<btScalar> m_hzb;
	std::vector<int> m_hzbLevelOffsets;
	std::vector<int> m_hzbWidths;
	std::vector<int> m_hzbHeights;
	// constructor: size=largest dimension of the buffer.
	// Buffer size depends on aspect ratio
	OcclusionBuffer()
	{
		m_initialized = false;
		m_occlusion = false;
		m_buffer = nullptr;
		m_bufferSize = 0;
	}

	void BuildHzb()
	{
		m_hzb.clear();
		m_hzbLevelOffsets.clear();
		m_hzbWidths.clear();
		m_hzbHeights.clear();

		if (!m_buffer || !m_occlusion || m_sizes[0] <= 0 || m_sizes[1] <= 0) {
			return;
		}

		const btScalar *prev = m_buffer;
		int prevW = m_sizes[0];
		int prevH = m_sizes[1];

		while (prevW > 1 || prevH > 1) {
			const int w = (prevW + 1) / 2;
			const int h = (prevH + 1) / 2;
			const int offset = (int)m_hzb.size();
			m_hzbLevelOffsets.push_back(offset);
			m_hzbWidths.push_back(w);
			m_hzbHeights.push_back(h);
			m_hzb.resize(m_hzb.size() + (size_t)w * (size_t)h);
			btScalar *dst = m_hzb.data() + offset;

			for (int y = 0; y < h; ++y) {
				for (int x = 0; x < w; ++x) {
					btScalar m = btScalar(1e30f);
					for (int dy = 0; dy < 2; ++dy) {
						const int sy = y * 2 + dy;
						if (sy >= prevH) {
							continue;
						}
						for (int dx = 0; dx < 2; ++dx) {
							const int sx = x * 2 + dx;
							if (sx >= prevW) {
								continue;
							}
							m = btMin(m, prev[sy * prevW + sx]);
						}
					}
					dst[y * w + x] = m;
				}
			}

			prev = dst;
			prevW = w;
			prevH = h;
		}
	}
	// multiplication of column major matrices: m = m1 * m2
	template<typename T1, typename T2>
	void CMmat4mul(btScalar *m, const T1 *m1, const T2 *m2)
	{
		m[0] = btScalar(m1[0] * m2[0] + m1[4] * m2[1] + m1[8] * m2[2] + m1[12] * m2[3]);
		m[1] = btScalar(m1[1] * m2[0] + m1[5] * m2[1] + m1[9] * m2[2] + m1[13] * m2[3]);
		m[2] = btScalar(m1[2] * m2[0] + m1[6] * m2[1] + m1[10] * m2[2] + m1[14] * m2[3]);
		m[3] = btScalar(m1[3] * m2[0] + m1[7] * m2[1] + m1[11] * m2[2] + m1[15] * m2[3]);

		m[4] = btScalar(m1[0] * m2[4] + m1[4] * m2[5] + m1[8] * m2[6] + m1[12] * m2[7]);
		m[5] = btScalar(m1[1] * m2[4] + m1[5] * m2[5] + m1[9] * m2[6] + m1[13] * m2[7]);
		m[6] = btScalar(m1[2] * m2[4] + m1[6] * m2[5] + m1[10] * m2[6] + m1[14] * m2[7]);
		m[7] = btScalar(m1[3] * m2[4] + m1[7] * m2[5] + m1[11] * m2[6] + m1[15] * m2[7]);

		m[8] = btScalar(m1[0] * m2[8] + m1[4] * m2[9] + m1[8] * m2[10] + m1[12] * m2[11]);
		m[9] = btScalar(m1[1] * m2[8] + m1[5] * m2[9] + m1[9] * m2[10] + m1[13] * m2[11]);
		m[10] = btScalar(m1[2] * m2[8] + m1[6] * m2[9] + m1[10] * m2[10] + m1[14] * m2[11]);
		m[11] = btScalar(m1[3] * m2[8] + m1[7] * m2[9] + m1[11] * m2[10] + m1[15] * m2[11]);

		m[12] = btScalar(m1[0] * m2[12] + m1[4] * m2[13] + m1[8] * m2[14] + m1[12] * m2[15]);
		m[13] = btScalar(m1[1] * m2[12] + m1[5] * m2[13] + m1[9] * m2[14] + m1[13] * m2[15]);
		m[14] = btScalar(m1[2] * m2[12] + m1[6] * m2[13] + m1[10] * m2[14] + m1[14] * m2[15]);
		m[15] = btScalar(m1[3] * m2[12] + m1[7] * m2[13] + m1[11] * m2[14] + m1[15] * m2[15]);
	}

	void setup(int size, const int *view, float mat[16])
	{
		m_initialized = false;
		m_occlusion = false;
		// compute the size of the buffer
		int maxsize = (view[2] > view[3]) ? view[2] : view[3];
		BLI_assert(maxsize > 0);
		double ratio = 1.0 / (2 * maxsize);
		// ensure even number
		m_sizes[0] = 2 * ((int)(size * view[2] * ratio + 0.5));
		m_sizes[1] = 2 * ((int)(size * view[3] * ratio + 0.5));
		m_scales[0] = btScalar(m_sizes[0] / 2);
		m_scales[1] = btScalar(m_sizes[1] / 2);
		m_offsets[0] = m_scales[0] + 0.5f;
		m_offsets[1] = m_scales[1] + 0.5f;
		// prepare matrix
		// at this time of the rendering, the modelview matrix is the
		// world to camera transformation and the projection matrix is
		// camera to clip transformation. combine both so that
		for (unsigned short i = 0; i < 16; i++) {
			m_wtc[i] = btScalar(mat[i]);
		}
	}

	void initialize()
	{
		size_t newsize = (m_sizes[0] * m_sizes[1]) * sizeof(btScalar);
		if (m_buffer) {
			// see if we can reuse
			if (newsize > m_bufferSize) {
				free(m_buffer);
				m_buffer = nullptr;
				m_bufferSize = 0;
			}
		}
		if (!m_buffer) {
			m_buffer = (btScalar *)calloc(1, newsize);
			m_bufferSize = newsize;
		}
		else {
			// buffer exists already, just clears it
			memset(m_buffer, 0, newsize);
		}
		// memory allocate must succeed
		BLI_assert(m_buffer != nullptr);
		m_initialized = true;
		m_occlusion = false;
	}

	void SetModelMatrix(float *fl)
	{
		CMmat4mul(m_mtc, m_wtc, fl);
		if (!m_initialized) {
			initialize();
		}
	}

	// transform a segment in world coordinate to clip coordinate
	void transformW(const btVector3 &x, btVector4 &t)
	{
		t[0] = x[0] * m_wtc[0] + x[1] * m_wtc[4] + x[2] * m_wtc[8] + m_wtc[12];
		t[1] = x[0] * m_wtc[1] + x[1] * m_wtc[5] + x[2] * m_wtc[9] + m_wtc[13];
		t[2] = x[0] * m_wtc[2] + x[1] * m_wtc[6] + x[2] * m_wtc[10] + m_wtc[14];
		t[3] = x[0] * m_wtc[3] + x[1] * m_wtc[7] + x[2] * m_wtc[11] + m_wtc[15];
	}

	void transformM(const float *x, btVector4 &t)
	{
		t[0] = x[0] * m_mtc[0] + x[1] * m_mtc[4] + x[2] * m_mtc[8] + m_mtc[12];
		t[1] = x[0] * m_mtc[1] + x[1] * m_mtc[5] + x[2] * m_mtc[9] + m_mtc[13];
		t[2] = x[0] * m_mtc[2] + x[1] * m_mtc[6] + x[2] * m_mtc[10] + m_mtc[14];
		t[3] = x[0] * m_mtc[3] + x[1] * m_mtc[7] + x[2] * m_mtc[11] + m_mtc[15];
	}
	// convert polygon to device coordinates
	static bool project(btVector4 *p, int n)
	{
		for (int i = 0; i < n; ++i) {
			p[i][2] = 1 / p[i][3];
			p[i][0] *= p[i][2];
			p[i][1] *= p[i][2];
		}
		return true;
	}
	// pi: closed polygon in clip coordinate, NP = number of segments
	// po: same polygon with clipped segments removed
	template <const int NP>
	static int clip(const btVector4 *pi, btVector4 *po)
	{
		btScalar s[2 * NP];
		btVector4 pn[2 * NP];
		int i, j, m, n, ni;
		// deal with near clipping
		for (i = 0, m = 0; i < NP; ++i) {
			s[i] = pi[i][2] + pi[i][3];
			if (s[i] < 0) {
				m += 1 << i;
			}
		}
		if (m == ((1 << NP) - 1)) {
			return 0;
		}
		if (m != 0) {
			for (i = NP - 1, j = 0, n = 0; j < NP; i = j++) {
				const btVector4 &a = pi[i];
				const btVector4 &b = pi[j];
				const btScalar t = s[i] / (a[3] + a[2] - b[3] - b[2]);
				if ((t > 0) && (t < 1)) {
					pn[n][0] = a[0] + (b[0] - a[0]) * t;
					pn[n][1] = a[1] + (b[1] - a[1]) * t;
					pn[n][2] = a[2] + (b[2] - a[2]) * t;
					pn[n][3] = a[3] + (b[3] - a[3]) * t;
					++n;
				}
				if (s[j] > 0) {
					pn[n++] = b;
				}
			}
			// ready to test far clipping, start from the modified polygon
			pi = pn;
			ni = n;
		}
		else {
			// no clipping on the near plane, keep same vector
			ni = NP;
		}
		// now deal with far clipping
		for (i = 0, m = 0; i < ni; ++i) {
			s[i] = pi[i][2] - pi[i][3];
			if (s[i] > 0) {
				m += 1 << i;
			}
		}
		if (m == ((1 << ni) - 1)) {
			return 0;
		}
		if (m != 0) {
			for (i = ni - 1, j = 0, n = 0; j < ni; i = j++) {
				const btVector4 &a = pi[i];
				const btVector4 &b = pi[j];
				const btScalar t = s[i] / (a[2] - a[3] - b[2] + b[3]);
				if ((t > 0) && (t < 1)) {
					po[n][0] = a[0] + (b[0] - a[0]) * t;
					po[n][1] = a[1] + (b[1] - a[1]) * t;
					po[n][2] = a[2] + (b[2] - a[2]) * t;
					po[n][3] = a[3] + (b[3] - a[3]) * t;
					++n;
				}
				if (s[j] < 0) {
					po[n++] = b;
				}
			}
			return n;
		}
		for (int i = 0; i < ni; ++i) {
			po[i] = pi[i];
		}
		return ni;
	}
	// write or check a triangle to buffer. a,b,c in device coordinates (-1,+1)
	template <typename POLICY>
	inline bool draw(const btVector4 &a,
	                 const btVector4 &b,
	                 const btVector4 &c,
	                 const float face,
	                 const btScalar minarea)
	{
		const btScalar a2 = btCross(b - a, c - a)[2];
		if ((face * a2) < 0.0f || btFabs(a2) < minarea) {
			return false;
		}
		// further down we are normally going to write to the Zbuffer, mark it so
		POLICY::Occlusion(m_occlusion);

		int x[3], y[3], ib = 1, ic = 2;
		btScalar z[3];
		x[0] = (int)(a.x() * m_scales[0] + m_offsets[0]);
		y[0] = (int)(a.y() * m_scales[1] + m_offsets[1]);
		z[0] = a.z();
		if (a2 < 0.f) {
			// negative aire is possible with double face => must
			// change the order of b and c otherwise the algorithm doesn't work
			ib = 2;
			ic = 1;
		}
		x[ib] = (int)(b.x() * m_scales[0] + m_offsets[0]);
		x[ic] = (int)(c.x() * m_scales[0] + m_offsets[0]);
		y[ib] = (int)(b.y() * m_scales[1] + m_offsets[1]);
		y[ic] = (int)(c.y() * m_scales[1] + m_offsets[1]);
		z[ib] = b.z();
		z[ic] = c.z();
		const int mix = btMax(0, btMin(x[0], btMin(x[1], x[2])));
		const int mxx = btMin(m_sizes[0], 1 + btMax(x[0], btMax(x[1], x[2])));
		const int miy = btMax(0, btMin(y[0], btMin(y[1], y[2])));
		const int mxy = btMin(m_sizes[1], 1 + btMax(y[0], btMax(y[1], y[2])));
		const int width = mxx - mix;
		const int height = mxy - miy;
		if ((width * height) <= 1) {
			// degenerated in at most one single pixel
			btScalar *scan = &m_buffer[miy * m_sizes[0] + mix];
			// use for loop to detect the case where width or height == 0
			for (int iy = miy; iy < mxy; ++iy) {
				for (int ix = mix; ix < mxx; ++ix) {
					if (POLICY::Process(*scan, z[0])) {
						return true;
					}
					if (POLICY::Process(*scan, z[1])) {
						return true;
					}
					if (POLICY::Process(*scan, z[2])) {
						return true;
					}
				}
			}
		}
		else if (width == 1) {
			// Degenerated in at least 2 vertical lines
			// The algorithm below doesn't work when face has a single pixel width
			// We cannot use general formulas because the plane is degenerated.
			// We have to interpolate along the 3 edges that overlaps and process each pixel.
			// sort the y coord to make formula simpler
			int ytmp;
			btScalar ztmp;
			if (y[0] > y[1]) {
				ytmp = y[1];
				y[1] = y[0];
				y[0] = ytmp;
				ztmp = z[1];
				z[1] = z[0];
				z[0] = ztmp;
			}
			if (y[0] > y[2]) {
				ytmp = y[2];
				y[2] = y[0];
				y[0] = ytmp;
				ztmp = z[2];
				z[2] = z[0];
				z[0] = ztmp;
			}
			if (y[1] > y[2]) {
				ytmp = y[2];
				y[2] = y[1];
				y[1] = ytmp;
				ztmp = z[2];
				z[2] = z[1];
				z[1] = ztmp;
			}
			int dy[] = {y[0] - y[1],
				        y[1] - y[2],
				        y[2] - y[0]};
			btScalar dzy[3];
			dzy[0] = (dy[0]) ? (z[0] - z[1]) / dy[0] : btScalar(0.0f);
			dzy[1] = (dy[1]) ? (z[1] - z[2]) / dy[1] : btScalar(0.0f);
			dzy[2] = (dy[2]) ? (z[2] - z[0]) / dy[2] : btScalar(0.0f);
			btScalar v[3] = {dzy[0] * (miy - y[0]) + z[0],
				             dzy[1] * (miy - y[1]) + z[1],
				             dzy[2] * (miy - y[2]) + z[2]};
			dy[0] = y[1] - y[0];
			dy[1] = y[0] - y[1];
			dy[2] = y[2] - y[0];
			btScalar *scan = &m_buffer[miy * m_sizes[0] + mix];
			for (int iy = miy; iy < mxy; ++iy) {
				if (dy[0] >= 0 && POLICY::Process(*scan, v[0])) {
					return true;
				}
				if (dy[1] >= 0 && POLICY::Process(*scan, v[1])) {
					return true;
				}
				if (dy[2] >= 0 && POLICY::Process(*scan, v[2])) {
					return true;
				}
				scan += m_sizes[0];
				v[0] += dzy[0];
				v[1] += dzy[1];
				v[2] += dzy[2];
				dy[0]--;
				dy[1]++;
				dy[2]--;
			}
		}
		else if (height == 1) {
			// Degenerated in at least 2 horizontal lines
			// The algorithm below doesn't work when face has a single pixel width
			// We cannot use general formulas because the plane is degenerated.
			// We have to interpolate along the 3 edges that overlaps and process each pixel.
			int xtmp;
			btScalar ztmp;
			if (x[0] > x[1]) {
				xtmp = x[1];
				x[1] = x[0];
				x[0] = xtmp;
				ztmp = z[1];
				z[1] = z[0];
				z[0] = ztmp;
			}
			if (x[0] > x[2]) {
				xtmp = x[2];
				x[2] = x[0];
				x[0] = xtmp;
				ztmp = z[2];
				z[2] = z[0];
				z[0] = ztmp;
			}
			if (x[1] > x[2]) {
				xtmp = x[2];
				x[2] = x[1];
				x[1] = xtmp;
				ztmp = z[2];
				z[2] = z[1];
				z[1] = ztmp;
			}
			int dx[] = {x[0] - x[1],
				        x[1] - x[2],
				        x[2] - x[0]};
			btScalar dzx[3];
			dzx[0] = (dx[0]) ? (z[0] - z[1]) / dx[0] : btScalar(0.0f);
			dzx[1] = (dx[1]) ? (z[1] - z[2]) / dx[1] : btScalar(0.0f);
			dzx[2] = (dx[2]) ? (z[2] - z[0]) / dx[2] : btScalar(0.0f);
			btScalar v[3] = {dzx[0] * (mix - x[0]) + z[0],
				             dzx[1] * (mix - x[1]) + z[1],
				             dzx[2] * (mix - x[2]) + z[2]};
			dx[0] = x[1] - x[0];
			dx[1] = x[0] - x[1];
			dx[2] = x[2] - x[0];
			btScalar *scan = &m_buffer[miy * m_sizes[0] + mix];
			for (int ix = mix; ix < mxx; ++ix) {
				if (dx[0] >= 0 && POLICY::Process(*scan, v[0])) {
					return true;
				}
				if (dx[1] >= 0 && POLICY::Process(*scan, v[1])) {
					return true;
				}
				if (dx[2] >= 0 && POLICY::Process(*scan, v[2])) {
					return true;
				}
				scan++;
				v[0] += dzx[0];
				v[1] += dzx[1];
				v[2] += dzx[2];
				dx[0]--;
				dx[1]++;
				dx[2]--;
			}
		}
		else {
			// general case
			const int dx[] = {y[0] - y[1],
				              y[1] - y[2],
				              y[2] - y[0]};
			const int dy[] = {x[1] - x[0] - dx[0] * width,
				              x[2] - x[1] - dx[1] * width,
				              x[0] - x[2] - dx[2] * width};
			const int a = x[2] * y[0] + x[0] * y[1] - x[2] * y[1] - x[0] * y[2] + x[1] * y[2] - x[1] * y[0];
			const btScalar ia = 1 / (btScalar)a;
			const btScalar dzx = ia * (y[2] * (z[1] - z[0]) + y[1] * (z[0] - z[2]) + y[0] * (z[2] - z[1]));
			const btScalar dzy = ia * (x[2] * (z[0] - z[1]) + x[0] * (z[1] - z[2]) + x[1] * (z[2] - z[0])) - (dzx * width);
			int c[] = {miy *x[1] + mix * y[0] - x[1] * y[0] - mix * y[1] + x[0] * y[1] - miy * x[0],
				miy *x[2] + mix * y[1] - x[2] * y[1] - mix * y[2] + x[1] * y[2] - miy * x[1],
				miy *x[0] + mix * y[2] - x[0] * y[2] - mix * y[0] + x[2] * y[0] - miy * x[2]};
			btScalar v = ia * ((z[2] * c[0]) + (z[0] * c[1]) + (z[1] * c[2]));
			btScalar *scan = &m_buffer[miy * m_sizes[0]];

			for (int iy = miy; iy < mxy; ++iy) {
				for (int ix = mix; ix < mxx; ++ix) {
					if ((c[0] >= 0) && (c[1] >= 0) && (c[2] >= 0)) {
						if (POLICY::Process(scan[ix], v)) {
							return true;
						}
					}
					c[0] += dx[0]; c[1] += dx[1]; c[2] += dx[2]; v += dzx;
				}
				c[0] += dy[0]; c[1] += dy[1]; c[2] += dy[2]; v += dzy;
				scan += m_sizes[0];
			}
		}
		return false;
	}
	// clip than write or check a polygon
	template <const int NP, typename POLICY>
	inline bool clipDraw(const btVector4 *p,
	                     const float face,
	                     btScalar minarea)
	{
		btVector4 o[NP * 2];
		int n = clip<NP>(p, o);
		bool earlyexit = false;
		if (n) {
			project(o, n);
			for (int i = 2; i < n && !earlyexit; ++i) {
				earlyexit |= draw<POLICY>(o[0], o[i - 1], o[i], face, minarea);
			}
		}
		return earlyexit;
	}
	// add a triangle (in model coordinate)
	// face =  0.f if face is double side,
	//      =  1.f if face is single sided and scale is positive
	//      = -1.f if face is single sided and scale is negative
	void appendOccluderM(const float *a,
	                     const float *b,
	                     const float *c,
	                     const float face)
	{
		btVector4 p[3];
		transformM(a, p[0]);
		transformM(b, p[1]);
		transformM(c, p[2]);
		clipDraw<3, WriteOCL>(p, face, btScalar(0.0f));
	}

	void appendOccluderW(const float *a,
	                     const float *b,
	                     const float *c,
	                     const float face)
	{
		btVector4 p[3];
		transformW(btVector3(a[0], a[1], a[2]), p[0]);
		transformW(btVector3(b[0], b[1], b[2]), p[1]);
		transformW(btVector3(c[0], c[1], c[2]), p[2]);
		clipDraw<3, WriteOCL>(p, face, btScalar(0.0f));
	}

	// query occluder for a box (c=center, e=extend) in world coordinate
	inline bool queryOccluderW(const btVector3 &c,
	                           const btVector3 &e)
	{
		if (!m_occlusion) {
			// no occlusion yet, no need to check
			return true;
		}

		if (!m_hzbWidths.empty() && m_buffer) {
			btVector4 x[8];
			transformW(btVector3(c[0] - e[0], c[1] - e[1], c[2] - e[2]), x[0]);
			transformW(btVector3(c[0] + e[0], c[1] - e[1], c[2] - e[2]), x[1]);
			transformW(btVector3(c[0] + e[0], c[1] + e[1], c[2] - e[2]), x[2]);
			transformW(btVector3(c[0] - e[0], c[1] + e[1], c[2] - e[2]), x[3]);
			transformW(btVector3(c[0] - e[0], c[1] - e[1], c[2] + e[2]), x[4]);
			transformW(btVector3(c[0] + e[0], c[1] - e[1], c[2] + e[2]), x[5]);
			transformW(btVector3(c[0] + e[0], c[1] + e[1], c[2] + e[2]), x[6]);
			transformW(btVector3(c[0] - e[0], c[1] + e[1], c[2] + e[2]), x[7]);

			for (int i = 0; i < 8; ++i) {
				if ((x[i][2] + x[i][3]) <= 0) {
					return true;
				}
			}

			int minX = m_sizes[0];
			int minY = m_sizes[1];
			int maxX = -1;
			int maxY = -1;
			btScalar vMax = btScalar(0);
			bool any = false;

			for (int i = 0; i < 8; ++i) {
				const btScalar w = x[i][3];
				if (w <= btScalar(1e-6f)) {
					continue;
				}
				const btScalar invW = btScalar(1) / w;
				const btScalar ndcX = x[i][0] * invW;
				const btScalar ndcY = x[i][1] * invW;
				const int sx = (int)(ndcX * m_scales[0] + m_offsets[0]);
				const int sy = (int)(ndcY * m_scales[1] + m_offsets[1]);
				minX = btMin(minX, sx);
				maxX = btMax(maxX, sx);
				minY = btMin(minY, sy);
				maxY = btMax(maxY, sy);
				vMax = btMax(vMax, invW);
				any = true;
			}

			if (!any) {
				return true;
			}

			minX = btMax(0, minX);
			minY = btMax(0, minY);
			maxX = btMin(m_sizes[0] - 1, maxX);
			maxY = btMin(m_sizes[1] - 1, maxY);
			if (minX >= maxX || minY >= maxY) {
				return true;
			}

			const int rx0 = minX;
			const int ry0 = minY;
			const int rx1 = maxX + 1;
			const int ry1 = maxY + 1;
			const int rectW = rx1 - rx0;
			const int rectH = ry1 - ry0;
			const int maxLevel = (int)m_hzbWidths.size();
			int level = 0;
			while (level < maxLevel) {
				const int covW = (rectW + ((1 << level) - 1)) >> level;
				const int covH = (rectH + ((1 << level) - 1)) >> level;
				if (covW <= 2 && covH <= 2) {
					break;
				}
				++level;
			}

			const btScalar *data = nullptr;
			int w = 0;
			int h = 0;
			if (level == 0) {
				data = m_buffer;
				w = m_sizes[0];
				h = m_sizes[1];
			}
			else {
				data = m_hzb.data() + m_hzbLevelOffsets[level - 1];
				w = m_hzbWidths[level - 1];
				h = m_hzbHeights[level - 1];
			}

			int sx0 = rx0 >> level;
			int sy0 = ry0 >> level;
			int sx1 = (rx1 - 1) >> level;
			int sy1 = (ry1 - 1) >> level;
			sx0 = btMax(0, btMin(w - 1, sx0));
			sy0 = btMax(0, btMin(h - 1, sy0));
			sx1 = btMax(0, btMin(w - 1, sx1));
			sy1 = btMax(0, btMin(h - 1, sy1));

			btScalar minQ = btScalar(1e30f);
			for (int y = sy0; y <= sy1; ++y) {
				for (int x = sx0; x <= sx1; ++x) {
					minQ = btMin(minQ, data[y * w + x]);
				}
			}

			return (minQ <= vMax);
		}

		btVector4 x[8];
		transformW(btVector3(c[0] - e[0], c[1] - e[1], c[2] - e[2]), x[0]);
		transformW(btVector3(c[0] + e[0], c[1] - e[1], c[2] - e[2]), x[1]);
		transformW(btVector3(c[0] + e[0], c[1] + e[1], c[2] - e[2]), x[2]);
		transformW(btVector3(c[0] - e[0], c[1] + e[1], c[2] - e[2]), x[3]);
		transformW(btVector3(c[0] - e[0], c[1] - e[1], c[2] + e[2]), x[4]);
		transformW(btVector3(c[0] + e[0], c[1] - e[1], c[2] + e[2]), x[5]);
		transformW(btVector3(c[0] + e[0], c[1] + e[1], c[2] + e[2]), x[6]);
		transformW(btVector3(c[0] - e[0], c[1] + e[1], c[2] + e[2]), x[7]);

		for (int i = 0; i < 8; ++i) {
			// the box is clipped, it's probably a large box, don't waste our time to check
			if ((x[i][2] + x[i][3]) <= 0) {
				return true;
			}
		}
		static const int d[] = {1, 0, 3, 2,
			                    4, 5, 6, 7,
			                    4, 7, 3, 0,
			                    6, 5, 1, 2,
			                    7, 6, 2, 3,
			                    5, 4, 0, 1};
		for (unsigned int i = 0; i < (sizeof(d) / sizeof(d[0])); ) {
			const btVector4 p[] = {x[d[i + 0]],
				                   x[d[i + 1]],
				                   x[d[i + 2]],
				                   x[d[i + 3]]};
			i += 4;
			if (clipDraw<4, QueryOCL>(p, 1.0f, 0.0f)) {
				return true;
			}
		}
		return false;
	}
};


struct  DbvtCullingCallback : btDbvt::ICollide {
	PHY_CullingCallback m_clientCallback;
	void *m_userData;
	OcclusionBuffer *m_ocb;

	DbvtCullingCallback(PHY_CullingCallback clientCallback, void *userData)
	{
		m_clientCallback = clientCallback;
		m_userData = userData;
		m_ocb = nullptr;
	}
	bool Descent(const btDbvtNode *node)
	{
		return (m_ocb->queryOccluderW(node->volume.Center(), node->volume.Extents()));
	}
	void Process(const btDbvtNode *node, btScalar depth)
	{
		Process(node);
	}
	void Process(const btDbvtNode *leaf)
	{
		btBroadphaseProxy *proxy = (btBroadphaseProxy *)leaf->data;
		// the client object is a graphic controller
		CcdGraphicController *ctrl = static_cast<CcdGraphicController *>(proxy->m_clientObject);
		KX_ClientObjectInfo *info = (KX_ClientObjectInfo *)ctrl->GetNewClientInfo();
		if (m_ocb) {
			// means we are doing occlusion culling. Check if this object is an occluders
			KX_GameObject *gameobj = KX_GameObject::GetClientObject(info);
			if (gameobj && gameobj->GetOccluder()) {
				float fl[16];
				gameobj->NodeGetWorldTransform().PackFromAffineTransform(fl);

				// this will create the occlusion buffer if not already done
				// and compute the transformation from model local space to clip space
				m_ocb->SetModelMatrix(fl);
				const float negative = gameobj->IsNegativeScaling();
				// walk through the meshes and for each add to buffer
				for (KX_Mesh *meshobj : gameobj->GetMeshList()) {
					for (RAS_MeshMaterial *meshmat : meshobj->GetMeshMaterialList()) {
						RAS_DisplayArray *array = meshmat->GetDisplayArray();
						const bool twoside = meshmat->GetBucket()->GetMaterial()->IsTwoSided();
						const float face = (twoside) ? 0.0f : ((negative) ? -1.0f : 1.0f);

						const unsigned int triCount = array->GetTriangleIndexCount();
						const unsigned int *triIndices = array->GetTriangleIndicesData();
						const mt::vec3_packed *positions = array->GetPositionsData();
						for (unsigned int j = 0; j < triCount; j += 3) {
							const unsigned int i0 = triIndices[j];
							const unsigned int i1 = triIndices[j + 1];
							const unsigned int i2 = triIndices[j + 2];
							m_ocb->appendOccluderM(positions[i0].data,
							                       positions[i1].data,
							                       positions[i2].data,
							                       face);
						}
					}
				}
			}
		}
		if (info) {
			(*m_clientCallback)(info, m_userData);
		}
	}
};

static OcclusionBuffer gOcb;
static OcclusionBuffer gOcbGpu;

#ifdef WITH_PYTHON
namespace {
static bool ComputeOcclusionParams(int occlusionRes,
                                   const int *viewport,
                                   const mt::mat4& matrix,
                                   int &outWidth,
                                   int &outHeight,
                                   btScalar outWtc[16],
                                   int outSizes[2],
                                   btScalar outScales[2],
                                   btScalar outOffsets[2])
{
	if (occlusionRes <= 0 || !viewport) {
		return false;
	}

	const int maxsize = (viewport[2] > viewport[3]) ? viewport[2] : viewport[3];
	if (maxsize <= 0) {
		return false;
	}

	const double ratio = 1.0 / (2.0 * (double)maxsize);
	outSizes[0] = 2 * ((int)(occlusionRes * viewport[2] * ratio + 0.5));
	outSizes[1] = 2 * ((int)(occlusionRes * viewport[3] * ratio + 0.5));
	outScales[0] = btScalar(outSizes[0] / 2);
	outScales[1] = btScalar(outSizes[1] / 2);
	outOffsets[0] = outScales[0] + btScalar(0.5f);
	outOffsets[1] = outScales[1] + btScalar(0.5f);

	const float *mat = (const float *)matrix.Data();
	for (int i = 0; i < 16; ++i) {
		outWtc[i] = btScalar(mat[i]);
	}

	outWidth = outSizes[0];
	outHeight = outSizes[1];
	return (outWidth > 0 && outHeight > 0);
}

struct GlInvWOcclusionSlot {
	GLuint pbo = 0;
	GLsync fence = nullptr;
	void *mapped = nullptr;
	size_t byteSize = 0;
	int width = 0;
	int height = 0;
	btScalar wtc[16] = {};
	int sizes[2] = {};
	btScalar scales[2] = {};
	btScalar offsets[2] = {};
	bool occlusion = false;
	// Cópia local do buffer para o path sem GL_ARB_buffer_storage
	std::vector<btScalar> localBuffer;
};

struct GlInvWOcclusionBackend {
	static constexpr int kRingSize = 3;
	GlInvWOcclusionSlot slots[kRingSize];
	int writeIndex = 0;
	int lastPublishedIndex = -1;
	bool published = false;

	GLuint fbo = 0;
	GLuint tex = 0;
	int texWidth = 0;
	int texHeight = 0;

	GLuint vao = 0;
	GLuint vbo = 0;
	size_t vboCapacity = 0;
	void *vboMapped = nullptr;
	uint32_t uploadedOccludersRevision = 0;

	GLuint program = 0;
	GLuint vs = 0;
	GLuint fs = 0;
	GLint uViewProj = -1;

	bool supportsPersistent = false;

	bool EnsureProgram()
	{
		if (program) {
			return true;
		}

		const char *vsSrc =
			"#version 330 core\n"
			"layout(location=0) in vec3 aPos;\n"
			"uniform mat4 uViewProj;\n"
			"void main(){gl_Position=uViewProj*vec4(aPos,1.0);}\n";

		const char *fsSrc =
			"#version 330 core\n"
			"layout(location=0) out float outInvW;\n"
			"void main(){outInvW=gl_FragCoord.w;}\n";

		auto compile = [](GLenum type, const char *src) -> GLuint {
			GLuint sh = glCreateShader(type);
			glShaderSource(sh, 1, &src, nullptr);
			glCompileShader(sh);
			GLint ok = 0;
			glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
			if (!ok) {
				glDeleteShader(sh);
				return 0;
			}
			return sh;
		};

		vs = compile(GL_VERTEX_SHADER, vsSrc);
		if (!vs) {
			return false;
		}
		fs = compile(GL_FRAGMENT_SHADER, fsSrc);
		if (!fs) {
			glDeleteShader(vs);
			vs = 0;
			return false;
		}

		program = glCreateProgram();
		glAttachShader(program, vs);
		glAttachShader(program, fs);
		glLinkProgram(program);
		GLint ok = 0;
		glGetProgramiv(program, GL_LINK_STATUS, &ok);
		if (!ok) {
			glDeleteProgram(program);
			program = 0;
			glDeleteShader(vs);
			glDeleteShader(fs);
			vs = 0;
			fs = 0;
			return false;
		}

		uViewProj = glGetUniformLocation(program, "uViewProj");

		glGenVertexArrays(1, &vao);
		glGenBuffers(1, &vbo);
		vboCapacity = 0;
		vboMapped = nullptr;

		supportsPersistent = (GLEW_ARB_buffer_storage || GLEW_VERSION_4_4);
		return (supportsPersistent && vao != 0 && vbo != 0);
	}

	bool EnsureTarget(int width, int height)
	{
		if (width <= 0 || height <= 0) {
			return false;
		}
		if (!fbo) {
			glGenFramebuffers(1, &fbo);
		}
		if (!tex) {
			glGenTextures(1, &tex);
		}
		if (!fbo || !tex) {
			return false;
		}
		if (texWidth != width || texHeight != height) {
			texWidth = width;
			texHeight = height;
			glBindTexture(GL_TEXTURE_2D, tex);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, width, height, 0, GL_RED, GL_FLOAT, nullptr);
			glBindTexture(GL_TEXTURE_2D, 0);

			glBindFramebuffer(GL_FRAMEBUFFER, fbo);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
			const GLenum drawBuffers[1] = {GL_COLOR_ATTACHMENT0};
			glDrawBuffers(1, drawBuffers);
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
		}
		return true;
	}

	bool EnsureSlot(GlInvWOcclusionSlot &slot, int width, int height)
	{
		const size_t bytes = (size_t)width * (size_t)height * sizeof(float);
		if (!slot.pbo) {
			glGenBuffers(1, &slot.pbo);
			slot.byteSize = 0;
		}
		if (!slot.pbo) {
			return false;
		}
		if (slot.byteSize != bytes) {
			if (slot.fence) {
				glDeleteSync(slot.fence);
				slot.fence = nullptr;
			}
			if (slot.mapped && supportsPersistent) {
				glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
				glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
				glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
				slot.mapped = nullptr;
			}
			glDeleteBuffers(1, &slot.pbo);
			slot.pbo = 0;
			glGenBuffers(1, &slot.pbo);
			if (!slot.pbo) {
				return false;
			}
			glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
			if (supportsPersistent) {
				glBufferStorage(GL_PIXEL_PACK_BUFFER,
				                (GLsizeiptr)bytes,
				                nullptr,
				                GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
				slot.mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER,
				                              0,
				                              (GLsizeiptr)bytes,
				                              GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
				if (!slot.mapped) {
					glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
					return false;
				}
			}
			else {
				glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)bytes, nullptr, GL_STREAM_READ);
				slot.mapped = nullptr;
			}
			glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
			slot.byteSize = bytes;
		}
		slot.width = width;
		slot.height = height;
		return true;
	}

	bool IsFenceReady(GLsync fence) const
	{
		if (!fence) {
			return true;
		}
		const GLenum r = glClientWaitSync(fence, 0, 0);
		return (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED);
	}

	int FindWritableSlotIndex()
	{
		for (int attempt = 0; attempt < kRingSize; ++attempt) {
			const int idx = (writeIndex + attempt) % kRingSize;
			if (published && idx == lastPublishedIndex) {
				continue;
			}
			GlInvWOcclusionSlot &slot = slots[idx];
			if (!slot.fence || IsFenceReady(slot.fence)) {
				writeIndex = idx;
				return idx;
			}
		}
		return -1;
	}

	void PublishLatestReady()
	{
		for (int k = 1; k <= kRingSize; ++k) {
			const int idx = (writeIndex - k + kRingSize) % kRingSize;
			GlInvWOcclusionSlot &slot = slots[idx];
			if (!slot.fence) {
				continue;
			}
			if (!IsFenceReady(slot.fence)) {
				continue;
			}

			if (!supportsPersistent) {
				glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
				slot.mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)slot.byteSize, GL_MAP_READ_BIT);
				glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
				if (!slot.mapped) {
					continue;
				}
			}

			gOcbGpu.m_buffer = (btScalar *)slot.mapped;
			gOcbGpu.m_bufferSize = slot.byteSize;
			gOcbGpu.m_initialized = true;
			gOcbGpu.m_occlusion = slot.occlusion;
			gOcbGpu.m_sizes[0] = slot.sizes[0];
			gOcbGpu.m_sizes[1] = slot.sizes[1];
			gOcbGpu.m_scales[0] = slot.scales[0];
			gOcbGpu.m_scales[1] = slot.scales[1];
			gOcbGpu.m_offsets[0] = slot.offsets[0];
			gOcbGpu.m_offsets[1] = slot.offsets[1];
			for (int i = 0; i < 16; ++i) {
				gOcbGpu.m_wtc[i] = slot.wtc[i];
			}

			if (!supportsPersistent) {
				glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
				glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
				glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
				slot.mapped = nullptr;
				gOcbGpu.m_buffer = nullptr;
				gOcbGpu.m_occlusion = false;
				gOcbGpu.m_initialized = false;
				return;
			}

			gOcbGpu.BuildHzb();

			published = true;
			lastPublishedIndex = idx;
			return;
		}
	}

	bool EnsureVboCapacity(size_t bytes)
	{
		if (bytes <= vboCapacity) {
			return true;
		}
		const size_t newCap = std::max(bytes, vboCapacity + (vboCapacity / 2) + (size_t)1);
		if (vboMapped) {
			glBindBuffer(GL_ARRAY_BUFFER, vbo);
			glUnmapBuffer(GL_ARRAY_BUFFER);
			glBindBuffer(GL_ARRAY_BUFFER, 0);
			vboMapped = nullptr;
		}
		if (vbo) {
			glDeleteBuffers(1, &vbo);
			vbo = 0;
		}
		glGenBuffers(1, &vbo);
		if (!vbo) {
			return false;
		}
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glBufferStorage(GL_ARRAY_BUFFER,
		                (GLsizeiptr)newCap,
		                nullptr,
		                GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
		vboMapped = glMapBufferRange(GL_ARRAY_BUFFER,
		                             0,
		                             (GLsizeiptr)newCap,
		                             GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		if (!vboMapped) {
			glDeleteBuffers(1, &vbo);
			vbo = 0;
			return false;
		}
		vboCapacity = newCap;
		return true;
	}

	template <typename CacheVector>
	bool EnsureOccludersUploaded(CacheVector &caches, uint32_t revision)
	{
		if (uploadedOccludersRevision == revision) {
			return true;
		}

		size_t totalFloatCount = 0;
		for (auto &cache : caches) {
			const auto &src = (!cache.lodTriangles.empty()) ? cache.lodTriangles : cache.triangles;
			const size_t floatCount = src.size();
			if (floatCount < 9) {
				cache.gpuFirstVertex = -1;
				cache.gpuVertexCount = 0;
				continue;
			}
			totalFloatCount += floatCount;
		}

		if (totalFloatCount < 9) {
			uploadedOccludersRevision = revision;
			return true;
		}

		const size_t totalBytes = totalFloatCount * sizeof(float);
		if (!EnsureVboCapacity(totalBytes) || !vboMapped) {
			return false;
		}

		size_t byteOffset = 0;
		int vertexOffset = 0;
		for (auto &cache : caches) {
			const auto &src = (!cache.lodTriangles.empty()) ? cache.lodTriangles : cache.triangles;
			const size_t floatCount = src.size();
			if (floatCount < 9) {
				cache.gpuFirstVertex = -1;
				cache.gpuVertexCount = 0;
				continue;
			}
			const size_t bytes = floatCount * sizeof(float);
			std::memcpy((unsigned char *)vboMapped + byteOffset, src.data(), bytes);
			cache.gpuFirstVertex = vertexOffset;
			cache.gpuVertexCount = (int)(floatCount / 3);
			byteOffset += bytes;
			vertexOffset += cache.gpuVertexCount;
		}

		uploadedOccludersRevision = revision;
		return true;
	}

	template <typename CacheVector, typename CandidateVector>
	bool RenderAndEnqueue(CacheVector &allCaches,
	                      const CandidateVector &candidates,
	                      uint32_t cachesRevision,
	                      int slotIndex,
	                      int width,
	                      int height,
	                      const float *viewProj)
	{
		GlInvWOcclusionSlot &slot = slots[slotIndex];

		GLint prevFbo = 0;
		GLint prevViewport[4] = {};
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
		glGetIntegerv(GL_VIEWPORT, prevViewport);

		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
		glViewport(0, 0, width, height);
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_CULL_FACE);
		glEnable(GL_BLEND);
		glBlendEquation(GL_MAX);
		glBlendFunc(GL_ONE, GL_ONE);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		const GLfloat zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
		glClearBufferfv(GL_COLOR, 0, zero);

		glUseProgram(program);
		glUniformMatrix4fv(uViewProj, 1, GL_FALSE, viewProj);

		const bool uploadOk = EnsureOccludersUploaded(allCaches, cachesRevision);

		glBindVertexArray(vao);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, (GLsizei)(sizeof(float) * 3), (void *)0);

		auto transformClip = [&](float x, float y, float z, btVector4 &out) {
			out[0] = btScalar(viewProj[0] * x + viewProj[4] * y + viewProj[8] * z + viewProj[12]);
			out[1] = btScalar(viewProj[1] * x + viewProj[5] * y + viewProj[9] * z + viewProj[13]);
			out[2] = btScalar(viewProj[2] * x + viewProj[6] * y + viewProj[10] * z + viewProj[14]);
			out[3] = btScalar(viewProj[3] * x + viewProj[7] * y + viewProj[11] * z + viewProj[15]);
		};

		auto cacheIntersectsClip = [&](const auto &cache) -> bool {
			const float xmin = cache.aabbMin[0], ymin = cache.aabbMin[1], zmin = cache.aabbMin[2];
			const float xmax = cache.aabbMax[0], ymax = cache.aabbMax[1], zmax = cache.aabbMax[2];

			btVector4 c[8];
			transformClip(xmin, ymin, zmin, c[0]);
			transformClip(xmax, ymin, zmin, c[1]);
			transformClip(xmax, ymax, zmin, c[2]);
			transformClip(xmin, ymax, zmin, c[3]);
			transformClip(xmin, ymin, zmax, c[4]);
			transformClip(xmax, ymin, zmax, c[5]);
			transformClip(xmax, ymax, zmax, c[6]);
			transformClip(xmin, ymax, zmax, c[7]);

			bool allLeft = true, allRight = true, allBottom = true, allTop = true, allNear = true, allFar = true;
			for (int i = 0; i < 8; ++i) {
				const btScalar x = c[i][0], y = c[i][1], z = c[i][2], w = c[i][3];
				allLeft   &= (x < -w);
				allRight  &= (x >  w);
				allBottom &= (y < -w);
				allTop    &= (y >  w);
				allNear   &= ((z + w) < 0);
				allFar    &= ((z - w) > 0);
			}
			if (allLeft || allRight || allBottom || allTop || allNear || allFar) {
				return false;
			}

			btScalar minX = btScalar(1e30f);
			btScalar maxX = btScalar(-1e30f);
			btScalar minY = btScalar(1e30f);
			btScalar maxY = btScalar(-1e30f);
			int valid = 0;
			for (int i = 0; i < 8; ++i) {
				const btScalar w = c[i][3];
				if (w <= btScalar(1e-6f)) {
					continue;
				}
				const btScalar invW = btScalar(1) / w;
				const btScalar ndcX = c[i][0] * invW;
				const btScalar ndcY = c[i][1] * invW;
				const btScalar sx = (ndcX * btScalar(0.5f) + btScalar(0.5f)) * (btScalar)width;
				const btScalar sy = (ndcY * btScalar(0.5f) + btScalar(0.5f)) * (btScalar)height;
				minX = btMin(minX, sx);
				maxX = btMax(maxX, sx);
				minY = btMin(minY, sy);
				maxY = btMax(maxY, sy);
				++valid;
			}

			return true;
		};

		bool drewAny = false;
		if (uploadOk) {
			std::vector<GLint> firsts;
			std::vector<GLsizei> counts;
			firsts.reserve(candidates.size());
			counts.reserve(candidates.size());
			for (auto *cache : candidates) {
				if (!cache) {
					continue;
				}
				if (cache->gpuFirstVertex < 0 || cache->gpuVertexCount < 3) {
					continue;
				}
				if (!cacheIntersectsClip(*cache)) {
					continue;
				}
				firsts.push_back((GLint)cache->gpuFirstVertex);
				counts.push_back((GLsizei)cache->gpuVertexCount);
			}
			if (!firsts.empty()) {
				glMultiDrawArrays(GL_TRIANGLES, firsts.data(), counts.data(), (GLsizei)firsts.size());
				drewAny = true;
			}
		}

		if (drewAny) {
			glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
			glReadPixels(0, 0, width, height, GL_RED, GL_FLOAT, 0);
			glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		}

		if (slot.fence) {
			glDeleteSync(slot.fence);
		}
		slot.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		slot.occlusion = drewAny;

		glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
		glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
		glUseProgram(0);
		glBindVertexArray(0);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		glDisable(GL_BLEND);
		glEnable(GL_DEPTH_TEST);
		glEnable(GL_CULL_FACE);
		glBlendEquation(GL_FUNC_ADD);
		glBlendFunc(GL_ONE, GL_ZERO);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

		return true;
	}
};

static GlInvWOcclusionBackend gGlInvWOcclusion;

static void ResetGlInvWOcclusion()
{
	gGlInvWOcclusion.published = false;
	gGlInvWOcclusion.lastPublishedIndex = -1;
	for (int i = 0; i < GlInvWOcclusionBackend::kRingSize; ++i) {
		gGlInvWOcclusion.slots[i].fence = nullptr;
		gGlInvWOcclusion.slots[i].occlusion = false;
	}
	gOcbGpu.m_occlusion = false;
	gOcbGpu.m_buffer = nullptr;
	gOcbGpu.m_initialized = false;
}
}  // namespace
#endif

void CcdPhysicsEnvironment::BeginOcclusionBuffer(int occlusionRes, const int *viewport, const mt::mat4& matrix)
{
	if (occlusionRes <= 0) {
		return;
	}

#ifdef WITH_PYTHON
	int width = 0;
	int height = 0;
	btScalar wtc[16] = {};
	int sizes[2] = {};
	btScalar scales[2] = {};
	btScalar offsets[2] = {};

	// Reseta o ring buffer GPU na primeira chamada de cada sessão do jogo.
	// gGlInvWOcclusion é global estático que persiste entre execuções no Blender.
	if (!m_occlusionGpuResetDone) {
		m_occlusionGpuResetDone = true;
		ResetGlInvWOcclusion();
	}

	if (ComputeOcclusionParams(occlusionRes, viewport, matrix, width, height, wtc, sizes, scales, offsets)) {
		if (gGlInvWOcclusion.EnsureProgram() && gGlInvWOcclusion.EnsureTarget(width, height)) {
			gGlInvWOcclusion.PublishLatestReady();

			const int slotIndex = gGlInvWOcclusion.FindWritableSlotIndex();
			if (slotIndex >= 0) {
				GlInvWOcclusionSlot &slot = gGlInvWOcclusion.slots[slotIndex];
				for (int i = 0; i < 16; ++i) {
					slot.wtc[i] = wtc[i];
				}
				slot.sizes[0] = sizes[0];
				slot.sizes[1] = sizes[1];
				slot.scales[0] = scales[0];
				slot.scales[1] = scales[1];
				slot.offsets[0] = offsets[0];
				slot.offsets[1] = offsets[1];

				if (gGlInvWOcclusion.EnsureSlot(slot, width, height)) {
					std::vector<StaticOccluderCache *> candidates;
					candidates.reserve(m_staticOccluders.size());
					if (m_staticOccluderDbvt && m_staticOccluderDbvt->m_root) {
						const float *m = (const float *)matrix.Data();
						btVector3 planes_n[6];
						btScalar planes_o[6];
						int signs[6];
						auto addPlane = [&](int idx, float a, float b, float c, float d) {
							const float len2 = a * a + b * b + c * c;
							if (len2 > 1e-20f) {
								const float invLen = 1.0f / std::sqrt(len2);
								a *= invLen;
								b *= invLen;
								c *= invLen;
								d *= invLen;
							}
							planes_n[idx] = btVector3((btScalar)a, (btScalar)b, (btScalar)c);
							planes_o[idx] = (btScalar)d;
							signs[idx] = ((a >= 0.0f) ? 1 : 0) + ((b >= 0.0f) ? 2 : 0) + ((c >= 0.0f) ? 4 : 0);
						};
						addPlane(0, m[3] + m[0],  m[7] + m[4],  m[11] + m[8],  m[15] + m[12]);
						addPlane(1, m[3] - m[0],  m[7] - m[4],  m[11] - m[8],  m[15] - m[12]);
						addPlane(2, m[3] + m[1],  m[7] + m[5],  m[11] + m[9],  m[15] + m[13]);
						addPlane(3, m[3] - m[1],  m[7] - m[5],  m[11] - m[9],  m[15] - m[13]);
						addPlane(4, m[3] + m[2],  m[7] + m[6],  m[11] + m[10], m[15] + m[14]);
						addPlane(5, m[3] - m[2],  m[7] - m[6],  m[11] - m[10], m[15] - m[14]);

						struct Collect : btDbvt::ICollide {
							std::vector<KX_GameObject *> objects;
							void Process(const btDbvtNode *leaf)
							{
								objects.push_back((KX_GameObject *)leaf->data);
							}
						} collect;
						collect.objects.reserve(m_staticOccluders.size());
						btDbvt::collideKDOP6(m_staticOccluderDbvt->m_root, planes_n, planes_o, signs, collect);

						for (KX_GameObject *obj : collect.objects) {
							auto it = m_staticOccluderIndex.find(obj);
							if (it != m_staticOccluderIndex.end()) {
								candidates.push_back(&m_staticOccluders[it->second]);
							}
						}
					}
					else {
						for (auto &cache : m_staticOccluders) {
							candidates.push_back(&cache);
						}
					}

					gGlInvWOcclusion.RenderAndEnqueue(m_staticOccluders,
					                                 candidates,
					                                 m_staticOccludersRevision,
					                                 slotIndex,
					                                 width,
					                                 height,
					                                 (const float *)matrix.Data());
					gGlInvWOcclusion.writeIndex = (slotIndex + 1) % GlInvWOcclusionBackend::kRingSize;
					return;
				}
			}

			if (gGlInvWOcclusion.published) {
				return;
			}
		}
	}
#endif

	gOcb.setup(occlusionRes, viewport, (float *)matrix.Data());
	gOcb.initialize();

	std::vector<StaticOccluderCache *> candidates;
	candidates.reserve(m_staticOccluders.size());
	if (m_staticOccluderDbvt && m_staticOccluderDbvt->m_root) {
		const float *m = (const float *)matrix.Data();
		btVector3 planes_n[6];
		btScalar planes_o[6];
		int signs[6];
		auto addPlane = [&](int idx, float a, float b, float c, float d) {
			const float len2 = a * a + b * b + c * c;
			if (len2 > 1e-20f) {
				const float invLen = 1.0f / std::sqrt(len2);
				a *= invLen;
				b *= invLen;
				c *= invLen;
				d *= invLen;
			}
			planes_n[idx] = btVector3((btScalar)a, (btScalar)b, (btScalar)c);
			planes_o[idx] = (btScalar)d;
			signs[idx] = ((a >= 0.0f) ? 1 : 0) + ((b >= 0.0f) ? 2 : 0) + ((c >= 0.0f) ? 4 : 0);
		};
		addPlane(0, m[3] + m[0],  m[7] + m[4],  m[11] + m[8],  m[15] + m[12]);
		addPlane(1, m[3] - m[0],  m[7] - m[4],  m[11] - m[8],  m[15] - m[12]);
		addPlane(2, m[3] + m[1],  m[7] + m[5],  m[11] + m[9],  m[15] + m[13]);
		addPlane(3, m[3] - m[1],  m[7] - m[5],  m[11] - m[9],  m[15] - m[13]);
		addPlane(4, m[3] + m[2],  m[7] + m[6],  m[11] + m[10], m[15] + m[14]);
		addPlane(5, m[3] - m[2],  m[7] - m[6],  m[11] - m[10], m[15] - m[14]);

		struct Collect : btDbvt::ICollide {
			std::vector<KX_GameObject *> objects;
			void Process(const btDbvtNode *leaf)
			{
				objects.push_back((KX_GameObject *)leaf->data);
			}
		} collect;
		collect.objects.reserve(m_staticOccluders.size());
		btDbvt::collideKDOP6(m_staticOccluderDbvt->m_root, planes_n, planes_o, signs, collect);

		for (KX_GameObject *obj : collect.objects) {
			auto it = m_staticOccluderIndex.find(obj);
			if (it != m_staticOccluderIndex.end()) {
				candidates.push_back(&m_staticOccluders[it->second]);
			}
		}
	}
	else {
		for (auto &cache : m_staticOccluders) {
			candidates.push_back(&cache);
		}
	}

	for (const StaticOccluderCache *cachePtr : candidates) {
		if (!cachePtr) {
			continue;
		}
		const StaticOccluderCache &cache = *cachePtr;
		auto transformClip = [&](float x, float y, float z, btVector4 &out) {
			const float *m = (float *)matrix.Data();
			out[0] = btScalar(m[0] * x + m[4] * y + m[8] * z + m[12]);
			out[1] = btScalar(m[1] * x + m[5] * y + m[9] * z + m[13]);
			out[2] = btScalar(m[2] * x + m[6] * y + m[10] * z + m[14]);
			out[3] = btScalar(m[3] * x + m[7] * y + m[11] * z + m[15]);
		};

		auto cacheIntersectsClip = [&]() -> bool {
			const float xmin = cache.aabbMin[0], ymin = cache.aabbMin[1], zmin = cache.aabbMin[2];
			const float xmax = cache.aabbMax[0], ymax = cache.aabbMax[1], zmax = cache.aabbMax[2];

			btVector4 c[8];
			transformClip(xmin, ymin, zmin, c[0]);
			transformClip(xmax, ymin, zmin, c[1]);
			transformClip(xmax, ymax, zmin, c[2]);
			transformClip(xmin, ymax, zmin, c[3]);
			transformClip(xmin, ymin, zmax, c[4]);
			transformClip(xmax, ymin, zmax, c[5]);
			transformClip(xmax, ymax, zmax, c[6]);
			transformClip(xmin, ymax, zmax, c[7]);

			bool allLeft = true, allRight = true, allBottom = true, allTop = true, allNear = true, allFar = true;
			for (int i = 0; i < 8; ++i) {
				const btScalar x = c[i][0], y = c[i][1], z = c[i][2], w = c[i][3];
				allLeft   &= (x < -w);
				allRight  &= (x >  w);
				allBottom &= (y < -w);
				allTop    &= (y >  w);
				allNear   &= ((z + w) < 0);
				allFar    &= ((z - w) > 0);
			}
			return !(allLeft || allRight || allBottom || allTop || allNear || allFar);
		};

		if (!cacheIntersectsClip()) {
			continue;
		}

		const std::vector<float> &tris = (!cache.lodTriangles.empty()) ? cache.lodTriangles : cache.triangles;
		const size_t floatCount = tris.size();
		const float *v = tris.data();
		for (size_t i = 0; (i + 8) < floatCount; i += 9) {
			gOcb.appendOccluderW(&v[i + 0], &v[i + 3], &v[i + 6], 0.0f);
		}
	}
}

void CcdPhysicsEnvironment::RegisterStaticOccluder(KX_GameObject *gameobj)
{
	if (!gameobj) {
		return;
	}

	for (size_t i = 0; i < m_staticOccluders.size(); ++i) {
		if (m_staticOccluders[i].object == gameobj) {
			auto itNode = m_staticOccluderDbvtNodes.find(gameobj);
			if (itNode != m_staticOccluderDbvtNodes.end()) {
				if (m_staticOccluderDbvt && itNode->second) {
					m_staticOccluderDbvt->remove(itNode->second);
				}
				m_staticOccluderDbvtNodes.erase(itNode);
			}
			m_staticOccluders.erase(m_staticOccluders.begin() + (ptrdiff_t)i);
			break;
		}
	}

	StaticOccluderCache cache;
	cache.object = gameobj;

	if (gameobj->GetMeshList().empty()) {
		m_staticOccluders.push_back(std::move(cache));
		m_staticOccluderIndex.clear();
		for (size_t i = 0; i < m_staticOccluders.size(); ++i) {
			if (m_staticOccluders[i].object) {
				m_staticOccluderIndex[m_staticOccluders[i].object] = i;
			}
		}
		++m_staticOccludersRevision;
		return;
	}

	gameobj->UpdateBounds(true);
	mt::vec3 localMin;
	mt::vec3 localMax;
	gameobj->GetBoundsAabb(localMin, localMax);

	float fl[16];
	gameobj->NodeGetWorldTransform().PackFromAffineTransform(fl);

	const mt::vec3 corners[8] = {
		mt::vec3(localMin.x, localMin.y, localMin.z),
		mt::vec3(localMin.x, localMin.y, localMax.z),
		mt::vec3(localMin.x, localMax.y, localMin.z),
		mt::vec3(localMin.x, localMax.y, localMax.z),
		mt::vec3(localMax.x, localMin.y, localMin.z),
		mt::vec3(localMax.x, localMin.y, localMax.z),
		mt::vec3(localMax.x, localMax.y, localMin.z),
		mt::vec3(localMax.x, localMax.y, localMax.z),
	};

	auto transformPointAffine = [&](const mt::vec3 &p) -> mt::vec3 {
		const float x = p.x, y = p.y, z = p.z;
		return mt::vec3(fl[0] * x + fl[4] * y + fl[8] * z + fl[12],
		                fl[1] * x + fl[5] * y + fl[9] * z + fl[13],
		                fl[2] * x + fl[6] * y + fl[10] * z + fl[14]);
	};

	mt::vec3 worldMin = transformPointAffine(corners[0]);
	mt::vec3 worldMax = worldMin;
	for (int i = 1; i < 8; ++i) {
		const mt::vec3 w = transformPointAffine(corners[i]);
		worldMin.x = std::min(worldMin.x, w.x);
		worldMin.y = std::min(worldMin.y, w.y);
		worldMin.z = std::min(worldMin.z, w.z);
		worldMax.x = std::max(worldMax.x, w.x);
		worldMax.y = std::max(worldMax.y, w.y);
		worldMax.z = std::max(worldMax.z, w.z);
	}
	cache.aabbMin[0] = worldMin.x;
	cache.aabbMin[1] = worldMin.y;
	cache.aabbMin[2] = worldMin.z;
	cache.aabbMax[0] = worldMax.x;
	cache.aabbMax[1] = worldMax.y;
	cache.aabbMax[2] = worldMax.z;

	auto transformPoint = [&](const float p[3]) {
		const float x = p[0], y = p[1], z = p[2];
		const float wx = fl[0] * x + fl[4] * y + fl[8] * z + fl[12];
		const float wy = fl[1] * x + fl[5] * y + fl[9] * z + fl[13];
		const float wz = fl[2] * x + fl[6] * y + fl[10] * z + fl[14];
		cache.triangles.push_back(wx);
		cache.triangles.push_back(wy);
		cache.triangles.push_back(wz);
	};

	size_t estimatedFloatCount = 0;
	for (KX_Mesh *meshobj : gameobj->GetMeshList()) {
		for (RAS_MeshMaterial *meshmat : meshobj->GetMeshMaterialList()) {
			RAS_DisplayArray *array = meshmat->GetDisplayArray();
			estimatedFloatCount += (size_t)array->GetTriangleIndexCount() * 3;
		}
	}
	cache.triangles.reserve(estimatedFloatCount);

	for (KX_Mesh *meshobj : gameobj->GetMeshList()) {
		for (RAS_MeshMaterial *meshmat : meshobj->GetMeshMaterialList()) {
			RAS_DisplayArray *array = meshmat->GetDisplayArray();
			const unsigned int triCount = array->GetTriangleIndexCount();
			const unsigned int *triIndices = array->GetTriangleIndicesData();
			const mt::vec3_packed *positions = array->GetPositionsData();
			for (unsigned int j = 0; j < triCount; j += 3) {
				const unsigned int i0 = triIndices[j];
				const unsigned int i1 = triIndices[j + 1];
				const unsigned int i2 = triIndices[j + 2];
				transformPoint(positions[i0].data);
				transformPoint(positions[i1].data);
				transformPoint(positions[i2].data);
			}
		}
	}

	{
		cache.lodTriangles.clear();
		const size_t triCount = cache.triangles.size() / 9;
		if (triCount <= 12) {
			cache.lodTriangles = cache.triangles;
		}
		else {
			struct Pick {
				float area;
				size_t tri;
			};

			std::array<std::vector<Pick>, 6> buckets;
			for (size_t i = 0; i < 6; ++i) {
				buckets[i].reserve(32);
			}

			for (size_t t = 0; t < triCount; ++t) {
				const size_t base = t * 9;
				const float ax = cache.triangles[base + 0];
				const float ay = cache.triangles[base + 1];
				const float az = cache.triangles[base + 2];
				const float bx = cache.triangles[base + 3];
				const float by = cache.triangles[base + 4];
				const float bz = cache.triangles[base + 5];
				const float cx = cache.triangles[base + 6];
				const float cy = cache.triangles[base + 7];
				const float cz = cache.triangles[base + 8];

				const float abx = bx - ax;
				const float aby = by - ay;
				const float abz = bz - az;
				const float acx = cx - ax;
				const float acy = cy - ay;
				const float acz = cz - az;

				const float nx = aby * acz - abz * acy;
				const float ny = abz * acx - abx * acz;
				const float nz = abx * acy - aby * acx;

				const float len2 = nx * nx + ny * ny + nz * nz;
				if (len2 <= 1e-12f) {
					continue;
				}

				const float len = std::sqrt(len2);
				const float area = 0.5f * len;

				const float anx = std::abs(nx);
				const float any = std::abs(ny);
				const float anz = std::abs(nz);
				int axis = 0;
				float major = anx;
				if (any > major) {
					axis = 1;
					major = any;
				}
				if (anz > major) {
					axis = 2;
				}
				const float comp = (axis == 0) ? nx : (axis == 1) ? ny : nz;
				const int bucket = axis * 2 + ((comp < 0.0f) ? 1 : 0);
				buckets[(size_t)bucket].push_back({area, t});
			}

			const int kPerBucket = 2;
			cache.lodTriangles.reserve((size_t)6 * (size_t)kPerBucket * 9);
			for (size_t b = 0; b < 6; ++b) {
				auto &v = buckets[b];
				if (v.empty()) {
					continue;
				}
				const size_t take = std::min((size_t)kPerBucket, v.size());
				std::nth_element(v.begin(), v.begin() + (ptrdiff_t)take, v.end(),
				                 [](const Pick &a, const Pick &c) { return a.area > c.area; });
				std::sort(v.begin(), v.begin() + (ptrdiff_t)take,
				          [](const Pick &a, const Pick &c) { return a.area > c.area; });
				for (size_t i = 0; i < take; ++i) {
					const size_t base = v[i].tri * 9;
					for (size_t k = 0; k < 9; ++k) {
						cache.lodTriangles.push_back(cache.triangles[base + k]);
					}
				}
			}

			if (cache.lodTriangles.size() < 9) {
				const size_t maxTris = 4096;
				const size_t step = (triCount + maxTris - 1) / maxTris;
				cache.lodTriangles.clear();
				cache.lodTriangles.reserve(((triCount + step - 1) / step) * 9);
				for (size_t t = 0; t < triCount; t += step) {
					const size_t base = t * 9;
					for (size_t k = 0; k < 9; ++k) {
						cache.lodTriangles.push_back(cache.triangles[base + k]);
					}
				}
			}
		}
	}

	m_staticOccluders.push_back(std::move(cache));

	m_staticOccluderIndex.clear();
	for (size_t i = 0; i < m_staticOccluders.size(); ++i) {
		if (m_staticOccluders[i].object) {
			m_staticOccluderIndex[m_staticOccluders[i].object] = i;
		}
	}

	if (m_staticOccluderDbvt) {
		const StaticOccluderCache &c = m_staticOccluders.back();
		const btVector3 mn(c.aabbMin[0], c.aabbMin[1], c.aabbMin[2]);
		const btVector3 mx(c.aabbMax[0], c.aabbMax[1], c.aabbMax[2]);
		btDbvtNode *node = m_staticOccluderDbvt->insert(btDbvtVolume::FromMM(mn, mx), (void *)gameobj);
		m_staticOccluderDbvtNodes[gameobj] = node;
	}

	++m_staticOccludersRevision;
}

void CcdPhysicsEnvironment::UnregisterStaticOccluder(KX_GameObject *gameobj)
{
	if (!gameobj) {
		return;
	}

	for (size_t i = 0; i < m_staticOccluders.size(); ++i) {
		if (m_staticOccluders[i].object == gameobj) {
			auto itNode = m_staticOccluderDbvtNodes.find(gameobj);
			if (itNode != m_staticOccluderDbvtNodes.end()) {
				if (m_staticOccluderDbvt && itNode->second) {
					m_staticOccluderDbvt->remove(itNode->second);
				}
				m_staticOccluderDbvtNodes.erase(itNode);
			}
			m_staticOccluders.erase(m_staticOccluders.begin() + (ptrdiff_t)i);
			m_staticOccluderIndex.clear();
			for (size_t i = 0; i < m_staticOccluders.size(); ++i) {
				if (m_staticOccluders[i].object) {
					m_staticOccluderIndex[m_staticOccluders[i].object] = i;
				}
			}
			++m_staticOccludersRevision;
			return;
		}
	}
}

void CcdPhysicsEnvironment::SubmitOccluder(KX_GameObject *gameobj)
{
	if (!gameobj || !gameobj->GetOccluder()) {
		return;
	}

	if (gameobj->GetMeshList().empty()) {
		return;
	}

	float fl[16];
	gameobj->NodeGetWorldTransform().PackFromAffineTransform(fl);
	gOcb.SetModelMatrix(fl);

	const bool negative = gameobj->IsNegativeScaling();
	for (KX_Mesh *meshobj : gameobj->GetMeshList()) {
		for (RAS_MeshMaterial *meshmat : meshobj->GetMeshMaterialList()) {
			RAS_DisplayArray *array = meshmat->GetDisplayArray();
			const bool twoside = meshmat->GetBucket()->GetMaterial()->IsTwoSided();
			const float face = (twoside) ? 0.0f : (negative ? -1.0f : 1.0f);

			const unsigned int triCount = array->GetTriangleIndexCount();
			const unsigned int *triIndices = array->GetTriangleIndicesData();
			const mt::vec3_packed *positions = array->GetPositionsData();
			for (unsigned int j = 0; j < triCount; j += 3) {
				const unsigned int i0 = triIndices[j];
				const unsigned int i1 = triIndices[j + 1];
				const unsigned int i2 = triIndices[j + 2];
				gOcb.appendOccluderM(positions[i0].data,
				                     positions[i1].data,
				                     positions[i2].data,
				                     face);
			}
		}
	}
}

bool CcdPhysicsEnvironment::QueryOcclusionAabb(const mt::vec3& aabbMin, const mt::vec3& aabbMax)
{
	const btVector3 c((aabbMin.x + aabbMax.x) * 0.5f,
	                  (aabbMin.y + aabbMax.y) * 0.5f,
	                  (aabbMin.z + aabbMax.z) * 0.5f);
	const btVector3 e((aabbMax.x - aabbMin.x) * 0.5f,
	                  (aabbMax.y - aabbMin.y) * 0.5f,
	                  (aabbMax.z - aabbMin.z) * 0.5f);

#ifdef WITH_PYTHON
	if (gGlInvWOcclusion.published && gOcbGpu.m_buffer) {
		return gOcbGpu.queryOccluderW(c, e);
	}
#endif

	return gOcb.queryOccluderW(c, e);
}

bool CcdPhysicsEnvironment::CullingTest(PHY_CullingCallback callback, void *userData, const std::array<mt::vec4, 6>& planes,
                                        int occlusionRes, const int *viewport, const mt::mat4& matrix)
{
	if (!m_cullingTree) {
		return false;
	}
	DbvtCullingCallback dispatcher(callback, userData);
	btVector3 planes_n[6];
	btScalar planes_o[6];
	for (int i = 0; i < 6; i++) {
		planes_n[i] = btVector3((btScalar)planes[i][0], (btScalar)planes[i][1], (btScalar)planes[i][2]);
		planes_o[i] = (btScalar)planes[i][3];
	}
	int signs[6];
	for (int i = 0; i < 6; ++i) {
		signs[i] = ((planes_n[i].x() >= 0) ? 1 : 0) +
		           ((planes_n[i].y() >= 0) ? 2 : 0) +
		           ((planes_n[i].z() >= 0) ? 4 : 0);
	}
	// if occlusionRes != 0 => occlusion culling
	if (occlusionRes) {
		gOcb.setup(occlusionRes, viewport, (float *)matrix.Data());
		dispatcher.m_ocb = &gOcb;
		// occlusion culling, the direction of the view is taken from the first plan which MUST be the near plane
		btDbvt::collideOCL(m_cullingTree->m_sets[1].m_root, planes_n, planes_o, planes_n[0], 6, dispatcher);
		btDbvt::collideOCL(m_cullingTree->m_sets[0].m_root, planes_n, planes_o, planes_n[0], 6, dispatcher);
	}
	else {
		btDbvt::collideKDOP6(m_cullingTree->m_sets[1].m_root, planes_n, planes_o, signs, dispatcher);
		btDbvt::collideKDOP6(m_cullingTree->m_sets[0].m_root, planes_n, planes_o, signs, dispatcher);
	}
	return true;
}

bool CcdPhysicsEnvironment::CullingTestKX(std::vector<KX_GameObject *>& objects, int layer, bool updateState, const std::array<mt::vec4, 6>& planes,
                                         int planeCount, int occlusionRes, const int *viewport, const mt::mat4& matrix)
{
	if (!m_cullingTree) {
		return false;
	}

	struct DbvtCullingCallbackKX : btDbvt::ICollide {
		std::vector<KX_GameObject *>& m_objects;
		int m_layer;
		bool m_updateState;
		OcclusionBuffer *m_ocb;

		DbvtCullingCallbackKX(std::vector<KX_GameObject *>& objects, int layer, bool updateState)
		    :m_objects(objects),
		     m_layer(layer),
		     m_updateState(updateState),
		     m_ocb(nullptr)
		{
		}

		bool Descent(const btDbvtNode *node)
		{
			return (m_ocb->queryOccluderW(node->volume.Center(), node->volume.Extents()));
		}

		void Process(const btDbvtNode *node, btScalar depth)
		{
			Process(node);
		}

		void Process(const btDbvtNode *leaf)
		{
			btBroadphaseProxy *proxy = (btBroadphaseProxy *)leaf->data;
			CcdGraphicController *ctrl = static_cast<CcdGraphicController *>(proxy->m_clientObject);
			KX_ClientObjectInfo *info = (KX_ClientObjectInfo *)ctrl->GetNewClientInfoFast();
			KX_GameObject *gameobj = info ? info->m_gameobject : nullptr;

			if (!gameobj || !gameobj->Renderable(m_layer)) {
				return;
			}

			if (m_updateState) {
				gameobj->GetCullingNode().SetCulled(false);
			}
			m_objects.push_back(gameobj);
		}
	};

	DbvtCullingCallbackKX dispatcher(objects, layer, updateState);
	btVector3 planes_n[6];
	btScalar planes_o[6];
	for (int i = 0; i < 6; i++) {
		planes_n[i] = btVector3((btScalar)planes[i][0], (btScalar)planes[i][1], (btScalar)planes[i][2]);
		planes_o[i] = (btScalar)planes[i][3];
	}

	if (occlusionRes) {
		gOcb.setup(occlusionRes, viewport, (float *)matrix.Data());
		dispatcher.m_ocb = &gOcb;
		btDbvt::collideOCL(m_cullingTree->m_sets[1].m_root, planes_n, planes_o, planes_n[0], 6, dispatcher);
		btDbvt::collideOCL(m_cullingTree->m_sets[0].m_root, planes_n, planes_o, planes_n[0], 6, dispatcher);
	}
	else {
		if (planeCount == 4) {
			btVector3 planes4_n[4] = {planes_n[2], planes_n[3], planes_n[4], planes_n[5]};
			btScalar planes4_o[4] = {planes_o[2], planes_o[3], planes_o[4], planes_o[5]};
			int signs4[4];
			for (int i = 0; i < 4; ++i) {
				signs4[i] = ((planes4_n[i].x() >= 0) ? 1 : 0) +
				            ((planes4_n[i].y() >= 0) ? 2 : 0) +
				            ((planes4_n[i].z() >= 0) ? 4 : 0);
			}
			btDbvt::collideKDOP4(m_cullingTree->m_sets[1].m_root, planes4_n, planes4_o, signs4, dispatcher);
			btDbvt::collideKDOP4(m_cullingTree->m_sets[0].m_root, planes4_n, planes4_o, signs4, dispatcher);
		}
		else {
			int signs6[6];
			for (int i = 0; i < 6; ++i) {
				signs6[i] = ((planes_n[i].x() >= 0) ? 1 : 0) +
				            ((planes_n[i].y() >= 0) ? 2 : 0) +
				            ((planes_n[i].z() >= 0) ? 4 : 0);
			}
			btDbvt::collideKDOP6(m_cullingTree->m_sets[1].m_root, planes_n, planes_o, signs6, dispatcher);
			btDbvt::collideKDOP6(m_cullingTree->m_sets[0].m_root, planes_n, planes_o, signs6, dispatcher);
		}
	}

	return true;
}

int CcdPhysicsEnvironment::GetNumContactPoints()
{
	return 0;
}

void CcdPhysicsEnvironment::GetContactPoint(int i, float& hitX, float& hitY, float& hitZ, float& normalX, float& normalY, float& normalZ)
{
}

btBroadphaseInterface *CcdPhysicsEnvironment::GetBroadphase()
{
	return m_dynamicsWorld->getBroadphase();
}

btDispatcher *CcdPhysicsEnvironment::GetDispatcher()
{
	return m_dynamicsWorld->getDispatcher();
}

void CcdPhysicsEnvironment::MergeEnvironment(PHY_IPhysicsEnvironment *other_env)
{
	CcdPhysicsEnvironment *other = static_cast<CcdPhysicsEnvironment *>(other_env);
	if (other == nullptr) {
		CM_Error("other scene is not using Bullet physics, not merging physics.");
		return;
	}

	while (other->m_controllers.begin() != other->m_controllers.end()) {
		CcdPhysicsController *ctrl = *other->m_controllers.begin();

		other->RemoveCcdPhysicsController(ctrl, true);
		this->AddCcdPhysicsController(ctrl);
	}
}

CcdPhysicsEnvironment::~CcdPhysicsEnvironment()
{
#ifdef WITH_PYTHON
	ShutdownReinstanceAsync();
#endif

	m_wrapperVehicles.clear();
	m_staticOccluderDbvtNodes.clear();
	m_staticOccluderIndex.clear();
	m_staticOccluders.clear();
	delete m_staticOccluderDbvt;
	m_staticOccluderDbvt = nullptr;

	//m_broadphase->DestroyScene();
	//delete broadphase ? release reference on broadphase ?

	//first delete scene, then dispatcher, because pairs have to release manifolds on the dispatcher
	//delete m_dispatcher;
	delete m_dynamicsWorld;

	if (nullptr != m_ownDispatcher) {
		delete m_ownDispatcher;
	}

	if (nullptr != m_solver) {
		delete m_solver;
	}

	if (nullptr != m_debugDrawer) {
		delete m_debugDrawer;
	}

	if (nullptr != m_filterCallback) {
		delete m_filterCallback;
	}

	if (nullptr != m_ghostPairCallback) {
		delete m_ghostPairCallback;
	}

	if (nullptr != m_collisionConfiguration) {
		delete m_collisionConfiguration;
	}

	if (nullptr != m_broadphase) {
		delete m_broadphase;
	}

	if (nullptr != m_cullingTree) {
		delete m_cullingTree;
	}

	if (nullptr != m_cullingCache) {
		delete m_cullingCache;
	}
}

btTypedConstraint *CcdPhysicsEnvironment::GetConstraintById(int constraintId)
{
	// For soft body constraints
	if (constraintId == 0) {
		return nullptr;
	}

	int numConstraints = m_dynamicsWorld->getNumConstraints();
	int i;
	for (i = 0; i < numConstraints; i++) {
		btTypedConstraint *constraint = m_dynamicsWorld->getConstraint(i);
		if (constraint->getUserConstraintId() == constraintId) {
			return constraint;
		}
	}
	return nullptr;
}

void CcdPhysicsEnvironment::AddSensor(PHY_IPhysicsController *ctrl)
{
	CcdPhysicsController *ctrl1 = (CcdPhysicsController *)ctrl;
	AddCcdPhysicsController(ctrl1);
}

bool CcdPhysicsEnvironment::RemoveCollisionCallback(PHY_IPhysicsController *ctrl)
{
	CcdPhysicsController *ccdCtrl = (CcdPhysicsController *)ctrl;
	return ccdCtrl->Unregister();
}

void CcdPhysicsEnvironment::RemoveSensor(PHY_IPhysicsController *ctrl)
{
	RemoveCcdPhysicsController((CcdPhysicsController *)ctrl, true);
}

void CcdPhysicsEnvironment::AddCollisionCallback(int response_class, PHY_ResponseCallback callback, void *user)
{
	m_triggerCallbacks[response_class] = callback;
	m_triggerCallbacksUserPtrs[response_class] = user;
}
bool CcdPhysicsEnvironment::RequestCollisionCallback(PHY_IPhysicsController *ctrl)
{
	CcdPhysicsController *ccdCtrl = static_cast<CcdPhysicsController *>(ctrl);
	return ccdCtrl->Register();
}

void CcdPhysicsEnvironment::CallbackTriggers()
{
	if (!m_triggerCallbacks[PHY_OBJECT_RESPONSE]) {
		return;
	}

	// Walk over all overlapping pairs, and if one of the involved bodies is registered for trigger callback, perform callback.
	btDispatcher *dispatcher = m_dynamicsWorld->getDispatcher();
	for (unsigned int i = 0, numManifolds = dispatcher->getNumManifolds(); i < numManifolds; i++) {
		btPersistentManifold *manifold = dispatcher->getManifoldByIndexInternal(i);
		if (manifold->getNumContacts() == 0) {
			continue;
		}

		const btCollisionObject *col0 = manifold->getBody0();
		const btCollisionObject *col1 = manifold->getBody1();

		CcdPhysicsController *ctrl0 = static_cast<CcdPhysicsController *>(col0->getUserPointer());
		CcdPhysicsController *ctrl1 = static_cast<CcdPhysicsController *>(col1->getUserPointer());

		bool first;
		// Test if one of the controller is registered and use collision callback.
		if (ctrl0->Registered()) {
			first = true;
		}
		else if (ctrl1->Registered()) {
			first = false;
		}
		else {
			// No controllers registered for collision callbacks.
			continue;
		}

		const CcdCollData *coll_data = new CcdCollData(manifold);
		m_triggerCallbacks[PHY_OBJECT_RESPONSE](m_triggerCallbacksUserPtrs[PHY_OBJECT_RESPONSE], ctrl0, ctrl1, coll_data, first);
	}
}

PHY_CollisionTestResult CcdPhysicsEnvironment::CheckCollision(PHY_IPhysicsController *ctrl0, PHY_IPhysicsController *ctrl1)
{
	PHY_CollisionTestResult result{false, false, nullptr};

	btCollisionObject *col0 = static_cast<CcdPhysicsController *>(ctrl0)->GetCollisionObject();
	btCollisionObject *col1 = static_cast<CcdPhysicsController *>(ctrl1)->GetCollisionObject();

	if (!col0 || !col1) {
		return result;
	}

	btBroadphaseProxy *proxy0 = col0->getBroadphaseHandle();
	btBroadphaseProxy *proxy1 = col1->getBroadphaseHandle();

	btBroadphasePair *pair = m_dynamicsWorld->getPairCache()->findPair(proxy0, proxy1);

	if (!pair) {
		return result;
	}

	result.collide = true;

	if (pair->m_algorithm) {
		btManifoldArray manifoldArray;
		pair->m_algorithm->getAllContactManifolds(manifoldArray);
		btPersistentManifold *manifold = manifoldArray[0];

		result.isFirst = (col0 == manifold->getBody0());
		result.collData = new CcdCollData(manifold);
	}

	return result;
}

// This call back is called before a pair is added in the cache
// Handy to remove objects that must be ignored by sensors
bool CcdOverlapFilterCallBack::needBroadphaseCollision(btBroadphaseProxy *proxy0, btBroadphaseProxy *proxy1) const
{
	btCollisionObject *colObj0 = (btCollisionObject *)proxy0->m_clientObject;
	btCollisionObject *colObj1 = (btCollisionObject *)proxy1->m_clientObject;

	if (!colObj0 || !colObj1) {
		return false;
	}

	CcdPhysicsController *ctrl0 = static_cast<CcdPhysicsController *>(colObj0->getUserPointer());
	CcdPhysicsController *ctrl1 = static_cast<CcdPhysicsController *>(colObj1->getUserPointer());

	if (!((proxy0->m_collisionFilterGroup & proxy1->m_collisionFilterMask) &&
	      (proxy1->m_collisionFilterGroup & proxy0->m_collisionFilterMask) &&
	      (ctrl0->GetCollisionGroup() & ctrl1->GetCollisionMask()) &&
	      (ctrl1->GetCollisionGroup() & ctrl0->GetCollisionMask()))) {
		return false;
	}

	CcdPhysicsController *sensorCtrl, *objCtrl;
	// additional check for sensor object
	if (proxy0->m_collisionFilterGroup & btBroadphaseProxy::SensorTrigger) {
		// this is a sensor object, the other one can't be a sensor object because
		// they exclude each other in the above test
		BLI_assert(!(proxy1->m_collisionFilterGroup & btBroadphaseProxy::SensorTrigger));
		sensorCtrl = ctrl0;
		objCtrl = ctrl1;
	}
	else if (proxy1->m_collisionFilterGroup & btBroadphaseProxy::SensorTrigger) {
		sensorCtrl = ctrl1;
		objCtrl = ctrl0;
	}
	else {
		return true;
	}

	if (m_physEnv->m_triggerCallbacks[PHY_BROADPH_RESPONSE]) {
		return m_physEnv->m_triggerCallbacks[PHY_BROADPH_RESPONSE](m_physEnv->m_triggerCallbacksUserPtrs[PHY_BROADPH_RESPONSE], sensorCtrl, objCtrl, nullptr, false);
	}
	return true;
}

//complex constraint for vehicles
PHY_IVehicle *CcdPhysicsEnvironment::GetVehicleConstraint(int constraintId)
{
	int i;

	int numVehicles = m_wrapperVehicles.size();
	for (i = 0; i < numVehicles; i++) {
		WrapperVehicle *wrapperVehicle = m_wrapperVehicles[i];
		if (wrapperVehicle->GetVehicle()->getUserConstraintId() == constraintId) {
			return wrapperVehicle;
		}
	}

	return nullptr;
}

PHY_ICharacter *CcdPhysicsEnvironment::GetCharacterController(KX_GameObject *ob)
{
	CcdPhysicsController *controller = (CcdPhysicsController *)ob->GetPhysicsController();
	return (controller) ? static_cast<CcdCharacter *>(controller->GetCharacterController()) : nullptr;
}


PHY_IPhysicsController *CcdPhysicsEnvironment::CreateSphereController(float radius, const mt::vec3& position)
{
	CcdConstructionInfo cinfo;
	memset(&cinfo, 0, sizeof(cinfo)); // avoid uninitialized values
	cinfo.m_collisionShape = new btSphereShape(radius); // memory leak! The shape is not deleted by Bullet and we cannot add it to the KX_Scene.m_shapes list
	cinfo.m_MotionState = nullptr;
	cinfo.m_physicsEnv = this;
	// declare this object as Dyamic rather than static!!
	// The reason as it is designed to detect all type of object, including static object
	// It would cause static-static message to be printed on the console otherwise
	cinfo.m_collisionFlags |= btCollisionObject::CF_NO_CONTACT_RESPONSE | btCollisionObject::CF_STATIC_OBJECT;
	DefaultMotionState *motionState = new DefaultMotionState();
	cinfo.m_MotionState = motionState;
	// we will add later the possibility to select the filter from option
	cinfo.m_collisionFilterMask = CcdConstructionInfo::AllFilter ^ CcdConstructionInfo::SensorFilter;
	cinfo.m_collisionFilterGroup = CcdConstructionInfo::SensorFilter;
	cinfo.m_collisionGroup = 0xFFFF;
	cinfo.m_collisionMask = 0xFFFF;
	cinfo.m_bSensor = true;
	motionState->m_worldTransform.setIdentity();
	motionState->m_worldTransform.setOrigin(ToBullet(position));

	CcdPhysicsController *sphereController = new CcdPhysicsController(cinfo);

	return sphereController;
}

int Ccd_FindClosestNode(btSoftBody *sb, const btVector3& worldPoint)
{
	int node = -1;

	btSoftBody::tNodeArray&   nodes(sb->m_nodes);
	float maxDistSqr = 1e30f;

	for (int n = 0; n < nodes.size(); n++) {
		btScalar distSqr = (nodes[n].m_x - worldPoint).length2();
		if (distSqr < maxDistSqr) {
			maxDistSqr = distSqr;
			node = n;
		}
	}
	return node;
}

PHY_IConstraint *CcdPhysicsEnvironment::CreateConstraint(class PHY_IPhysicsController *ctrl0, class PHY_IPhysicsController *ctrl1, PHY_ConstraintType type,
																 float pivotX, float pivotY, float pivotZ,
																 float axisX, float axisY, float axisZ,
																 float axis1X, float axis1Y, float axis1Z,
																 float axis2X, float axis2Y, float axis2Z, int flags)
{
	bool disableCollisionBetweenLinkedBodies = (0 != (flags & CCD_CONSTRAINT_DISABLE_LINKED_COLLISION));

	CcdPhysicsController *c0 = (CcdPhysicsController *)ctrl0;
	CcdPhysicsController *c1 = (CcdPhysicsController *)ctrl1;

	btRigidBody *rb0 = c0 ? c0->GetRigidBody() : nullptr;
	btRigidBody *rb1 = c1 ? c1->GetRigidBody() : nullptr;

	bool rb0static = rb0 ? rb0->isStaticOrKinematicObject() : true;
	bool rb1static = rb1 ? rb1->isStaticOrKinematicObject() : true;

	btCollisionObject *colObj0 = c0->GetCollisionObject();
	if (!colObj0) {
		return nullptr;
	}

	btVector3 pivotInA(pivotX, pivotY, pivotZ);

	//it might be a soft body, let's try
	btSoftBody *sb0 = c0 ? c0->GetSoftBody() : nullptr;
	btSoftBody *sb1 = c1 ? c1->GetSoftBody() : nullptr;
	if (sb0 && sb1) {
		//not between two soft bodies?
		return nullptr;
	}

	if (sb0) {
		//either cluster or node attach, let's find closest node first
		//the soft body doesn't have a 'real' world transform, so get its initial world transform for now
		btVector3 pivotPointSoftWorld = sb0->m_initialWorldTransform(pivotInA);
		int node = Ccd_FindClosestNode(sb0, pivotPointSoftWorld);
		if (node >= 0) {
			if (rb1) {
				sb0->appendAnchor(node, rb1, disableCollisionBetweenLinkedBodies);
			}
			else {
				sb0->setMass(node, 0.0f);
			}
		}
		return nullptr;//can't remove soft body anchors yet
	}

	if (sb1) {
		btVector3 pivotPointAWorld = colObj0->getWorldTransform()(pivotInA);
		int node = Ccd_FindClosestNode(sb1, pivotPointAWorld);
		if (node >= 0) {
			if (rb0) {
				sb1->appendAnchor(node, rb0, disableCollisionBetweenLinkedBodies);
			}
			else {
				sb1->setMass(node, 0.0f);
			}
		}
		return nullptr;//can't remove soft body anchors yet
	}

	if (rb0static && rb1static) {
		return nullptr;
	}


	if (!rb0) {
		return nullptr;
	}

	// If either of the controllers is missing, we can't do anything.
	if (!c0 || !c1) {
		return nullptr;
	}

	btTypedConstraint *con = nullptr;

	btVector3 pivotInB = rb1 ? rb1->getCenterOfMassTransform().inverse()(rb0->getCenterOfMassTransform()(pivotInA)) :
	                     rb0->getCenterOfMassTransform() * pivotInA;
	btVector3 axisInA(axisX, axisY, axisZ);


	bool angularOnly = false;

	switch (type) {
		case PHY_POINT2POINT_CONSTRAINT:
		{
			btPoint2PointConstraint *p2p = nullptr;

			if (rb1) {
				p2p = new btPoint2PointConstraint(*rb0, *rb1, pivotInA, pivotInB);
			}
			else {
				p2p = new btPoint2PointConstraint(*rb0, pivotInA);
			}

			con = p2p;

			break;
		}

		case PHY_GENERIC_6DOF_CONSTRAINT:
		{
			btGeneric6DofConstraint *genericConstraint = nullptr;

			if (rb1) {
				btTransform frameInA;
				btTransform frameInB;

				btVector3 axis1(axis1X, axis1Y, axis1Z), axis2(axis2X, axis2Y, axis2Z);
				if (axis1.length() == 0.0) {
					btPlaneSpace1(axisInA, axis1, axis2);
				}

				frameInA.getBasis().setValue(axisInA.x(), axis1.x(), axis2.x(),
				                             axisInA.y(), axis1.y(), axis2.y(),
				                             axisInA.z(), axis1.z(), axis2.z());
				frameInA.setOrigin(pivotInA);

				btTransform inv = rb1->getCenterOfMassTransform().inverse();

				btTransform globalFrameA = rb0->getCenterOfMassTransform() * frameInA;

				frameInB = inv * globalFrameA;
				bool useReferenceFrameA = true;

				genericConstraint = new btGeneric6DofSpringConstraint(
					*rb0, *rb1,
					frameInA, frameInB, useReferenceFrameA);
			}
			else {
				static btRigidBody s_fixedObject2(0.0f, nullptr, nullptr);
				btTransform frameInA;
				btTransform frameInB;

				btVector3 axis1, axis2;
				btPlaneSpace1(axisInA, axis1, axis2);

				frameInA.getBasis().setValue(axisInA.x(), axis1.x(), axis2.x(),
				                             axisInA.y(), axis1.y(), axis2.y(),
				                             axisInA.z(), axis1.z(), axis2.z());

				frameInA.setOrigin(pivotInA);

				///frameInB in worldspace
				frameInB = rb0->getCenterOfMassTransform() * frameInA;

				bool useReferenceFrameA = true;
				genericConstraint = new btGeneric6DofSpringConstraint(
					*rb0, s_fixedObject2,
					frameInA, frameInB, useReferenceFrameA);
			}

			con = genericConstraint;

			break;
		}
		case PHY_CONE_TWIST_CONSTRAINT:
		{
			btConeTwistConstraint *coneTwistContraint = nullptr;

			if (rb1) {
				btTransform frameInA;
				btTransform frameInB;

				btVector3 axis1(axis1X, axis1Y, axis1Z), axis2(axis2X, axis2Y, axis2Z);
				if (axis1.length() == 0.0) {
					btPlaneSpace1(axisInA, axis1, axis2);
				}

				frameInA.getBasis().setValue(axisInA.x(), axis1.x(), axis2.x(),
				                             axisInA.y(), axis1.y(), axis2.y(),
				                             axisInA.z(), axis1.z(), axis2.z());
				frameInA.setOrigin(pivotInA);

				btTransform inv = rb1->getCenterOfMassTransform().inverse();

				btTransform globalFrameA = rb0->getCenterOfMassTransform() * frameInA;

				frameInB = inv * globalFrameA;

				coneTwistContraint = new btConeTwistConstraint(*rb0, *rb1,
				                                               frameInA, frameInB);
			}
			else {
				static btRigidBody s_fixedObject2(0.0f, nullptr, nullptr);
				btTransform frameInA;
				btTransform frameInB;

				btVector3 axis1, axis2;
				btPlaneSpace1(axisInA, axis1, axis2);

				frameInA.getBasis().setValue(axisInA.x(), axis1.x(), axis2.x(),
				                             axisInA.y(), axis1.y(), axis2.y(),
				                             axisInA.z(), axis1.z(), axis2.z());

				frameInA.setOrigin(pivotInA);

				///frameInB in worldspace
				frameInB = rb0->getCenterOfMassTransform() * frameInA;

				coneTwistContraint = new btConeTwistConstraint(
					*rb0, s_fixedObject2,
					frameInA, frameInB);
			}

			con = coneTwistContraint;

			break;
		}
		case PHY_ANGULAR_CONSTRAINT:
		{
			angularOnly = true;

		}
		case PHY_LINEHINGE_CONSTRAINT:
		{
			btHingeConstraint *hinge = nullptr;

			if (rb1) {
				// We know the orientations so we should use them instead of
				// having btHingeConstraint fill in the blanks any way it wants to.
				btTransform frameInA;
				btTransform frameInB;

				btVector3 axis1(axis1X, axis1Y, axis1Z), axis2(axis2X, axis2Y, axis2Z);
				if (axis1.length() == 0.0f) {
					btPlaneSpace1(axisInA, axis1, axis2);
				}

				// Internally btHingeConstraint's hinge-axis is z
				frameInA.getBasis().setValue(axis1.x(), axis2.x(), axisInA.x(),
				                             axis1.y(), axis2.y(), axisInA.y(),
				                             axis1.z(), axis2.z(), axisInA.z());

				frameInA.setOrigin(pivotInA);

				btTransform inv = rb1->getCenterOfMassTransform().inverse();

				btTransform globalFrameA = rb0->getCenterOfMassTransform() * frameInA;

				frameInB = inv  * globalFrameA;

				hinge = new btHingeConstraint(*rb0, *rb1, frameInA, frameInB);
			}
			else {
				static btRigidBody s_fixedObject2(0.0f, nullptr, nullptr);

				btTransform frameInA;
				btTransform frameInB;

				btVector3 axis1(axis1X, axis1Y, axis1Z), axis2(axis2X, axis2Y, axis2Z);
				if (axis1.length() == 0.0f) {
					btPlaneSpace1(axisInA, axis1, axis2);
				}

				// Internally btHingeConstraint's hinge-axis is z
				frameInA.getBasis().setValue(axis1.x(), axis2.x(), axisInA.x(),
				                             axis1.y(), axis2.y(), axisInA.y(),
				                             axis1.z(), axis2.z(), axisInA.z());
				frameInA.setOrigin(pivotInA);
				frameInB = rb0->getCenterOfMassTransform() * frameInA;

				hinge = new btHingeConstraint(*rb0, s_fixedObject2, frameInA, frameInB);
			}
			hinge->setAngularOnly(angularOnly);

			con = hinge;

			break;
		}
		default:
		{
		}
	}
	;

	if (!con) {
		return nullptr;
	}

	c0->addCcdConstraintRef(con);
	c1->addCcdConstraintRef(con);
	con->setUserConstraintId(gConstraintUid++);
	con->setUserConstraintType(type);
	CcdConstraint *constraintData = new CcdConstraint(con, disableCollisionBetweenLinkedBodies);
	con->setUserConstraintPtr(constraintData);
	m_dynamicsWorld->addConstraint(con, disableCollisionBetweenLinkedBodies);

	return constraintData;
}

PHY_IVehicle *CcdPhysicsEnvironment::CreateVehicle(PHY_IPhysicsController *ctrl)
{
	const btRaycastVehicle::btVehicleTuning tuning = btRaycastVehicle::btVehicleTuning();
	BlenderVehicleRaycaster *raycaster = new BlenderVehicleRaycaster(m_dynamicsWorld);
	btRaycastVehicle *vehicle = new btRaycastVehicle(tuning, ((CcdPhysicsController *)ctrl)->GetRigidBody(), raycaster);
	WrapperVehicle *wrapperVehicle = new WrapperVehicle(vehicle, raycaster, ctrl);
	m_wrapperVehicles.push_back(wrapperVehicle);

	m_dynamicsWorld->addVehicle(vehicle);

	vehicle->setUserConstraintId(gConstraintUid++);
	vehicle->setUserConstraintType(PHY_VEHICLE_CONSTRAINT);

	return wrapperVehicle;
}

PHY_IPhysicsController *CcdPhysicsEnvironment::CreateConeController(float coneradius, float coneheight)
{
	CcdConstructionInfo cinfo;
	//don't memset cinfo: this is C++ and values should be set in the constructor!

	// we don't need a CcdShapeConstructionInfo for this shape:
	// it is simple enough for the standard copy constructor (see CcdPhysicsController::GetReplica)
	cinfo.m_collisionShape = new btConeShape(coneradius, coneheight);
	cinfo.m_MotionState = nullptr;
	cinfo.m_physicsEnv = this;
	cinfo.m_collisionFlags |= btCollisionObject::CF_NO_CONTACT_RESPONSE | btCollisionObject::CF_STATIC_OBJECT;
	DefaultMotionState *motionState = new DefaultMotionState();
	cinfo.m_MotionState = motionState;

	// we will add later the possibility to select the filter from option
	cinfo.m_collisionFilterMask = CcdConstructionInfo::AllFilter ^ CcdConstructionInfo::SensorFilter;
	cinfo.m_collisionFilterGroup = CcdConstructionInfo::SensorFilter;
	cinfo.m_bSensor = true;
	motionState->m_worldTransform.setIdentity();
//	motionState->m_worldTransform.setOrigin(btVector3(position[0],position[1],position[2]));

	CcdPhysicsController *sphereController = new CcdPhysicsController(cinfo);

	return sphereController;
}

float CcdPhysicsEnvironment::getAppliedImpulse(int constraintid)
{
	// For soft body constraints
	if (constraintid == 0) {
		return 0.0f;
	}

	int i;
	int numConstraints = m_dynamicsWorld->getNumConstraints();
	for (i = 0; i < numConstraints; i++) {
		btTypedConstraint *constraint = m_dynamicsWorld->getConstraint(i);
		if (constraint->getUserConstraintId() == constraintid) {
			return constraint->getAppliedImpulse();
		}
	}

	return 0.0f;
}

void CcdPhysicsEnvironment::ExportFile(const std::string& filename)
{
	btDefaultSerializer *serializer = new btDefaultSerializer();

	for (int i = 0; i < m_dynamicsWorld->getNumCollisionObjects(); i++) {
		btCollisionObject *colObj = m_dynamicsWorld->getCollisionObjectArray()[i];

		CcdPhysicsController *controller = static_cast<CcdPhysicsController *>(colObj->getUserPointer());
		if (controller) {
			const std::string name = KX_GameObject::GetClientObject((KX_ClientObjectInfo *)controller->GetNewClientInfo())->GetName();
			if (!name.empty()) {
				serializer->registerNameForPointer(colObj, name.c_str());
			}
		}
	}

	m_dynamicsWorld->serialize(serializer);

	FILE *file = fopen(filename.c_str(), "wb");
	if (file) {
		fwrite(serializer->getBufferPointer(), serializer->getCurrentBufferSize(), 1, file);
		fclose(file);
	}
}

struct BlenderDebugDraw : public btIDebugDraw {
	BlenderDebugDraw()
		:m_debugMode(0)
	{
	}

	int m_debugMode;

	virtual void drawLine(const btVector3& from, const btVector3& to, const btVector3& color)
	{
		if (m_debugMode > 0) {
			KX_RasterizerDrawDebugLine(ToMt(from), ToMt(to), mt::vec4(color.x(), color.y(), color.z(), 1.0f));
		}
	}

	virtual void reportErrorWarning(const char *warningString)
	{
	}

	virtual void drawContactPoint(const btVector3& PointOnB, const btVector3& normalOnB, float distance, int lifeTime, const btVector3& color)
	{
		drawLine(PointOnB, PointOnB + normalOnB, color);
		drawSphere(PointOnB, 0.1f, color);
	}

	virtual void setDebugMode(int debugMode)
	{
		m_debugMode = debugMode;
	}
	virtual int getDebugMode() const
	{
		return m_debugMode;
	}
	///todo: find out if Blender can do this
	virtual void draw3dText(const btVector3& location, const char *textString)
	{
	}
};

CcdPhysicsEnvironment *CcdPhysicsEnvironment::Create(Scene *blenderscene, bool visualizePhysics)
{
	static const PHY_SolverType solverTypeTable[] = {
		PHY_SOLVER_SEQUENTIAL, // GAME_SOLVER_SEQUENTIAL
		PHY_SOLVER_NNCG, // GAME_SOLVER_NNGC
		PHY_SOLVER_MLCP_DANTZIG, // GAME_SOLVER_MLCP_DANTZIG
		PHY_SOLVER_MLCP_LEMKE // GAME_SOLVER_MLCP_LEMKE
	};

	CcdPhysicsEnvironment *ccdPhysEnv = new CcdPhysicsEnvironment(solverTypeTable[blenderscene->gm.solverType],
	                                                              (blenderscene->gm.mode & WO_DBVT_CULLING) != 0);

	ccdPhysEnv->SetDebugDrawer(new BlenderDebugDraw());
	ccdPhysEnv->SetDeactivationLinearTreshold(blenderscene->gm.lineardeactthreshold);
	ccdPhysEnv->SetDeactivationAngularTreshold(blenderscene->gm.angulardeactthreshold);
	ccdPhysEnv->SetDeactivationTime(blenderscene->gm.deactivationtime);

	if (visualizePhysics) {
		ccdPhysEnv->SetDebugMode(btIDebugDraw::DBG_DrawWireframe | btIDebugDraw::DBG_DrawAabb | btIDebugDraw::DBG_DrawContactPoints |
		                         btIDebugDraw::DBG_DrawText | btIDebugDraw::DBG_DrawConstraintLimits | btIDebugDraw::DBG_DrawConstraints);
	}

	return ccdPhysEnv;
}

void CcdPhysicsEnvironment::ConvertObject(BL_SceneConverter& converter, KX_GameObject *gameobj, RAS_Mesh *meshobj,
                                          KX_Scene *kxscene, PHY_IMotionState *motionstate,
                                          int activeLayerBitInfo, bool isCompoundChild, bool hasCompoundChildren)
{
	Object *blenderobject = gameobj->GetBlenderObject();

	bool isbulletdyna = (blenderobject->gameflag & OB_DYNAMIC) != 0;
	bool isbulletsensor = (blenderobject->gameflag & OB_SENSOR) != 0;
	bool isbulletchar = (blenderobject->gameflag & OB_CHARACTER) != 0;
	bool isbulletsoftbody = (blenderobject->gameflag & OB_SOFT_BODY) != 0;
	bool isbulletrigidbody = (blenderobject->gameflag & OB_RIGID_BODY) != 0;
	bool useGimpact = false;
	CcdConstructionInfo ci;
	class CcdShapeConstructionInfo *shapeInfo = new CcdShapeConstructionInfo();

	Object *blenderRoot = blenderobject->parent;
	Object *blenderCompoundRoot = nullptr;
	// Iterate over all parents in the object tree.
	{
		Object *parentit = blenderobject->parent;
		while (parentit) {
			// If the parent is valid for compound parent shape, update blenderCompoundRoot.
			if ((parentit->gameflag & OB_CHILD) && (blenderobject->gameflag & (OB_COLLISION | OB_DYNAMIC | OB_RIGID_BODY)) &&
			    !(blenderobject->gameflag & OB_SOFT_BODY)) {
				blenderCompoundRoot = parentit;
			}
			// Continue looking for root parent.
			blenderRoot = parentit;

			parentit = parentit->parent;
		}
	}

	KX_GameObject *compoundParent = nullptr;
	if (blenderCompoundRoot) {
		compoundParent = converter.FindGameObject(blenderCompoundRoot);
		isbulletsoftbody = false;
	}

	KX_GameObject *parentRoot = nullptr;
	if (blenderRoot) {
		parentRoot = converter.FindGameObject(blenderRoot);
		isbulletsoftbody = false;
	}

	if (!isbulletdyna) {
		ci.m_collisionFlags |= btCollisionObject::CF_STATIC_OBJECT;
	}
	if ((blenderobject->gameflag & (OB_GHOST | OB_SENSOR | OB_CHARACTER)) != 0) {
		ci.m_collisionFlags |= btCollisionObject::CF_NO_CONTACT_RESPONSE;
	}

	ci.m_collisionGroup = blenderobject->col_group;
	ci.m_collisionMask = blenderobject->col_mask;

	ci.m_MotionState = motionstate;
	ci.m_gravity = btVector3(0.0f, 0.0f, 0.0f);
	ci.m_linearFactor = btVector3(((blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_X_AXIS) != 0) ? 0.0f : 1.0f,
	                              ((blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_Y_AXIS) != 0) ? 0.0f : 1.0f,
	                              ((blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_Z_AXIS) != 0) ? 0.0f : 1.0f);
	ci.m_angularFactor = btVector3(((blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_X_ROT_AXIS) != 0) ? 0.0f : 1.0f,
	                               ((blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_Y_ROT_AXIS) != 0) ? 0.0f : 1.0f,
	                               ((blenderobject->gameflag2 & OB_LOCK_RIGID_BODY_Z_ROT_AXIS) != 0) ? 0.0f : 1.0f);
	ci.m_localInertiaTensor = btVector3(0.0f, 0.0f, 0.0f);
	ci.m_mass = isbulletdyna ? blenderobject->mass : 0.0f;
	ci.m_clamp_vel_min = blenderobject->min_vel;
	ci.m_clamp_vel_max = blenderobject->max_vel;
	ci.m_clamp_angvel_min = blenderobject->min_angvel;
	ci.m_clamp_angvel_max = blenderobject->max_angvel;
	ci.m_stepHeight = isbulletchar ? blenderobject->step_height : 0.0f;
	ci.m_jumpSpeed = isbulletchar ? blenderobject->jump_speed : 0.0f;
	ci.m_fallSpeed = isbulletchar ? blenderobject->fall_speed : 0.0f;
	ci.m_maxSlope = isbulletchar ? blenderobject->max_slope : 0.0f;
	ci.m_maxJumps = isbulletchar ? blenderobject->max_jumps : 0;

	//mmm, for now, take this for the size of the dynamicobject
	// Blender uses inertia for radius of dynamic object
	shapeInfo->m_radius = ci.m_radius = blenderobject->inertia;
	useGimpact = ((isbulletdyna || isbulletsensor) && !isbulletsoftbody);

	if (isbulletsoftbody) {
		if (blenderobject->bsoft) {
			ci.m_margin = blenderobject->bsoft->margin;
			ci.m_gamesoftFlag = blenderobject->bsoft->flag;

			ci.m_softBendingDistance = blenderobject->bsoft->bending_dist;

			ci.m_soft_linStiff = blenderobject->bsoft->linStiff;
			ci.m_soft_angStiff = blenderobject->bsoft->angStiff; // angular stiffness 0..1
			ci.m_soft_volume = blenderobject->bsoft->volume; // volume preservation 0..1

			ci.m_soft_viterations = blenderobject->bsoft->viterations; // Velocities solver iterations
			ci.m_soft_piterations = blenderobject->bsoft->piterations; // Positions solver iterations
			ci.m_soft_diterations = blenderobject->bsoft->diterations; // Drift solver iterations
			ci.m_soft_citerations = blenderobject->bsoft->citerations; // Cluster solver iterations

			ci.m_soft_kSRHR_CL = blenderobject->bsoft->kSRHR_CL; // Soft vs rigid hardness [0,1] (cluster only)
			ci.m_soft_kSKHR_CL = blenderobject->bsoft->kSKHR_CL; // Soft vs kinetic hardness [0,1] (cluster only)
			ci.m_soft_kSSHR_CL = blenderobject->bsoft->kSSHR_CL; // Soft vs soft hardness [0,1] (cluster only)
			ci.m_soft_kSR_SPLT_CL = blenderobject->bsoft->kSR_SPLT_CL; // Soft vs rigid impulse split [0,1] (cluster only)

			ci.m_soft_kSK_SPLT_CL = blenderobject->bsoft->kSK_SPLT_CL; // Soft vs rigid impulse split [0,1] (cluster only)
			ci.m_soft_kSS_SPLT_CL = blenderobject->bsoft->kSS_SPLT_CL; // Soft vs rigid impulse split [0,1] (cluster only)
			ci.m_soft_kVCF = blenderobject->bsoft->kVCF; // Velocities correction factor (Baumgarte)
			ci.m_soft_kDP = blenderobject->bsoft->kDP; // Damping coefficient [0,1]

			ci.m_soft_kDG = blenderobject->bsoft->kDG; // Drag coefficient [0,+inf]
			ci.m_soft_kLF = blenderobject->bsoft->kLF; // Lift coefficient [0,+inf]
			ci.m_soft_kPR = blenderobject->bsoft->kPR; // Pressure coefficient [-inf,+inf]
			ci.m_soft_kVC = blenderobject->bsoft->kVC; // Volume conversation coefficient [0,+inf]

			ci.m_soft_kDF = blenderobject->bsoft->kDF; // Dynamic friction coefficient [0,1]
			ci.m_soft_kMT = blenderobject->bsoft->kMT; // Pose matching coefficient [0,1]
			ci.m_soft_kCHR = blenderobject->bsoft->kCHR; // Rigid contacts hardness [0,1]
			ci.m_soft_kKHR = blenderobject->bsoft->kKHR; // Kinetic contacts hardness [0,1]

			ci.m_soft_kSHR = blenderobject->bsoft->kSHR; // Soft contacts hardness [0,1]
			ci.m_soft_kAHR = blenderobject->bsoft->kAHR; // Anchors hardness [0,1]
			ci.m_soft_collisionflags = blenderobject->bsoft->collisionflags; // Vertex/Face or Signed Distance Field(SDF) or Clusters, Soft versus Soft or Rigid
			ci.m_soft_numclusteriterations = blenderobject->bsoft->numclusteriterations; // number of iterations to refine collision clusters

		}
		else {
			ci.m_margin = 0.0f;
			ci.m_gamesoftFlag = OB_BSB_BENDING_CONSTRAINTS | OB_BSB_SHAPE_MATCHING | OB_BSB_AERO_VPOINT;

			ci.m_softBendingDistance = 2;

			ci.m_soft_linStiff = 0.5f;
			ci.m_soft_angStiff = 1.0f; // angular stiffness 0..1
			ci.m_soft_volume = 1.0f; // volume preservation 0..1

			ci.m_soft_viterations = 0;
			ci.m_soft_piterations = 1;
			ci.m_soft_diterations = 0;
			ci.m_soft_citerations = 4;

			ci.m_soft_kSRHR_CL = 0.1f;
			ci.m_soft_kSKHR_CL = 1.0f;
			ci.m_soft_kSSHR_CL = 0.5f;
			ci.m_soft_kSR_SPLT_CL = 0.5f;

			ci.m_soft_kSK_SPLT_CL = 0.5f;
			ci.m_soft_kSS_SPLT_CL = 0.5f;
			ci.m_soft_kVCF = 1;
			ci.m_soft_kDP = 0;

			ci.m_soft_kDG = 0;
			ci.m_soft_kLF = 0;
			ci.m_soft_kPR = 0;
			ci.m_soft_kVC = 0;

			ci.m_soft_kDF = 0.2f;
			ci.m_soft_kMT = 0.05f;
			ci.m_soft_kCHR = 1.0f;
			ci.m_soft_kKHR = 0.1f;

			ci.m_soft_kSHR = 1.0f;
			ci.m_soft_kAHR = 0.7f;
			ci.m_soft_collisionflags = OB_BSB_COL_SDF_RS + OB_BSB_COL_VF_SS;
			ci.m_soft_numclusteriterations = 16;
		}
	}
	else {
		ci.m_margin = blenderobject->margin;
	}

	ci.m_localInertiaTensor = btVector3(ci.m_mass / 3.0f, ci.m_mass / 3.0f, ci.m_mass / 3.0f);

	btCollisionShape *bm = nullptr;

	char bounds = isbulletdyna ? OB_BOUND_SPHERE : OB_BOUND_TRIANGLE_MESH;
	if (!(blenderobject->gameflag & OB_BOUNDS)) {
		if (blenderobject->gameflag & OB_SOFT_BODY) {
			bounds = OB_BOUND_TRIANGLE_MESH;
		}
		else if (blenderobject->gameflag & OB_CHARACTER) {
			bounds = OB_BOUND_SPHERE;
		}
	}
	else {
		if (ELEM(blenderobject->collision_boundtype, OB_BOUND_CONVEX_HULL, OB_BOUND_TRIANGLE_MESH)
		    && blenderobject->type != OB_MESH) {
			// Can't use triangle mesh or convex hull on a non-mesh object, fall-back to sphere
			bounds = OB_BOUND_SPHERE;
		}
		else {
			bounds = blenderobject->collision_boundtype;
		}
	}

	// Get bounds information
	float bounds_center[3], bounds_extends[3];
	BoundBox *bb = BKE_object_boundbox_get(blenderobject);
	if (bb == nullptr) {
		bounds_center[0] = bounds_center[1] = bounds_center[2] = 0.0f;
		bounds_extends[0] = bounds_extends[1] = bounds_extends[2] = 1.0f;
	}
	else {
		bounds_extends[0] = 0.5f * fabsf(bb->vec[0][0] - bb->vec[4][0]);
		bounds_extends[1] = 0.5f * fabsf(bb->vec[0][1] - bb->vec[2][1]);
		bounds_extends[2] = 0.5f * fabsf(bb->vec[0][2] - bb->vec[1][2]);

		bounds_center[0] = 0.5f * (bb->vec[0][0] + bb->vec[4][0]);
		bounds_center[1] = 0.5f * (bb->vec[0][1] + bb->vec[2][1]);
		bounds_center[2] = 0.5f * (bb->vec[0][2] + bb->vec[1][2]);
	}

	switch (bounds) {
		case OB_BOUND_SPHERE:
		{
			shapeInfo->m_shapeType = PHY_SHAPE_SPHERE;
			bm = shapeInfo->CreateBulletShape(ci.m_margin);
			break;
		}
		case OB_BOUND_BOX:
		{
			shapeInfo->m_halfExtend.setValue(
				2.0f * bounds_extends[0],
				2.0f * bounds_extends[1],
				2.0f * bounds_extends[2]);

			shapeInfo->m_halfExtend /= 2.0f;
			shapeInfo->m_halfExtend = shapeInfo->m_halfExtend.absolute();
			shapeInfo->m_shapeType = PHY_SHAPE_BOX;
			bm = shapeInfo->CreateBulletShape(ci.m_margin);
			break;
		}
		case OB_BOUND_CYLINDER:
		{
			float radius = std::max(bounds_extends[0], bounds_extends[1]);
			shapeInfo->m_halfExtend.setValue(
				radius,
				radius,
				bounds_extends[2]
				);
			shapeInfo->m_shapeType = PHY_SHAPE_CYLINDER;
			bm = shapeInfo->CreateBulletShape(ci.m_margin);
			break;
		}

		case OB_BOUND_CONE:
		{
			shapeInfo->m_radius = std::max(bounds_extends[0], bounds_extends[1]);
			shapeInfo->m_height = 2.0f * bounds_extends[2];
			shapeInfo->m_shapeType = PHY_SHAPE_CONE;
			bm = shapeInfo->CreateBulletShape(ci.m_margin);
			break;
		}
		case OB_BOUND_CONVEX_HULL:
		{
			// Convex shapes can be shared, check first if we already have a shape on that mesh.
			CcdShapeConstructionInfo *sharedShapeInfo = CcdShapeConstructionInfo::FindMesh(meshobj, gameobj->GetDeformer(), PHY_SHAPE_POLYTOPE);
			if (sharedShapeInfo) {
				shapeInfo->Release();
				shapeInfo = sharedShapeInfo;
				shapeInfo->AddRef();
			}
			else {
				shapeInfo->m_shapeType = PHY_SHAPE_POLYTOPE;
				// Update from deformer or mesh.
				shapeInfo->UpdateMesh(gameobj, nullptr);
			}

			bm = shapeInfo->CreateBulletShape(ci.m_margin);
			break;
		}
		case OB_BOUND_CAPSULE:
		{
			shapeInfo->m_radius = std::max(bounds_extends[0], bounds_extends[1]);
			shapeInfo->m_height = 2.0f * bounds_extends[2];
			if (shapeInfo->m_height < 0.0f) {
				shapeInfo->m_height = 0.0f;
			}
			shapeInfo->m_shapeType = PHY_SHAPE_CAPSULE;
			bm = shapeInfo->CreateBulletShape(ci.m_margin);
			break;
		}
		case OB_BOUND_TRIANGLE_MESH:
		{
			// Mesh shapes can be shared, check first if we already have a shape on that mesh.
			CcdShapeConstructionInfo *sharedShapeInfo = CcdShapeConstructionInfo::FindMesh(meshobj, gameobj->GetDeformer(), PHY_SHAPE_MESH);
			if (sharedShapeInfo) {
				shapeInfo->Release();
				shapeInfo = sharedShapeInfo;
				shapeInfo->AddRef();
			}
			else {
				shapeInfo->m_shapeType = PHY_SHAPE_MESH;
				// Update from deformer or mesh.
				shapeInfo->UpdateMesh(gameobj, nullptr);
			}

			// Soft bodies can benefit from welding, don't do it on non-soft bodies
			if (isbulletsoftbody) {
				// disable welding: it doesn't bring any additional stability and it breaks the relation between soft body collision shape and graphic mesh
				// shapeInfo->setVertexWeldingThreshold1((blenderobject->bsoft) ? blenderobject->bsoft->welding ? 0.f);
				shapeInfo->setVertexWeldingThreshold1(0.0f); //todo: expose this to the UI
			}

			bm = shapeInfo->CreateBulletShape(ci.m_margin, useGimpact, !isbulletsoftbody);
			//should we compute inertia for dynamic shape?
			//bm->calculateLocalInertia(ci.m_mass,ci.m_localInertiaTensor);

			break;
		}
		case OB_BOUND_EMPTY:
		{
			shapeInfo->m_shapeType = PHY_SHAPE_EMPTY;
			bm = shapeInfo->CreateBulletShape(ci.m_margin);
			break;
		}
	}

	if (!bm) {
		delete motionstate;
		shapeInfo->Release();
		return;
	}

	if (isCompoundChild) {
		//find parent, compound shape and add to it
		//take relative transform into account!
		CcdPhysicsController *parentCtrl = (CcdPhysicsController *)compoundParent->GetPhysicsController();
		BLI_assert(parentCtrl);

		// only makes compound shape if parent has a physics controller (i.e not an empty, etc)
		if (parentCtrl) {
			CcdShapeConstructionInfo *parentShapeInfo = parentCtrl->GetShapeInfo();
			btRigidBody *rigidbody = parentCtrl->GetRigidBody();
			btCollisionShape *colShape = rigidbody->getCollisionShape();
			BLI_assert(colShape->isCompound());
			btCompoundShape *compoundShape = (btCompoundShape *)colShape;

			// compute the local transform from parent, this may include several node in the chain
			SG_Node *gameNode = gameobj->GetNode();
			SG_Node *parentNode = compoundParent->GetNode();
			// relative transform
			mt::vec3 parentScale = parentNode->GetWorldScaling();
			parentScale[0] = 1.0f / parentScale[0];
			parentScale[1] = 1.0f / parentScale[1];
			parentScale[2] = 1.0f / parentScale[2];
			mt::vec3 relativeScale = gameNode->GetWorldScaling() * parentScale;
			mt::mat3 parentInvRot = parentNode->GetWorldOrientation().Transpose();
			mt::vec3 relativePos = parentInvRot * ((gameNode->GetWorldPosition() - parentNode->GetWorldPosition()) * parentScale);
			mt::mat3 relativeRot = parentInvRot * gameNode->GetWorldOrientation();

			shapeInfo->m_childScale = ToBullet(relativeScale);
			bm->setLocalScaling(shapeInfo->m_childScale);
			shapeInfo->m_childTrans.setOrigin(ToBullet(relativePos));
			shapeInfo->m_childTrans.setBasis(ToBullet(relativeRot));

			parentShapeInfo->AddShape(shapeInfo);
			compoundShape->addChildShape(shapeInfo->m_childTrans, bm);

			// Recalculate inertia for object owning compound shape.
			if (!rigidbody->isStaticOrKinematicObject()) {
				btVector3 localInertia;
				const float mass = 1.0f / rigidbody->getInvMass();
				compoundShape->calculateLocalInertia(mass, localInertia);
				rigidbody->setMassProps(mass, localInertia * parentCtrl->GetInertiaFactor());
			}
			shapeInfo->Release();
			// delete motionstate as it's not used
			delete motionstate;
		}
		return;
	}

	if (hasCompoundChildren) {
		// create a compound shape info
		CcdShapeConstructionInfo *compoundShapeInfo = new CcdShapeConstructionInfo();
		compoundShapeInfo->m_shapeType = PHY_SHAPE_COMPOUND;
		compoundShapeInfo->AddShape(shapeInfo);
		// create the compound shape manually as we already have the child shape
		btCompoundShape *compoundShape = new btCompoundShape();
		compoundShape->addChildShape(shapeInfo->m_childTrans, bm);
		// now replace the shape
		bm = compoundShape;
		shapeInfo->Release();
		shapeInfo = compoundShapeInfo;
	}

#ifdef TEST_SIMD_HULL
	if (bm->IsPolyhedral()) {
		PolyhedralConvexShape *polyhedron = static_cast<PolyhedralConvexShape *>(bm);
		if (!polyhedron->m_optionalHull) {
			//first convert vertices in 'Point3' format
			int numPoints = polyhedron->GetNumVertices();
			Point3 *points = new Point3[numPoints + 1];
			//first 4 points should not be co-planar, so add central point to satisfy MakeHull
			points[0] = Point3(0.0f, 0.0f, 0.0f);

			btVector3 vertex;
			for (int p = 0; p < numPoints; p++) {
				polyhedron->GetVertex(p, vertex);
				points[p + 1] = Point3(vertex.getX(), vertex.getY(), vertex.getZ());
			}

			Hull *hull = Hull::MakeHull(numPoints + 1, points);
			polyhedron->m_optionalHull = hull;
		}
	}
#endif //TEST_SIMD_HULL


	ci.m_collisionShape = bm;
	ci.m_shapeInfo = shapeInfo;
	ci.m_friction = blenderobject->friction;
	ci.m_rollingFriction = blenderobject->rolling_friction;
	ci.m_restitution = blenderobject->reflect;
	ci.m_physicsEnv = this;
	ci.m_linearDamping = blenderobject->damping;
	ci.m_angularDamping = blenderobject->rdamping;
	//need a bit of damping, else system doesn't behave well
	ci.m_inertiaFactor = blenderobject->formfactor / 0.4f;//defaults to 0.4, don't want to change behavior

	ci.m_do_anisotropic = (blenderobject->gameflag & OB_ANISOTROPIC_FRICTION);
	ci.m_anisotropicFriction = btVector3(
		blenderobject->anisotropicFriction[0],
		blenderobject->anisotropicFriction[1],
		blenderobject->anisotropicFriction[2]);

	//do Fh, do Rot Fh
	ci.m_do_fh = (blenderobject->gameflag & OB_DO_FH);
	ci.m_do_rot_fh = (blenderobject->gameflag & OB_ROT_FH);
	ci.m_fh_damping = blenderobject->xyfrict;
	ci.m_fh_distance = blenderobject->fhdist;
	ci.m_fh_normal = (blenderobject->dynamode & OB_FH_NOR);
	ci.m_fh_spring = blenderobject->fh;

	ci.m_collisionFilterGroup =
		(isbulletsensor) ? short(CcdConstructionInfo::SensorFilter) :
		(isbulletdyna) ? short(CcdConstructionInfo::DynamicFilter) :
		(isbulletchar) ? short(CcdConstructionInfo::CharacterFilter) :
		short(CcdConstructionInfo::StaticFilter);
	ci.m_collisionFilterMask =
		(isbulletsensor) ? short(CcdConstructionInfo::AllFilter ^ CcdConstructionInfo::SensorFilter) :
		(isbulletdyna) ? short(CcdConstructionInfo::AllFilter) :
		(isbulletchar) ? short(CcdConstructionInfo::AllFilter) :
		short(CcdConstructionInfo::AllFilter ^ CcdConstructionInfo::StaticFilter);
	ci.m_bRigid = isbulletdyna && isbulletrigidbody;
	ci.m_bSoft = isbulletsoftbody;
	ci.m_bDyna = isbulletdyna;
	ci.m_bSensor = isbulletsensor;
	ci.m_bCharacter = isbulletchar;
	ci.m_bGimpact = useGimpact;
	mt::vec3 scaling = gameobj->NodeGetWorldScaling();
	ci.m_scaling.setValue(scaling[0], scaling[1], scaling[2]);
	CcdPhysicsController *physicscontroller = new CcdPhysicsController(ci);
	// shapeInfo is reference counted, decrement now as we don't use it anymore
	if (shapeInfo) {
		shapeInfo->Release();
	}

	gameobj->SetPhysicsController(physicscontroller);

	physicscontroller->SetNewClientInfo(&gameobj->GetClientInfo());

	// don't add automatically sensor object, they are added when a collision sensor is registered
	if (!isbulletsensor && (blenderobject->lay & activeLayerBitInfo) != 0) {
		this->AddCcdPhysicsController(physicscontroller);
	}

	{
		btRigidBody *rbody = physicscontroller->GetRigidBody();

		if (rbody) {
			rbody->setLinearFactor(ci.m_linearFactor);

			if (isbulletrigidbody) {
				rbody->setAngularFactor(ci.m_angularFactor);
			}

			if (rbody && (blenderobject->gameflag & OB_COLLISION_RESPONSE) != 0) {
				rbody->setActivationState(DISABLE_DEACTIVATION);
			}
		}
	}

	if (parentRoot) {
		physicscontroller->SuspendDynamics(false);
	}

	CcdPhysicsController *parentCtrl = parentRoot ? static_cast<CcdPhysicsController *>(parentRoot->GetPhysicsController()) : nullptr;
	physicscontroller->SetParentRoot(parentCtrl);
}

void CcdPhysicsEnvironment::SetupObjectConstraints(KX_GameObject *obj_src, KX_GameObject *obj_dest,
                                                   bRigidBodyJointConstraint *dat)
{
	PHY_IPhysicsController *phy_src = obj_src->GetPhysicsController();
	PHY_IPhysicsController *phy_dest = obj_dest->GetPhysicsController();
	PHY_IPhysicsEnvironment *phys_env = obj_src->GetScene()->GetPhysicsEnvironment();

	/* We need to pass a full constraint frame, not just axis. */
	mt::mat3 localCFrame(mt::vec3(dat->axX, dat->axY, dat->axZ));
	mt::vec3 axis0 = localCFrame.GetColumn(0);
	mt::vec3 axis1 = localCFrame.GetColumn(1);
	mt::vec3 axis2 = localCFrame.GetColumn(2);
	mt::vec3 scale = obj_src->NodeGetWorldScaling();

	/* Apply not only the pivot and axis values, but also take scale into count
	 * this is not working well, if only one or two axis are scaled, but works ok on
	 * homogeneous scaling. */
	PHY_IConstraint *constraint = phys_env->CreateConstraint(
		phy_src, phy_dest, (PHY_ConstraintType)dat->type,
		(float)(dat->pivX * scale.x), (float)(dat->pivY * scale.y), (float)(dat->pivZ * scale.z),
		(float)(axis0.x * scale.x), (float)(axis0.y * scale.y), (float)(axis0.z * scale.z),
		(float)(axis1.x * scale.x), (float)(axis1.y * scale.y), (float)(axis1.z * scale.z),
		(float)(axis2.x * scale.x), (float)(axis2.y * scale.y), (float)(axis2.z * scale.z),
		dat->flag);

	/* PHY_POINT2POINT_CONSTRAINT = 1,
	 * PHY_LINEHINGE_CONSTRAINT = 2,
	 * PHY_ANGULAR_CONSTRAINT = 3,
	 * PHY_CONE_TWIST_CONSTRAINT = 4,
	 * PHY_VEHICLE_CONSTRAINT = 11,
	 * PHY_GENERIC_6DOF_CONSTRAINT = 12 */

	if (!constraint) {
		return;
	}

	int dof = 0;
	int dof_max = 0;
	int dofbit = 0;

	switch (dat->type) {
		/* Set all the limits for generic 6DOF constraint. */
		case PHY_GENERIC_6DOF_CONSTRAINT:
		{
			dof_max = 6;
			dofbit = 1;
			break;
		}
		/* Set XYZ angular limits for cone twist constraint. */
		case PHY_CONE_TWIST_CONSTRAINT:
		{
			dof = 3;
			dof_max = 6;
			dofbit = 1 << 3;
			break;
		}
		/* Set only X angular limits for line hinge and angular constraint. */
		case PHY_LINEHINGE_CONSTRAINT:
		case PHY_ANGULAR_CONSTRAINT:
		{
			dof = 3;
			dof_max = 4;
			dofbit = 1 << 3;
			break;
		}
		default:
		{
			break;
		}
	}

	for (; dof < dof_max; dof++) {
		if (dat->flag & dofbit) {
			constraint->SetParam(dof, dat->minLimit[dof], dat->maxLimit[dof]);
		}
		else {
			/* minLimit > maxLimit means free (no limit) for this degree of freedom. */
			constraint->SetParam(dof, 1.0f, -1.0f);
		}
		dofbit <<= 1;
	}

	if (dat->flag & CONSTRAINT_USE_BREAKING) {
		constraint->SetBreakingThreshold(dat->breaking);
	}
}

CcdCollData::CcdCollData(const btPersistentManifold *manifoldPoint)
	:m_manifoldPoint(manifoldPoint)
{
}

CcdCollData::~CcdCollData()
{
}

unsigned int CcdCollData::GetNumContacts() const
{
	return m_manifoldPoint->getNumContacts();
}

mt::vec3 CcdCollData::GetLocalPointA(unsigned int index, bool first) const
{
	const btManifoldPoint& point = m_manifoldPoint->getContactPoint(index);
	return ToMt(first ? point.m_localPointA : point.m_localPointB);
}

mt::vec3 CcdCollData::GetLocalPointB(unsigned int index, bool first) const
{
	const btManifoldPoint& point = m_manifoldPoint->getContactPoint(index);
	return ToMt(first ? point.m_localPointB : point.m_localPointA);
}

mt::vec3 CcdCollData::GetWorldPoint(unsigned int index, bool first) const
{
	const btManifoldPoint& point = m_manifoldPoint->getContactPoint(index);
	return ToMt(point.m_positionWorldOnB);
}

mt::vec3 CcdCollData::GetNormal(unsigned int index, bool first) const
{
	const btManifoldPoint& point = m_manifoldPoint->getContactPoint(index);
	return ToMt(first ? -point.m_normalWorldOnB : point.m_normalWorldOnB);
}

float CcdCollData::GetCombinedFriction(unsigned int index, bool first) const
{
	const btManifoldPoint& point = m_manifoldPoint->getContactPoint(index);
	return point.m_combinedFriction;
}

float CcdCollData::GetCombinedRollingFriction(unsigned int index, bool first) const
{
	const btManifoldPoint& point = m_manifoldPoint->getContactPoint(index);
	return point.m_combinedRollingFriction;
}

float CcdCollData::GetCombinedRestitution(unsigned int index, bool first) const
{
	const btManifoldPoint& point = m_manifoldPoint->getContactPoint(index);
	return point.m_combinedRestitution;
}

float CcdCollData::GetAppliedImpulse(unsigned int index, bool first) const
{
	const btManifoldPoint& point = m_manifoldPoint->getContactPoint(index);
	return point.m_appliedImpulse;
}
