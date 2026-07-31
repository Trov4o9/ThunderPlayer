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
 * The engine ties all game modules together.
 */

/** \file gameengine/Ketsji/KX_KetsjiEngine.cpp
 *  \ingroup ketsji
 */

#ifdef _MSC_VER
#  pragma warning (disable:4786)
#endif

#include "CM_Message.h"
#include "Gbuffer.h"

#include <boost/format.hpp>
#include <cstdio>
#include <stdio.h>

#include "BLI_task.h"

#include "KX_KetsjiEngine.h"

#include "EXP_ListValue.h"
#include "EXP_IntValue.h"
#include "EXP_BoolValue.h"
#include "EXP_FloatValue.h"

#include "RAS_BucketManager.h"
#include "RAS_Rasterizer.h"
#include "RAS_ICanvas.h"
#include "RAS_LightManager.h"
#include "RAS_OffScreen.h"
#include "RAS_Query.h"
#include "RAS_ILightObject.h"
#include "SCA_IInputDevice.h"
#include "KX_Camera.h"
#include "KX_LightObject.h"
#include "KX_Globals.h"
#include "KX_PyConstraintBinding.h"
#include "RAS_OpenGLRasterizer.h"
#include "PHY_IPhysicsEnvironment.h"
#include "RAS_IMaterial.h"

#include "GPU_glew.h"

#include "RAS_MeshUser.h"
#include "RAS_PersistentInstanceManager.h"

#include "GPU_draw.h"
#include "GPU_extensions.h"
#include "GPU_material.h"
#include "GPU_shader.h"
#include "GPU_vertex_array.h"

#include "KX_NetworkMessageScene.h"

#include "DEV_Joystick.h" // for DEV_Joystick::HandleEvents
#include "KX_PythonInit.h" // for updatePythonJoysticks

#include "KX_WorldInfo.h"

#include "BL_Converter.h"
#include "BL_SceneConverter.h"

#include "RAS_FramingManager.h"
#include "DNA_world_types.h"
#include "DNA_scene_types.h"

#include "KX_NavMeshObject.h"
#include "GPU_framebuffer.h"
#include "KX_GrassSystem.h"
#define DEFAULT_LOGIC_TIC_RATE 60.0
GPUFrameBuffer *gGBufferFBO = nullptr;

GPUTexture *gGBufferTexAlbedo   = nullptr;
GPUTexture *gGBufferTexNormal   = nullptr;
GPUTexture *gGBufferTexMaterial = nullptr;
GPUTexture *gGBufferTexDepth    = nullptr; 
bool gBufferInitialized = false;
double g_render_fps_limit = 60.0;

KX_ExitInfo::KX_ExitInfo()
	:m_code(NO_REQUEST)
{
}

KX_KetsjiEngine::CameraRenderData::CameraRenderData(KX_Camera *rendercam, KX_Camera *cullingcam, const RAS_Rect& area,
                                                    const RAS_Rect& viewport, RAS_Rasterizer::StereoMode stereoMode, RAS_Rasterizer::StereoEye eye)
	:m_renderCamera(rendercam),
	m_cullingCamera(cullingcam),
	m_area(area),
	m_viewport(viewport),
	m_stereoMode(stereoMode),
	m_eye(eye)
{
	m_renderCamera->AddRef();
}

KX_KetsjiEngine::CameraRenderData::CameraRenderData(const CameraRenderData& other)
	:m_renderCamera(CM_AddRef(other.m_renderCamera)),
	m_cullingCamera(other.m_cullingCamera),
	m_area(other.m_area),
	m_viewport(other.m_viewport),
	m_stereoMode(other.m_stereoMode),
	m_eye(other.m_eye)
{
}

KX_KetsjiEngine::CameraRenderData::~CameraRenderData()
{
	m_renderCamera->Release();
}

KX_KetsjiEngine::SceneRenderData::SceneRenderData(KX_Scene *scene)
	:m_scene(scene)
{
}

KX_KetsjiEngine::FrameRenderData::FrameRenderData(RAS_OffScreen::Type ofsType)
	:m_ofsType(ofsType)
{
}

KX_KetsjiEngine::RenderData::RenderData(RAS_Rasterizer::StereoMode stereoMode, bool renderPerEye)
	:m_stereoMode(stereoMode),
	m_renderPerEye(renderPerEye)
{
}


const std::string KX_KetsjiEngine::m_profileLabels[tc_numCategories] = {
	"Physics:", // tc_physics
	"Logic:", // tc_logic
	"Animations:", // tc_animations
	"Network:", // tc_network
	"Scenegraph:", // tc_scenegraph
	"Rasterizer:", // tc_rasterizer
	"Services:", // tc_services
	"Overhead:", // tc_overhead
	"Outside:", // tc_outside
	"GPU Latency:" // tc_latency
};

const std::string KX_KetsjiEngine::m_renderQueriesLabels[QUERY_MAX] = {
	"Samples:", // QUERY_SAMPLES
	"Primitives:", // QUERY_PRIMITIVES
	"Time:" // QUERY_TIME
};

/**
 * Constructor of the Ketsji Engine
 */
KX_KetsjiEngine::KX_KetsjiEngine()
	:m_canvas(nullptr),
	m_rasterizer(nullptr),
	m_converter(nullptr),
	m_networkMessageManager(nullptr),
#ifdef WITH_PYTHON
	m_pyprofiledict(PyDict_New()),
#endif
	m_inputDevice(nullptr),
	m_scenes(new EXP_ListValue<KX_Scene>()),
	m_bInitialized(false),
	m_flags(AUTO_ADD_DEBUG_PROPERTIES),
	m_interpolationAlpha(0.0),
	m_frameTime(0.0f),
	m_frameCounter(0),
	m_clockTime(0.0f),
	m_timescale(1.0f),
	m_previousRealTime(0.0f),
	m_lastRenderTime(0.0f),
	m_accumulatedSkippedTime(0.0f),
	m_maxLogicFrame(5),
	m_maxPhysicsFrame(5),
	m_ticrate(DEFAULT_LOGIC_TIC_RATE),
	m_anim_framerate(25.0),
	m_doRender(true),
	m_exitKey(SCA_IInputDevice::ENDKEY),
	m_logger(KX_TimeCategoryLogger(m_clock, 25)),
	m_average_framerate(0.0),
	m_showBoundingBox(KX_DebugOption::DISABLE),
	m_showArmature(KX_DebugOption::DISABLE),
	m_showCameraFrustum(KX_DebugOption::DISABLE),
	m_showShadowFrustum(KX_DebugOption::DISABLE),
	m_globalsettings({0}),
	m_taskscheduler(BLI_task_scheduler_create(TASK_SCHEDULER_AUTO_THREADS))
{
	for (int i = tc_first; i < tc_numCategories; i++) {
		m_logger.AddCategory((KX_TimeCategory)i);
	}

	m_renderQueries.emplace_back(RAS_Query::SAMPLES);
	m_renderQueries.emplace_back(RAS_Query::PRIMITIVES);
	m_renderQueries.emplace_back(RAS_Query::TIME);
}

/**
 *	Destructor of the Ketsji Engine, release all memory
 */
KX_KetsjiEngine::~KX_KetsjiEngine()
{
	// Cleanup Persistent Instance Manager
	RAS_PersistentInstanceManager::Cleanup();
	
#ifdef WITH_PYTHON
	Py_CLEAR(m_pyprofiledict);
#endif

	if (m_taskscheduler) {
		BLI_task_scheduler_free(m_taskscheduler);
	}

	m_scenes->Release();
}

void KX_KetsjiEngine::SetInputDevice(SCA_IInputDevice *inputDevice)
{
	BLI_assert(inputDevice);
	m_inputDevice = inputDevice;
}

void KX_KetsjiEngine::SetCanvas(RAS_ICanvas *canvas)
{
	BLI_assert(canvas);
	m_canvas = canvas;
}

void KX_KetsjiEngine::SetRasterizer(RAS_Rasterizer *rasterizer)
{
	BLI_assert(rasterizer);
	m_rasterizer = rasterizer;
	
	// Initialize Persistent Instance Manager
	RAS_PersistentInstanceManager::Initialize();
}

void KX_KetsjiEngine::SetNetworkMessageManager(KX_NetworkMessageManager *manager)
{
	m_networkMessageManager = manager;
}

#ifdef WITH_PYTHON
PyObject *KX_KetsjiEngine::GetPyProfileDict()
{
	Py_INCREF(m_pyprofiledict);
	return m_pyprofiledict;
}
#endif

void KX_KetsjiEngine::SetConverter(BL_Converter *converter)
{
	BLI_assert(converter);
	m_converter = converter;
}

void CreateGBuffer(const int width, const int height, const int samples)
{
    printf("[CreateGBuffer] Iniciando criação do GBuffer (%dx%d), samples=%d\n", width, height, samples);
    char err[256] = {0};

    if (gGBufferFBO) {
        GPU_framebuffer_free(gGBufferFBO);
        gGBufferFBO = nullptr;
    }

    if (gGBufferTexAlbedo) GPU_texture_free(gGBufferTexAlbedo);
    if (gGBufferTexNormal) GPU_texture_free(gGBufferTexNormal);
    if (gGBufferTexMaterial) GPU_texture_free(gGBufferTexMaterial);
    if (gGBufferTexDepth) GPU_texture_free(gGBufferTexDepth);

    gGBufferFBO = GPU_framebuffer_create();
    if (!gGBufferFBO) {
        printf("[ERRO] GPU_framebuffer_create retornou NULL!\n");
        return;
    }

    bool error = false;
    const bool useMSAA = samples > 0;

    if (useMSAA) {
        printf("[CreateGBuffer] Criando attachments multisample...\n");

        GPURenderBuffer *rbColor = GPU_renderbuffer_create(width, height, samples, GPU_HDR_HALF_FLOAT, GPU_RENDERBUFFER_COLOR, err);
        if (!rbColor || !GPU_framebuffer_renderbuffer_attach(gGBufferFBO, rbColor, 0, err)) {
            printf("[ERRO] Falha ao anexar renderbuffer COLOR: %s\n", err);
            error = true;
        }

        GPURenderBuffer *rbNormal = GPU_renderbuffer_create(width, height, samples, GPU_HDR_HALF_FLOAT, GPU_RENDERBUFFER_COLOR, err);
        if (!rbNormal || !GPU_framebuffer_renderbuffer_attach(gGBufferFBO, rbNormal, 1, err)) {
            printf("[ERRO] Falha ao anexar renderbuffer NORMAL: %s\n", err);
            error = true;
        }

        GPURenderBuffer *rbMaterial = GPU_renderbuffer_create(width, height, samples, GPU_HDR_NONE, GPU_RENDERBUFFER_COLOR, err);
        if (!rbMaterial || !GPU_framebuffer_renderbuffer_attach(gGBufferFBO, rbMaterial, 2, err)) {
            printf("[ERRO] Falha ao anexar renderbuffer MATERIAL: %s\n", err);
            error = true;
        }

        GPURenderBuffer *rbDepth = GPU_renderbuffer_create(width, height, samples, GPU_HDR_NONE, GPU_RENDERBUFFER_DEPTH, err);
        if (!rbDepth || !GPU_framebuffer_renderbuffer_attach(gGBufferFBO, rbDepth, -1, err)) {
            printf("[ERRO] Falha ao anexar renderbuffer DEPTH: %s\n", err);
            error = true;
        }
    }
    else {
        printf("[CreateGBuffer] Criando texturas 2D...\n");

        gGBufferTexAlbedo = GPU_texture_create_2D(width, height, nullptr, GPU_HDR_NONE, err);
        gGBufferTexNormal = GPU_texture_create_2D(width, height, nullptr, GPU_HDR_HALF_FLOAT, err);
        gGBufferTexMaterial = GPU_texture_create_2D(width, height, nullptr, GPU_HDR_NONE, err);
        gGBufferTexDepth = GPU_texture_create_depth(width, height, false, err);

        if (!gGBufferTexAlbedo || !GPU_framebuffer_texture_attach(gGBufferFBO, gGBufferTexAlbedo, 0, err)) {
            printf("[ERRO] Falha ao anexar textura ALBEDO: %s\n", err);
            error = true;
        }

        if (!gGBufferTexNormal || !GPU_framebuffer_texture_attach(gGBufferFBO, gGBufferTexNormal, 1, err)) {
            printf("[ERRO] Falha ao anexar textura NORMAL: %s\n", err);
            error = true;
        }

        if (!gGBufferTexMaterial || !GPU_framebuffer_texture_attach(gGBufferFBO, gGBufferTexMaterial, 2, err)) {
            printf("[ERRO] Falha ao anexar textura MATERIAL: %s\n", err);
            error = true;
        }

        if (!gGBufferTexDepth || !GPU_framebuffer_texture_attach(gGBufferFBO, gGBufferTexDepth, -1, err)) {
            printf("[ERRO] Falha ao anexar textura DEPTH: %s\n", err);
            error = true;
        }
    }


    if (error || !GPU_framebuffer_check_valid(gGBufferFBO, err)) {
        printf("[ERRO] GBuffer inválido: %s\n", err);
        GPU_framebuffer_free(gGBufferFBO);
        gGBufferFBO = nullptr;
        return;
    }

    printf("[CreateGBuffer] GBuffer criado com sucesso (%dx%d, %s)\n",
           width, height, useMSAA ? "MSAA" : "sem MSAA");
}

void KX_KetsjiEngine::StartEngine()
{
    m_previousRealTime = m_clock.GetTimeSecond();
    m_lastRenderTime = m_previousRealTime;
    m_accumulatedSkippedTime = 0.0;
    // Reset game time counters so the first frame always starts from t=0.
    // Without this, any time spent between StartEngine() and the first NextFrame()
    // (e.g. Python init, audio device setup, scene conversion) is counted as elapsed
    // game time, causing the engine to skip logic frames on startup/restart and
    // leaving logic bricks in an inconsistent state.
    m_clockTime = 0.0;
    m_frameTime = 0.0;
    m_bInitialized = true;
}


void KX_KetsjiEngine::BeginFrame()
{
	++m_frameCounter;

	if (m_flags & SHOW_RENDER_QUERIES) {
		m_logger.StartLog(tc_overhead);

		for (RAS_Query& query : m_renderQueries) {
			query.Begin();
		}
	}

	// Reset per-frame caches.
	m_cameraVisibilityCache.clear();
	//m_lastViewport[0] = m_lastViewport[1] = m_lastViewport[2] = m_lastViewport[3] = -1;
	//m_lastScissor[0] = m_lastScissor[1] = m_lastScissor[2] = m_lastScissor[3] = -1;

	m_logger.StartLog(tc_rasterizer);

	m_rasterizer->BeginFrame(m_frameTime);

	m_canvas->BeginDraw();
}

void KX_KetsjiEngine::EndFrame()
{
	m_rasterizer->MotionBlur();

	m_logger.StartLog(tc_overhead);

	if (m_flags & SHOW_RENDER_QUERIES) {
		for (RAS_Query& query : m_renderQueries) {
			query.End();
		}
	}

	// Show profiling info
	if (m_flags & (SHOW_PROFILE | SHOW_FRAMERATE | SHOW_DEBUG_PROPERTIES | SHOW_RENDER_QUERIES)) {
		RenderDebugProperties();
	}

	double tottime = m_logger.GetAverage();
	if (tottime < 1e-6) {
		tottime = 1e-6;
	}

#ifdef WITH_PYTHON
	for (int i = tc_first; i < tc_numCategories; ++i) {
		double time = m_logger.GetAverage((KX_TimeCategory)i);
		PyObject *val = PyTuple_New(2);
		PyTuple_SetItem(val, 0, PyFloat_FromDouble(time * 1000.0));
		PyTuple_SetItem(val, 1, PyFloat_FromDouble(time / tottime * 100.0));

		PyDict_SetItemString(m_pyprofiledict, m_profileLabels[i].c_str(), val);
		Py_DECREF(val);
	}
#endif

	m_average_framerate = 1.0 / tottime;

	// Go to next profiling measurement, time spent after this call is shown in the next frame.
	m_logger.NextMeasurement();

	m_logger.StartLog(tc_rasterizer);
	m_rasterizer->EndFrame();
	if (m_converter) {
		m_converter->FlushRemovedMeshes();
	}

	m_logger.StartLog(tc_logic);
	m_canvas->FlushScreenshots(m_rasterizer);

	// swap backbuffer (drawing into this buffer) <-> front/visible buffer
	m_logger.StartLog(tc_latency);
	m_canvas->SwapBuffers();
	m_logger.StartLog(tc_rasterizer);

	m_canvas->EndDraw();
}



bool KX_KetsjiEngine::NextFrame()
{
	m_logger.StartLog(tc_services);

	/*
	 * Clock advancement. There is basically two case:
	 *   - USE_EXTERNAL_CLOCK is true, the user is responsible to advance the time
	 *   manually using setClockTime, so here, we do not do anything.
	 *   - USE_EXTERNAL_CLOCK is false, we consider how much
	 *   time has elapsed since last call and we scale this time by the time
	 *   scaling parameter. If m_timescale is 1.0 (default value), the clock
	 *   corresponds to the computer clock.
	 *
	 * Once clockTime has been computed, we will compute how many logic frames
	 * will be executed before the next rendering phase (which will occur at "clockTime").
	 * The game time elapsing between two logic frames (called framestep)
	 * depends on several variables:
	 *   - ticrate
	 *   - max_physic_frame
	 *   - max_logic_frame
	 *   - fixed_framerate
	 * XXX The logic over computation framestep is definitively not clear (and
	 * I'm not even sure it is correct). If needed frame is strictly greater
	 * than max_physics_frame, we are doing a jump in game time, but keeping
	 * framestep = 1 / ticrate, while if frames is greater than
	 * max_logic_frame, we increase framestep.
	 *
	 * XXX render.fps is not considred anywhere.
	 */

	double timestep = m_timescale / m_ticrate;
	if (!(m_flags & USE_EXTERNAL_CLOCK)) {
		double current_time = m_clock.GetTimeSecond();
		double dt = current_time - m_previousRealTime;
		m_previousRealTime = current_time;
		m_clockTime += dt * m_timescale;
	}

	double deltatime = m_clockTime - m_frameTime;

	// Compute the number of logic frames to do each update.
	int frames = int(deltatime * m_ticrate / m_timescale + 1e-6);

	double framestep = timestep;

	double skippedTime = 0.0;
	if (frames > m_maxPhysicsFrame) {
		skippedTime = (frames - m_maxPhysicsFrame) * timestep;
		m_frameTime += skippedTime;
		frames = m_maxPhysicsFrame;
	}

	m_accumulatedSkippedTime += skippedTime;

	if (frames > m_maxLogicFrame) {
		framestep = (frames * timestep) / m_maxLogicFrame;
		frames = m_maxLogicFrame;
	}

	if (m_inputDevice) {
		m_inputDevice->ReleaseMoveEvent();
	}

	for (unsigned short i = 0; i < frames; ++i) {
		m_frameTime += framestep;


#ifdef WITH_SDL
		// Handle all SDL Joystick events here to share them for all scenes properly.
		short addrem[JOYINDEX_MAX] = {0};
		if (DEV_Joystick::HandleEvents(addrem)) {
#  ifdef WITH_PYTHON
			updatePythonJoysticks(addrem);
#  endif  // WITH_PYTHON
		}
#endif  // WITH_SDL

		// for each scene, call the proceed functions
		for (KX_Scene *scene : m_scenes) {
			/* Suspension holds the physics and logic processing for an
			 * entire scene. Objects can be suspended individually, and
			 * the settings for that precede the logic and physics
			 * update. */
			m_logger.StartLog(tc_logic);

			scene->UpdateObjectActivity();

			if (!scene->IsSuspended()) {
				// set Python hooks for each scene
				KX_SetActiveScene(scene);

				// Process sensors, and controllers
				m_logger.StartLog(tc_logic);
				scene->LogicBeginFrame(m_frameTime, framestep);

				// Scenegraph needs to be updated again, because Logic Controllers
				// can affect the local matrices.
				m_logger.StartLog(tc_scenegraph);
				scene->UpdateParents();

				// Process actuators

				// Do some cleanup work for this logic frame
				m_logger.StartLog(tc_logic);
				scene->LogicUpdateFrame(m_frameTime);

				scene->LogicEndFrame();

				// Actuators can affect the scenegraph
				m_logger.StartLog(tc_scenegraph);
				scene->UpdateParents();
			}

			m_logger.StartLog(tc_services);
		}

		m_logger.StartLog(tc_network);
		m_networkMessageManager->ClearMessages();

		// update system devices
		m_logger.StartLog(tc_logic);
		if (m_inputDevice) {
			m_inputDevice->ClearInputs();
		}

		m_converter->ProcessScheduledLibraries();

		UpdateSuspendedScenes(framestep);
		// scene management
		ProcessScheduledScenes();

		for (KX_Scene *scene : m_scenes) {
			if (!scene->IsSuspended()) {
				m_logger.StartLog(tc_physics);
				KX_SetActiveScene(scene);

				scene->GetPhysicsEnvironment()->ProceedDeltaTime(m_frameTime, (float)timestep, (float)framestep);

				m_logger.StartLog(tc_scenegraph);
				scene->UpdateParents();
			}
		}
	}

	bool doRender = true;
	if (g_render_fps_limit > 0.0) {
		double minFrameInterval = 1.0 / g_render_fps_limit;
		double currentTime = m_clock.GetTimeSecond();
		double renderElapsed = currentTime - m_lastRenderTime;

		if (renderElapsed < minFrameInterval) {
			doRender = false;
		}
		else {
			// Sincroniza o próximo frame compensando drifts e latências
			// m_lastRenderTime += minFrameInterval mantém o pacing fixo
			m_lastRenderTime += minFrameInterval;

			// Se estivermos MUITO atrasados (ex: stutter longo), resetamos para evitar
			// uma "metralhadora" de frames tentando compensar o tempo perdido.
			if (currentTime - m_lastRenderTime > minFrameInterval * 2.0) {
				m_lastRenderTime = currentTime;
			}
		}
	}
	else {
		m_lastRenderTime = m_clock.GetTimeSecond();
	}

	if (doRender) {
		float alpha = (float)((m_clockTime - m_frameTime) * m_ticrate / m_timescale + 1e-6);
		if (alpha < 0.0f) alpha = 0.0f;
		if (alpha > 1.0f) alpha = 1.0f;
		m_interpolationAlpha = (double)alpha;

		// If we are at the exact end of a physics step, alpha might be 1.0.
		// However, Bullet's stepSimulation might have already updated the transforms.
		// We want to ensure that if we just finished a physics step, we use the most recent state.
		// The current alpha calculation is correct for interpolation between the previous and current state.

		for (KX_Scene *scene : m_scenes) {
			if (!scene->IsSuspended()) {
				m_logger.StartLog(tc_physics);
				KX_SetActiveScene(scene);

				scene->GetPhysicsEnvironment()->SyncMotionStatesInterpolated((float)timestep, m_interpolationAlpha);

				m_logger.StartLog(tc_scenegraph);
				scene->UpdateParents();
			}
		}
	}


	// Start logging time spent outside main loop
	m_logger.StartLog(tc_outside);

	return doRender && m_doRender;
}

void KX_KetsjiEngine::UpdateSuspendedScenes(double framestep)
{
	for (KX_Scene *scene : m_scenes) {
		if (scene->IsSuspended()) {
			scene->SetSuspendedDelta(scene->GetSuspendedDelta() + framestep);
		}
	}
}

KX_KetsjiEngine::CameraRenderData KX_KetsjiEngine::GetCameraRenderData(KX_Scene *scene, KX_Camera *camera, KX_Camera *overrideCullingCam,
                                                                       const RAS_Rect& displayArea, RAS_Rasterizer::StereoMode stereoMode, RAS_Rasterizer::StereoEye eye)
{
	KX_Camera *rendercam;
	/* In case of stereo we must copy the camera because it is used twice with different settings
	 * (modelview matrix). This copy use the same transform settings that the original camera
	 * and its name is based on with the eye number in addition.
	 */
	const bool usestereo = (stereoMode != RAS_Rasterizer::RAS_STEREO_NOSTEREO);
	if (usestereo) {
		rendercam = new KX_Camera(scene, KX_Scene::m_callbacks, *camera->GetCameraData(), true);
		rendercam->SetName("__stereo_" + camera->GetName() + "_" + std::to_string(eye) + "__");
		rendercam->NodeSetGlobalOrientation(camera->NodeGetWorldOrientation());
		rendercam->NodeSetWorldPosition(camera->NodeGetWorldPosition());
		rendercam->NodeSetWorldScale(camera->NodeGetWorldScaling());
		rendercam->NodeUpdate();
	}
	// Else use the native camera.
	else {
		rendercam = camera;
	}

	KX_Camera *cullingcam = (overrideCullingCam) ? overrideCullingCam : rendercam;

	KX_SetActiveScene(scene);
#ifdef WITH_PYTHON
	scene->RunDrawingCallbacks(KX_Scene::PRE_DRAW_SETUP, rendercam);
#endif

	RAS_Rect area;
	RAS_Rect viewport;
	// Compute the area and the viewport based on the current display area and the optional camera viewport.
	GetSceneViewport(scene, rendercam, displayArea, area, viewport);
	// Compute the camera matrices: modelview and projection.
	rendercam->UpdateView(m_rasterizer, scene, stereoMode, eye, viewport, area);

	CameraRenderData cameraData(rendercam, cullingcam, area, viewport, stereoMode, eye);

	if (usestereo) {
		rendercam->Release();
	}

	return cameraData;
}

KX_KetsjiEngine::RenderData KX_KetsjiEngine::GetRenderData()
{
	const RAS_Rasterizer::StereoMode stereomode = m_rasterizer->GetStereoMode();
	const bool usestereo = (stereomode != RAS_Rasterizer::RAS_STEREO_NOSTEREO);
	// Set to true when each eye needs to be rendered in a separated off screen.
	const bool renderpereye = stereomode == RAS_Rasterizer::RAS_STEREO_INTERLACED ||
	                          stereomode == RAS_Rasterizer::RAS_STEREO_VINTERLACE ||
	                          stereomode == RAS_Rasterizer::RAS_STEREO_ANAGLYPH;

	RenderData renderData(stereomode, renderpereye);

	// The number of eyes to manage in case of stereo.
	const unsigned short numeyes = (usestereo) ? 2 : 1;
	// The number of frames in case of stereo, could be multiple for interlaced or anaglyph stereo.
	const unsigned short numframes = (renderpereye) ? 2 : 1;

	// The off screen corresponding to the frame.
	static const RAS_OffScreen::Type ofsType[] = {
		RAS_OffScreen::RAS_OFFSCREEN_EYE_LEFT0,
		RAS_OffScreen::RAS_OFFSCREEN_EYE_RIGHT0
	};

	// Pre-compute the display area used for stereo or normal rendering.
	std::vector<RAS_Rect> displayAreas;
	for (unsigned short eye = 0; eye < numeyes; ++eye) {
		displayAreas.push_back(m_rasterizer->GetRenderArea(m_canvas, stereomode, (RAS_Rasterizer::StereoEye)eye));
	}

	// Prepare override culling camera of each scenes, we don't manage stereo currently.
	for (KX_Scene *scene : m_scenes) {
		KX_Camera *overrideCullingCam = scene->GetOverrideCullingCamera();

		if (overrideCullingCam) {
			RAS_Rect area;
			RAS_Rect viewport;
			// Compute the area and the viewport based on the current display area and the optional camera viewport.
			GetSceneViewport(scene, overrideCullingCam, displayAreas[RAS_Rasterizer::RAS_STEREO_LEFTEYE], area, viewport);
			// Compute the camera matrices: modelview and projection.
			overrideCullingCam->UpdateView(m_rasterizer, scene, stereomode, RAS_Rasterizer::RAS_STEREO_LEFTEYE, viewport, area);
		}
	}

	for (unsigned short frame = 0; frame < numframes; ++frame) {
		renderData.m_frameDataList.emplace_back(ofsType[frame]);
		FrameRenderData& frameData = renderData.m_frameDataList.back();

		// Get the eyes managed per frame.
		std::vector<RAS_Rasterizer::StereoEye> eyes;
		// One eye per frame but different.
		if (renderpereye) {
			eyes = {(RAS_Rasterizer::StereoEye)frame};
		}
		// Two eyes for unique frame.
		else if (usestereo) {
			eyes = {RAS_Rasterizer::RAS_STEREO_LEFTEYE, RAS_Rasterizer::RAS_STEREO_RIGHTEYE};
		}
		// Only one eye for unique frame.
		else {
			eyes = {RAS_Rasterizer::RAS_STEREO_LEFTEYE};
		}

		for (KX_Scene *scene : m_scenes) {
			frameData.m_sceneDataList.emplace_back(scene);
			SceneRenderData& sceneFrameData = frameData.m_sceneDataList.back();

			KX_Camera *activecam = scene->GetActiveCamera();
			KX_Camera *overrideCullingCam = scene->GetOverrideCullingCamera();
			for (KX_Camera *cam : scene->GetCameraList()) {
				if (cam != activecam && !cam->UseViewport()) {
					continue;
				}

				for (RAS_Rasterizer::StereoEye eye : eyes) {
					sceneFrameData.m_cameraDataList.push_back(GetCameraRenderData(scene, cam, overrideCullingCam, displayAreas[eye],
					                                                              stereomode, eye));
				}
			}
		}
	}

	return renderData;
}

void KX_KetsjiEngine::Render()
{
	IncrementFrameCounter();
	
	// Begin frame for persistent instance manager
	RAS_PersistentInstanceManager *persistentMgr = RAS_PersistentInstanceManager::GetInstance();
	if (persistentMgr && persistentMgr->IsInitialized()) {
		persistentMgr->BeginFrame();
	}
	
	// Update global light UBO for all scenes
	RAS_LightManager *lightMgr = RAS_LightManager::GetInstance();
	for (KX_Scene *scene : m_scenes) {
		lightMgr->UpdateLights(scene, GetCurrentFrame());
	}
	lightMgr->BindUBO();
	
	m_logger.StartLog(tc_rasterizer);

	BeginFrame();

	// Set vsync one time per frame
	m_canvas->SetSwapControl(m_canvas->GetSwapControl());

	for (KX_Scene *scene : m_scenes) {
		// shadow buffers
		RenderShadowBuffers(scene);
		// Render only independent texture renderers here.
		scene->RenderTextureRenderers(KX_TextureRendererManager::VIEWPORT_INDEPENDENT, m_rasterizer, nullptr, nullptr, RAS_Rect(), RAS_Rect());
	}

	RenderData renderData = GetRenderData();

	const int width = m_canvas->GetWidth();
	const int height = m_canvas->GetHeight();
	// clear the entire game screen with the border color
	// only once per frame
	m_rasterizer->SetViewport(0, 0, width, height);
	m_rasterizer->SetScissor(0, 0, width, height);

	KX_Scene *firstscene = m_scenes->GetFront();
	const RAS_FrameSettings &framesettings = firstscene->GetFramingType();
	// Use the framing bar color set in the Blender scenes
	m_rasterizer->SetClearColor(framesettings.BarRed(), framesettings.BarGreen(), framesettings.BarBlue(), 1.0f);

	// Used to detect when a camera is the first rendered an then doesn't request a depth clear.
	unsigned short pass = 0;

	for (FrameRenderData& frameData : renderData.m_frameDataList) {
		// Current bound off screen.
		RAS_OffScreen *offScreen = m_canvas->GetOffScreen(frameData.m_ofsType);
		offScreen->Bind();

		// Clear off screen only before the first scene render.
		m_rasterizer->Clear(RAS_Rasterizer::RAS_COLOR_BUFFER_BIT | RAS_Rasterizer::RAS_DEPTH_BUFFER_BIT);

		// for each scene, call the proceed functions
		for (unsigned short i = 0, size = frameData.m_sceneDataList.size(); i < size; ++i) {
			const SceneRenderData& sceneFrameData = frameData.m_sceneDataList[i];
			KX_Scene *scene = sceneFrameData.m_scene;

			const bool isfirstscene = (i == 0);
			const bool islastscene = (i == (size - 1));

			// pass the scene's worldsettings to the rasterizer
			// Optimization: only update if different from previous scene or first scene
			static KX_Scene* lastScene = nullptr;
			if (isfirstscene || scene != lastScene) {
				scene->GetWorldInfo()->UpdateWorldSettings(m_rasterizer);
				lastScene = scene;
			}

			m_rasterizer->SetAuxilaryClientInfo(scene);

			// Draw the scene once for each camera with an enabled viewport or an active camera.
			for (const CameraRenderData& cameraFrameData : sceneFrameData.m_cameraDataList) {
				// do the rendering
				RenderCamera(scene, cameraFrameData, offScreen, pass++, isfirstscene);
			}

			/* Choose final render off screen target. If the current off screen is using multisamples we
			 * are sure that it will be copied to a non-multisamples off screen before render the filters.
			 * In this case the targeted off screen is the same as the current off screen. */
			RAS_OffScreen::Type target;
			if (offScreen->GetSamples() > 0) {
				/* If the last scene is rendered it's useless to specify a multisamples off screen, we use then
				 * a non-multisamples off screen and avoid an extra off screen blit. */
				if (islastscene) {
					target = RAS_OffScreen::NextRenderOffScreen(frameData.m_ofsType);
				}
				else {
					target = frameData.m_ofsType;
				}
			}
			/* In case of non-multisamples a ping pong per scene render is made between a potentially multisamples
			 * off screen and a non-multisamples off screen as the both doesn't use multisamples. */
			else {
				target = RAS_OffScreen::NextRenderOffScreen(frameData.m_ofsType);
			}

			// Render filters and get output off screen.
			offScreen = PostRenderScene(scene, offScreen, m_canvas->GetOffScreen(target));
			frameData.m_ofsType = offScreen->GetType();
		}
	}

	m_canvas->SetViewPort(0, 0, width, height);

	// Compositing per eye off screens to screen.
	if (renderData.m_renderPerEye) {
		RAS_OffScreen *leftofs = m_canvas->GetOffScreen(renderData.m_frameDataList[0].m_ofsType);
		RAS_OffScreen *rightofs = m_canvas->GetOffScreen(renderData.m_frameDataList[1].m_ofsType);
		m_rasterizer->DrawStereoOffScreen(m_canvas, leftofs, rightofs, renderData.m_stereoMode);
	}
	// Else simply draw the off screen to screen.
	else {
		m_rasterizer->DrawOffScreen(m_canvas, m_canvas->GetOffScreen(renderData.m_frameDataList[0].m_ofsType));
	}

	EndFrame();
	
	// End frame for persistent instance manager
	{
		RAS_PersistentInstanceManager *persistentMgr = RAS_PersistentInstanceManager::GetInstance();
		if (persistentMgr && persistentMgr->IsInitialized()) {
			persistentMgr->EndFrame();
		}
	}

#if RAS_ENABLE_CPU_PROFILE
	RAS_CpuProfile_EndFrameAndPrint();
#endif
}

void KX_KetsjiEngine::RequestExit(KX_ExitInfo::Code code)
{
	RequestExit(code, "");
}

void KX_KetsjiEngine::RequestExit(KX_ExitInfo::Code code, const std::string& fileName)
{
	m_exitInfo.m_code = code;
	m_exitInfo.m_fileName = fileName;
}

const KX_ExitInfo& KX_KetsjiEngine::GetExitInfo() const
{
	return m_exitInfo;
}

void KX_KetsjiEngine::EnableCameraOverride(const std::string& forscene, const mt::mat3& orientation,
		const mt::vec3& position, const RAS_CameraData& camdata)
{
	SetFlag(CAMERA_OVERRIDE, true);

	m_overrideSceneName = forscene;
	m_overrideCamOrientation = orientation;
	m_overrideCamPosition = position;
	m_overrideCamData = camdata;
}


void KX_KetsjiEngine::GetSceneViewport(KX_Scene *scene, KX_Camera *cam, const RAS_Rect& displayArea, RAS_Rect& area, RAS_Rect& viewport)
{
	// In this function we make sure the rasterizer settings are up-to-date.
	// We compute the viewport so that logic using this information is up-to-date.

	// Note we postpone computation of the projection matrix
	// so that we are using the latest camera position.

	if (cam->UseViewport()) {
		area = cam->GetViewport();
	}
	else {
		area = displayArea;
	}

	RAS_FramingManager::ComputeViewport(scene->GetFramingType(), area, viewport);
}

static uint64_t frame_counter = 0;

static uint64_t g_frameCounter = 0;

uint64_t GetCurrentFrame() { return g_frameCounter; }
void IncrementFrameCounter() { ++g_frameCounter; }


void KX_KetsjiEngine::UpdateAnimations(KX_Scene *scene)
{
	if (scene->IsSuspended()) {
		return;
	}

	scene->UpdateAnimations(m_frameTime, (m_flags & RESTRICT_ANIMATION) != 0);
}

void KX_KetsjiEngine::RenderShadowBuffers(KX_Scene *scene)
{
	EXP_ListValue<KX_LightObject> *lightlist = scene->GetLightList();
	m_rasterizer->SetAuxilaryClientInfo(scene);

	if (m_rasterizer->GetDrawingMode() != RAS_Rasterizer::RAS_TEXTURED) {
		return;
	}

	m_logger.StartLog(tc_animations);
	// Start Optimization: Only update animations once per frame if not already updated
	// However, since we can't easily check if it was updated, we rely on scene->UpdateAnimations guard (if any)
	// But to be safe and redundant-free, we move this call to ensure it happens before shadow passes
	// AND we only do it if we are in a state that requires it.
	UpdateAnimations(scene);
	m_logger.StartLog(tc_rasterizer);

	// Start Optimization: Avoid allocating shadow camera if no lights cast shadows
	bool hasShadowUpdates = false;
	for (int i=0; i<lightlist->GetCount(); ++i) {
		KX_LightObject *light = lightlist->GetValue(i);
		RAS_ILightObject *raslight = light->GetLightData();
		if (light->GetVisible() && raslight->HasShadowBuffer()) {
			if (raslight->NeedShadowUpdate()) {
				hasShadowUpdates = true;
			}
		}
	}

	if (!hasShadowUpdates) {
		return;
	}
	// End Optimization

	RAS_CameraData camdata = RAS_CameraData();
	KX_Camera *shadowCam = new KX_Camera(scene, KX_Scene::m_callbacks, camdata, true);
	shadowCam->SetName("__shadow__cam__");

	// Map to cache visibility by light frustum to avoid redundant culling for identical frustums
	ankerl::unordered_dense::map<std::uintptr_t, std::vector<KX_GameObject *>> visibilityCache;
	visibilityCache.reserve(lightlist->GetCount());

	for (KX_LightObject *light : lightlist) {
		light->Update();
		RAS_ILightObject *raslight = light->GetLightData();

		if (light->GetVisible() && raslight->HasShadowBuffer() && raslight->NeedShadowUpdate()) {
			mt::mat3x4 camtrans;

			raslight->BindShadowBuffer(m_canvas, shadowCam, camtrans);

			// Optimization: Cache visibility by frustum data if possible
			const SG_Frustum& frustum = shadowCam->GetFrustum(RAS_Rasterizer::RAS_STEREO_LEFTEYE);
			
			// Generate a simple hash/key for the frustum to avoid redundant culling
			// We use the modelview matrix as the key
			const mt::mat4& projection = shadowCam->GetProjectionMatrix(RAS_Rasterizer::RAS_STEREO_LEFTEYE);
			const mt::mat4& modelview = shadowCam->GetModelviewMatrix(RAS_Rasterizer::RAS_STEREO_LEFTEYE);
			const mt::mat4 viewProj = projection * modelview;
			
			std::uintptr_t frustumKey = 0;
			const float* matData = &viewProj[0];
			for(int i=0; i<16; ++i) frustumKey ^= std::hash<float>{}(matData[i]) + 0x9e3779b9 + (frustumKey << 6) + (frustumKey >> 2);
			frustumKey ^= std::hash<int>{}(raslight->GetShadowLayer());

			auto cacheIt = visibilityCache.find(frustumKey);
			if (cacheIt == visibilityCache.end()) {
				std::vector<KX_GameObject *> visible = scene->CalculateVisibleMeshes(frustum, raslight->GetShadowLayer(), false);
				
				std::vector<KX_GameObject *> shadowObjects;
				shadowObjects.reserve(visible.size());
				for (KX_GameObject *gameobj : visible) {
					if (gameobj->HasShadowCasterMaterial()) {
						shadowObjects.push_back(gameobj);
					}
				}
				cacheIt = visibilityCache.emplace(frustumKey, std::move(shadowObjects)).first;
			}
			
			const std::vector<KX_GameObject *> &objects = cacheIt->second;

			m_logger.StartLog(tc_rasterizer);
			m_rasterizer->Clear(RAS_Rasterizer::RAS_DEPTH_BUFFER_BIT | RAS_Rasterizer::RAS_COLOR_BUFFER_BIT);

			scene->RenderBuckets(objects, RAS_Rasterizer::RAS_SHADOW, camtrans, m_rasterizer, nullptr);

			raslight->UnbindShadowBuffer();
		}
	}

	shadowCam->Release();
}
bool g_useDeferred_GGG = false;
static GLuint lightProgram = 0;
static bool shaderInitialized = false;
static GLint dataTexLoc[4];
static GLint depthTexLoc = -1;
void KX_KetsjiEngine::RenderCamera(KX_Scene *scene, const CameraRenderData& cameraFrameData, RAS_OffScreen *offScreen,
                                   unsigned short pass, bool isFirstScene)
{
	KX_Camera *rendercam = cameraFrameData.m_renderCamera;
	KX_Camera *cullingcam = cameraFrameData.m_cullingCamera;
	const RAS_Rect &area = cameraFrameData.m_area;
	const RAS_Rect &viewport = cameraFrameData.m_viewport;

	KX_SetActiveScene(scene);

	scene->RenderTextureRenderers(KX_TextureRendererManager::VIEWPORT_DEPENDENT, m_rasterizer, offScreen, rendercam, viewport, area);

	const int left = viewport.GetLeft();
	const int bottom = viewport.GetBottom();
	const int width = viewport.GetWidth();
	const int height = viewport.GetHeight();
	if (m_lastViewport[0] != left || m_lastViewport[1] != bottom || m_lastViewport[2] != width || m_lastViewport[3] != height) {
		m_rasterizer->SetViewport(left, bottom, width, height);
		m_lastViewport[0] = left; m_lastViewport[1] = bottom; m_lastViewport[2] = width; m_lastViewport[3] = height;
	}
	if (m_lastScissor[0] != left || m_lastScissor[1] != bottom || m_lastScissor[2] != width || m_lastScissor[3] != height) {
		m_rasterizer->SetScissor(left, bottom, width, height);
		m_lastScissor[0] = left; m_lastScissor[1] = bottom; m_lastScissor[2] = width; m_lastScissor[3] = height;
	}

	if (pass > 0) {
		m_rasterizer->Clear(RAS_Rasterizer::RAS_DEPTH_BUFFER_BIT);
	}

	RAS_Rasterizer::StereoEye eye = cameraFrameData.m_eye;
	m_rasterizer->SetEye(eye);

	m_rasterizer->SetProjectionMatrix(rendercam->GetProjectionMatrix(eye));
	m_rasterizer->SetViewMatrix(rendercam->GetModelviewMatrix(eye), rendercam->NodeGetWorldScaling());

	if (isFirstScene) {
		KX_WorldInfo *worldInfo = scene->GetWorldInfo();
		// Find sun light on first render (only searches once)
		worldInfo->FindSunLight(scene);
		worldInfo->UpdateBackGround(m_rasterizer);
		worldInfo->RenderBackground(m_rasterizer);
	}

	m_logger.StartLog(tc_scenegraph);

	// Cache camera visibility by frustum key to avoid recomputing within the same frame.
	const mt::mat4 viewProj = rendercam->GetProjectionMatrix(eye) * rendercam->GetModelviewMatrix(eye);
	std::uintptr_t frustumKey = 0;
	const float* matData = &viewProj[0];
	for (int i = 0; i < 16; ++i)
		frustumKey ^= std::hash<float>{}(matData[i]) + 0x9e3779b9 + (frustumKey << 6) + (frustumKey >> 2);
	frustumKey ^= std::hash<int>{}(eye);
	frustumKey ^= std::hash<int>{}(width);
	frustumKey ^= std::hash<int>{}(height);
	std::vector<KX_GameObject *> objects;
	{
		auto visIt = m_cameraVisibilityCache.find(frustumKey);
		if (visIt != m_cameraVisibilityCache.end()) {
			objects = visIt->second;
		}
		else {
			{
				RAS_CPU_PROFILE_SCOPE(RAS_CPU_SCENE_CALCULATEVISIBLEMESHES);
				objects = scene->CalculateVisibleMeshes(cullingcam, eye, 0);
			}
			{
				RAS_CPU_PROFILE_SCOPE(RAS_CPU_SCENE_UPDATEOBJECTLODS);
				scene->UpdateObjectLods(cullingcam, objects);
			}
			m_cameraVisibilityCache.emplace(frustumKey, objects);
		}
	}

	m_logger.StartLog(tc_animations);
	// UpdateAnimations(scene); // Redundant call removed - already updated in RenderShadowBuffers or handled by scene
	m_logger.StartLog(tc_rasterizer);

	scene->DrawDebug(objects, m_showBoundingBox, m_showArmature);
	DrawDebugCameraFrustum(scene, cameraFrameData);
	DrawDebugShadowFrustum(scene);

#ifdef WITH_PYTHON
	scene->RunDrawingCallbacks(KX_Scene::PRE_DRAW, rendercam);
#endif

	if (g_useDeferred_GGG) {

		if (!shaderInitialized) {
			shaderInitialized = true;

			const char *vsSrc = R"(
				void main(void)
				{
					gl_Position = gl_Vertex;
					gl_TexCoord[0] = gl_MultiTexCoord0;
				}
			)";

			const char *lightShaderSrc = R"(

			uniform sampler2D bgl_DepthTexture;

			float LinearizeDepth(float depth)
			{
				float near = 0.1;
				float far  = 100.0;
				return (2.0 * near) / (far + near - depth * (far - near));
			}

			void main()
			{
				vec2 uv = gl_TexCoord[0].st;

				float raw = texture2D(bgl_DepthTexture, uv).r;

				float d = LinearizeDepth(raw);

				gl_FragColor = vec4(vec3(d), 1.0);
			}

			)";


			GLuint vs = glCreateShader(GL_VERTEX_SHADER);
			glShaderSource(vs, 1, &vsSrc, nullptr);
			glCompileShader(vs);

			GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
			glShaderSource(fs, 1, &lightShaderSrc, nullptr);
			glCompileShader(fs);

			lightProgram = glCreateProgram();
			glAttachShader(lightProgram, vs);
			glAttachShader(lightProgram, fs);
			glLinkProgram(lightProgram);

			glDeleteShader(vs);
			glDeleteShader(fs);

			glUseProgram(lightProgram);
			for (int i = 0; i < 4; ++i) {
				std::string name = "bgl_DataTextures[" + std::to_string(i) + "]";
				dataTexLoc[i] = glGetUniformLocation(lightProgram, name.c_str());
			}
			depthTexLoc = glGetUniformLocation(lightProgram, "bgl_DepthTexture");
			glUseProgram(0);
		}

		RAS_OffScreen *gbuffer = m_canvas->GetOffScreen(RAS_OffScreen::RAS_OFFSCREEN_GBUFFER);

		GPU_framebuffer_bind_all_attachments(gbuffer->GetFrameBuffer(), gbuffer->GetNumColorSlot());
		glViewport(0, 0, width, height);
		glEnable(GL_DEPTH_TEST);
		glDepthMask(GL_TRUE);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		scene->RenderBuckets(objects,
			m_rasterizer->GetDrawingMode(),
			rendercam->GetWorldToCamera(),
			m_rasterizer,
			gbuffer);

		GPU_framebuffer_restore();

		glUseProgram(lightProgram);

		for (int i = 0; i < gbuffer->GetNumColorSlot() && i < 4; ++i) {
			gbuffer->BindColorTexture(i, 9 + i);
			glUniform1i(dataTexLoc[i], 9 + i);
		}
		gbuffer->BindDepthTexture(8);
		glUniform1i(depthTexLoc, 8);

		glDisable(GL_DEPTH_TEST);
		m_rasterizer->SetViewport(0, 0, m_canvas->GetWidth(), m_canvas->GetHeight());
		m_rasterizer->SetScissor(0, 0, m_canvas->GetWidth(), m_canvas->GetHeight());
		m_rasterizer->DrawOverlayPlane();

		glUseProgram(0);

		for (int i = 0; i < gbuffer->GetNumColorSlot(); ++i)
			gbuffer->UnbindColorTexture(i);
		gbuffer->UnbindDepthTexture();
	}
	else {
		scene->RenderSolidBuckets(objects,
			m_rasterizer->GetDrawingMode(),
			rendercam->GetWorldToCamera(),
			m_rasterizer);


		if (KX_GrassSystem::SceneHasTerrain(scene)) {
			scene->DrawGrassSystem(m_rasterizer);
		}

		scene->RenderAlphaBuckets(
			m_rasterizer->GetDrawingMode(),
			rendercam->GetWorldToCamera(),
			m_rasterizer,
			offScreen);
	}

	if (scene->GetPhysicsEnvironment()) {
		scene->GetPhysicsEnvironment()->DebugDrawWorld();
	}
}

/*
 * To run once per scene
 */
RAS_OffScreen *KX_KetsjiEngine::PostRenderScene(KX_Scene *scene, RAS_OffScreen *inputofs, RAS_OffScreen *targetofs)
{
	KX_SetActiveScene(scene);

	scene->FlushDebugDraw(m_rasterizer, m_canvas);

	const int width = m_canvas->GetWidth();
	const int height = m_canvas->GetHeight();
	m_rasterizer->SetViewport(0, 0, width, height);
	m_rasterizer->SetScissor(0, 0, width, height);

	RAS_OffScreen *offScreen = scene->Render2DFilters(m_rasterizer, m_canvas, inputofs, targetofs);

#ifdef WITH_PYTHON
	scene->RunDrawingCallbacks(KX_Scene::POST_DRAW, nullptr);
	scene->FlushDebugDraw(m_rasterizer, m_canvas);
#endif

	return offScreen;
}

void KX_KetsjiEngine::StopEngine()
{
	if (m_bInitialized) {
		m_converter->FinalizeAsyncLoads();

		while (m_scenes->GetCount() > 0) {
			KX_Scene *scene = m_scenes->GetFront();
			DestructScene(scene);
			// WARNING: here the scene is a dangling pointer.
			m_scenes->Remove(0);
		}

		// cleanup all the stuff
		m_rasterizer->Exit();
	}
}

// Scene Management is able to switch between scenes
// and have several scenes running in parallel
void KX_KetsjiEngine::AddScene(KX_Scene *scene)
{
	m_scenes->Add(CM_AddRef(scene));
	PostProcessScene(scene);
}

void KX_KetsjiEngine::PostProcessScene(KX_Scene *scene)
{
	bool override_camera = (((m_flags & CAMERA_OVERRIDE) != 0) && (scene->GetName() == m_overrideSceneName));

	// if there is no activecamera, or the camera is being
	// overridden we need to construct a temporary camera
	if (!scene->GetActiveCamera() || override_camera) {
		KX_Camera *activecam = nullptr;

		activecam = new KX_Camera(scene, KX_Scene::m_callbacks, override_camera ? m_overrideCamData : RAS_CameraData());
		activecam->SetName("__default__cam__");

		// set transformation
		if (override_camera) {
			activecam->NodeSetLocalPosition(m_overrideCamPosition);
			activecam->NodeSetLocalOrientation(m_overrideCamOrientation);
		}
		else {
			activecam->NodeSetLocalPosition(mt::zero3);
			activecam->NodeSetLocalOrientation(mt::mat3::Identity());
		}

		activecam->NodeUpdate();

		scene->GetCameraList()->Add(CM_AddRef(activecam));
		scene->SetActiveCamera(activecam);
		scene->GetObjectList()->Add(CM_AddRef(activecam));
		scene->GetRootParentList()->Add(CM_AddRef(activecam));
		// done with activecam
		activecam->Release();
	}

	scene->UpdateParents();
}

void KX_KetsjiEngine::RenderDebugProperties()
{
	std::string debugtxt;
	int title_xmargin = -7;
	int title_y_top_margin = 4;
	int title_y_bottom_margin = 2;

	int const_xindent = 4;
	int const_ysize = 14;

	int xcoord = 12;    // mmmm, these constants were taken from blender source
	int ycoord = 17;    // to 'mimic' behavior

	int profile_indent = 72;

	float tottime = m_logger.GetAverage();
	if (tottime < 1e-6f) {
		tottime = 1e-6f;
	}

	static const mt::vec4 white(1.0f, 1.0f, 1.0f, 1.0f);

	if (m_flags & (SHOW_FRAMERATE | SHOW_PROFILE)) {
		// Title for profiling("Profile")
		// Adds the constant x indent (0 for now) to the title x margin
		m_debugDraw.RenderText2d("Profile", mt::vec2(xcoord + const_xindent + title_xmargin, ycoord), white);

		// Increase the indent by default increase
		ycoord += const_ysize;
		// Add the title indent afterwards
		ycoord += title_y_bottom_margin;
	}

	// Framerate display
	if (m_flags & SHOW_FRAMERATE) {
		m_debugDraw.RenderText2d("Frametime :",
		                       mt::vec2(xcoord + const_xindent,
		                                ycoord), white);

		char buf_ft[64];
		snprintf(buf_ft, sizeof(buf_ft), "%5.2fms (%.1ffps)", tottime * 1000.0f, 1.0f / tottime);
		debugtxt.assign(buf_ft);
		m_debugDraw.RenderText2d(debugtxt, mt::vec2(xcoord + const_xindent + profile_indent, ycoord), white);
		// Increase the indent by default increase
		ycoord += const_ysize;
	}

	// Profile display
	if (m_flags & SHOW_PROFILE) {
		for (int j = tc_first; j < tc_numCategories; j++) {
			m_debugDraw.RenderText2d(m_profileLabels[j], mt::vec2(xcoord + const_xindent, ycoord), white);

			double time = m_logger.GetAverage((KX_TimeCategory)j);

			char buf_prof[64];
			snprintf(buf_prof, sizeof(buf_prof), "%5.2fms | %d%%", time * 1000.f, (int)(time / tottime * 100.f));
			debugtxt.assign(buf_prof);
			m_debugDraw.RenderText2d(debugtxt, mt::vec2(xcoord + const_xindent + profile_indent, ycoord), white);

			const mt::vec2 boxSize(50 * (time / tottime), 9);
			m_debugDraw.RenderBox2d(mt::vec2(xcoord + (int)(2.2 * profile_indent), ycoord), boxSize, white);
			ycoord += const_ysize;
		}
	}

	if (m_flags & SHOW_RENDER_QUERIES) {
		m_debugDraw.RenderText2d("Render Queries :", mt::vec2(xcoord + const_xindent + title_xmargin, ycoord), white);
		ycoord += const_ysize;

		for (unsigned short i = 0; i < QUERY_MAX; ++i) {
			m_debugDraw.RenderText2d(m_renderQueriesLabels[i], mt::vec2(xcoord + const_xindent, ycoord), white);

			if (i == QUERY_TIME) {
				char buf_qt[32];
				snprintf(buf_qt, sizeof(buf_qt), "%.2fms", (((float)m_renderQueries[i].Result()) / 1e6));
				debugtxt.assign(buf_qt);
			}
			else {
				char buf_qv[32];
				snprintf(buf_qv, sizeof(buf_qv), "%i", m_renderQueries[i].Result());
				debugtxt.assign(buf_qv);
			}

			m_debugDraw.RenderText2d(debugtxt, mt::vec2(xcoord + const_xindent + profile_indent, ycoord), white);
			ycoord += const_ysize;
		}
	}

	// Add the ymargin for titles below the other section of debug info
	ycoord += title_y_top_margin;

	/* Property display */
	if (m_flags & SHOW_DEBUG_PROPERTIES) {
		// Title for debugging("Debug properties")
		// Adds the constant x indent (0 for now) to the title x margin
		m_debugDraw.RenderText2d("Debug Properties", mt::vec2(xcoord + const_xindent + title_xmargin, ycoord), white);

		// Increase the indent by default increase
		ycoord += const_ysize;
		// Add the title indent afterwards
		ycoord += title_y_bottom_margin;

		/* Calculate amount of properties that can displayed. */
		const unsigned short propsMax = (m_canvas->GetHeight() - ycoord) / const_ysize;

		for (KX_Scene *scene : m_scenes) {
			scene->RenderDebugProperties(m_debugDraw, const_xindent, const_ysize, xcoord, ycoord, propsMax);
		}
	}

	m_debugDraw.Flush(m_rasterizer, m_canvas);
}

void KX_KetsjiEngine::DrawDebugCameraFrustum(KX_Scene *scene, const CameraRenderData& cameraFrameData)
{
	if (m_showCameraFrustum == KX_DebugOption::DISABLE) {
		return;
	}

	RAS_DebugDraw& debugDraw = scene->GetDebugDraw();
	for (KX_Camera *cam : scene->GetCameraList()) {
		if (cam != cameraFrameData.m_renderCamera && (m_showCameraFrustum == KX_DebugOption::FORCE || cam->GetShowCameraFrustum())) {

			cam->UpdateView(m_rasterizer, scene, cameraFrameData.m_stereoMode, cameraFrameData.m_eye,
					cameraFrameData.m_viewport, cameraFrameData.m_area);

			debugDraw.DrawCameraFrustum(
				cam->GetProjectionMatrix(cameraFrameData.m_eye) * cam->GetModelviewMatrix(cameraFrameData.m_eye));
		}
	}
}

void KX_KetsjiEngine::DrawDebugShadowFrustum(KX_Scene *scene)
{
	if (m_showShadowFrustum == KX_DebugOption::DISABLE) {
		return;
	}

	RAS_DebugDraw& debugDraw = scene->GetDebugDraw();
	for (KX_LightObject *light : scene->GetLightList()) {
		RAS_ILightObject *raslight = light->GetLightData();
		if (m_showShadowFrustum == KX_DebugOption::FORCE || light->GetShowShadowFrustum()) {
			const mt::mat4 projmat(raslight->GetWinMat());
			const mt::mat4 viewmat(raslight->GetViewMat());

			debugDraw.DrawCameraFrustum(projmat * viewmat);
		}
	}
}

EXP_ListValue<KX_Scene> *KX_KetsjiEngine::CurrentScenes()
{
	return m_scenes;
}

KX_Scene *KX_KetsjiEngine::FindScene(const std::string& scenename)
{
	return m_scenes->FindValue(scenename);
}

void KX_KetsjiEngine::ConvertAndAddScene(const std::string& scenename, bool overlay)
{
	// only add scene when it doesn't exist!
	if (FindScene(scenename)) {
		CM_Warning("scene " << scenename << " already exists, not added!");
	}
	else {
		if (overlay) {
			m_addingOverlayScenes.push_back(scenename);
		}
		else {
			m_addingBackgroundScenes.push_back(scenename);
		}
	}
}

void KX_KetsjiEngine::RemoveScene(const std::string& scenename)
{
	if (FindScene(scenename)) {
		m_removingScenes.push_back(scenename);
	}
	else {
		CM_Warning("scene " << scenename << " does not exist, not removed!");
	}
}

void KX_KetsjiEngine::RemoveScheduledScenes()
{
	if (!m_removingScenes.empty()) {
		std::vector<std::string>::iterator scenenameit;
		for (scenenameit = m_removingScenes.begin(); scenenameit != m_removingScenes.end(); scenenameit++) {
			std::string scenename = *scenenameit;

			KX_Scene *scene = FindScene(scenename);
			if (scene) {
				DestructScene(scene);
				m_scenes->RemoveValue(scene);
			}
		}
		m_removingScenes.clear();
	}
}

KX_Scene *KX_KetsjiEngine::CreateScene(Scene *scene)
{
	KX_Scene *tmpscene = new KX_Scene(m_inputDevice,
	                                  scene->id.name + 2,
	                                  scene,
	                                  m_canvas,
	                                  m_networkMessageManager);

	return tmpscene;
}

KX_Scene *KX_KetsjiEngine::CreateScene(const std::string& scenename)
{
	Scene *scene = m_converter->GetBlenderSceneForName(scenename);
	if (!scene) {
		return nullptr;
	}

	return CreateScene(scene);
}

void KX_KetsjiEngine::AddScheduledScenes()
{
	if (!m_addingOverlayScenes.empty()) {
		for (const std::string& scenename : m_addingOverlayScenes) {
			KX_Scene *tmpscene = CreateScene(scenename);

			if (tmpscene) {
				m_converter->ConvertScene(tmpscene);
				m_scenes->Add(CM_AddRef(tmpscene));
				PostProcessScene(tmpscene);
				tmpscene->Release();
			}
			else {
				CM_Warning("scene " << scenename << " could not be found, not added!");
			}
		}
		m_addingOverlayScenes.clear();
	}

	if (!m_addingBackgroundScenes.empty()) {
		for (const std::string& scenename : m_addingBackgroundScenes) {
			KX_Scene *tmpscene = CreateScene(scenename);

			if (tmpscene) {
				m_converter->ConvertScene(tmpscene);
				m_scenes->Insert(0, CM_AddRef(tmpscene));
				PostProcessScene(tmpscene);
				tmpscene->Release();
			}
			else {
				CM_Warning("scene " << scenename << " could not be found, not added!");
			}
		}
		m_addingBackgroundScenes.clear();
	}
}

bool KX_KetsjiEngine::ReplaceScene(const std::string& oldscene, const std::string& newscene)
{
	// Don't allow replacement if the new scene doesn't exist.
	// Allows smarter game design (used to have no check here).
	// Note that it creates a small backward compatbility issue
	// for a game that did a replace followed by a lib load with the
	// new scene in the lib => it won't work anymore, the lib
	// must be loaded before doing the replace.
	if (m_converter->GetBlenderSceneForName(newscene)) {
		m_replace_scenes.emplace_back(oldscene, newscene);
		return true;
	}

	return false;
}

// replace scene is not the same as removing and adding because the
// scene must be in exact the same place (to maintain drawingorder)
// (nzc) - should that not be done with a scene-display list? It seems
// stupid to rely on the mem allocation order...
void KX_KetsjiEngine::ReplaceScheduledScenes()
{
	if (!m_replace_scenes.empty()) {
		std::vector<std::pair<std::string, std::string> >::iterator scenenameit;

		for (scenenameit = m_replace_scenes.begin();
		     scenenameit != m_replace_scenes.end();
		     scenenameit++)
		{
			std::string oldscenename = (*scenenameit).first;
			std::string newscenename = (*scenenameit).second;
			/* Scenes are not supposed to be included twice... I think */
			for (unsigned int sce_idx = 0; sce_idx < m_scenes->GetCount(); ++sce_idx) {
				KX_Scene *scene = m_scenes->GetValue(sce_idx);
				if (scene->GetName() == oldscenename) {
					// avoid crash if the new scene doesn't exist, just do nothing
					Scene *blScene = m_converter->GetBlenderSceneForName(newscenename);
					if (blScene) {
						DestructScene(scene);

						KX_Scene *tmpscene = CreateScene(blScene);
						m_converter->ConvertScene(tmpscene);

						m_scenes->SetValue(sce_idx, CM_AddRef(tmpscene));
						PostProcessScene(tmpscene);
						tmpscene->Release();
					}
					else {
						CM_Warning("scene " << newscenename << " could not be found, not replaced!");
					}
				}
			}
		}
		m_replace_scenes.clear();
	}
}

void KX_KetsjiEngine::SuspendScene(const std::string& scenename)
{
	KX_Scene *scene = FindScene(scenename);
	if (scene) {
		scene->Suspend();
	}
}

void KX_KetsjiEngine::ResumeScene(const std::string& scenename)
{
	KX_Scene *scene = FindScene(scenename);
	if (scene) {
		scene->Resume();
	}
}

void KX_KetsjiEngine::DestructScene(KX_Scene *scene)
{
	scene->RunOnRemoveCallbacks();
	m_converter->RemoveScene(scene);
}

double KX_KetsjiEngine::GetTicRate()
{
	return m_ticrate;
}

void KX_KetsjiEngine::SetTicRate(double ticrate)
{
	m_ticrate = ticrate;
}

double KX_KetsjiEngine::GetTimeScale() const
{
	return m_timescale;
}

void KX_KetsjiEngine::SetTimeScale(double timeScale)
{
	m_timescale = timeScale;
}

int KX_KetsjiEngine::GetMaxLogicFrame()
{
	return m_maxLogicFrame;
}

void KX_KetsjiEngine::SetMaxLogicFrame(int frame)
{
	m_maxLogicFrame = frame;
}

int KX_KetsjiEngine::GetMaxPhysicsFrame()
{
	return m_maxPhysicsFrame;
}

void KX_KetsjiEngine::SetMaxPhysicsFrame(int frame)
{
	m_maxPhysicsFrame = frame;
}

double KX_KetsjiEngine::GetAnimFrameRate()
{
	return m_anim_framerate;
}

bool KX_KetsjiEngine::GetFlag(FlagType flag) const
{
	return (m_flags & flag) != 0;
}

void KX_KetsjiEngine::SetFlag(FlagType flag, bool enable)
{
	if (enable) {
		m_flags = (FlagType)(m_flags | flag);
	}
	else {
		m_flags = (FlagType)(m_flags & ~flag);
	}
}

double KX_KetsjiEngine::GetClockTime() const
{
	return m_clockTime;
}

void KX_KetsjiEngine::SetClockTime(double externalClockTime)
{
	m_clockTime = externalClockTime;
}

double KX_KetsjiEngine::GetFrameTime() const
{
	return m_frameTime;
}

double KX_KetsjiEngine::GetRealTime() const
{
	return m_clock.GetTimeSecond();
}

void KX_KetsjiEngine::SetAnimFrameRate(double framerate)
{
	m_anim_framerate = framerate;
}

double KX_KetsjiEngine::GetAverageFrameRate()
{
	return m_average_framerate;
}

void KX_KetsjiEngine::SetExitKey(SCA_IInputDevice::SCA_EnumInputs key)
{
	m_exitKey = key;
}

SCA_IInputDevice::SCA_EnumInputs KX_KetsjiEngine::GetExitKey() const
{
	return m_exitKey;
}

void KX_KetsjiEngine::SetRender(bool render)
{
	m_doRender = render;
}

bool KX_KetsjiEngine::GetRender()
{
	return m_doRender;
}

void KX_KetsjiEngine::ProcessScheduledScenes()
{
	// Check whether there will be changes to the list of scenes
	if (!(m_addingOverlayScenes.empty() && m_addingBackgroundScenes.empty() &&
	      m_replace_scenes.empty() && m_removingScenes.empty())) {
		// Change the scene list
		ReplaceScheduledScenes();
		RemoveScheduledScenes();
		AddScheduledScenes();
	}

	if (m_scenes->Empty()) {
		RequestExit(KX_ExitInfo::NO_SCENES_LEFT);
	}
}

void KX_KetsjiEngine::SetShowBoundingBox(KX_DebugOption mode)
{
	m_showBoundingBox = mode;
}

KX_DebugOption KX_KetsjiEngine::GetShowBoundingBox() const
{
	return m_showBoundingBox;
}

void KX_KetsjiEngine::SetShowArmatures(KX_DebugOption mode)
{
	m_showArmature = mode;
}

KX_DebugOption KX_KetsjiEngine::GetShowArmatures() const
{
	return m_showArmature;
}

void KX_KetsjiEngine::SetShowCameraFrustum(KX_DebugOption mode)
{
	m_showCameraFrustum = mode;
}

KX_DebugOption KX_KetsjiEngine::GetShowCameraFrustum() const
{
	return m_showCameraFrustum;
}

void KX_KetsjiEngine::SetShowShadowFrustum(KX_DebugOption mode)
{
	m_showShadowFrustum = mode;
}

KX_DebugOption KX_KetsjiEngine::GetShowShadowFrustum() const
{
	return m_showShadowFrustum;
}

void KX_KetsjiEngine::Resize()
{
	/* extended mode needs to recalculate camera frusta when */
	KX_Scene *firstscene = m_scenes->GetFront();
	const RAS_FrameSettings &framesettings = firstscene->GetFramingType();

	if (framesettings.FrameType() == RAS_FrameSettings::e_frame_extend) {
		for (KX_Scene *scene : m_scenes) {
			KX_Camera *cam = scene->GetActiveCamera();
			cam->InvalidateProjectionMatrix();
		}
	}
}

void KX_KetsjiEngine::SetGlobalSettings(GlobalSettings *gs)
{
	m_globalsettings.glslflag = gs->glslflag;
}

GlobalSettings *KX_KetsjiEngine::GetGlobalSettings()
{
	return &m_globalsettings;
}
