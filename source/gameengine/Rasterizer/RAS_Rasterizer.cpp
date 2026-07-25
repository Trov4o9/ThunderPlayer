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

/** \file gameengine/Rasterizer/RAS_Rasterizer.cpp
 *  \ingroup bgerastogl
 */

#include "RAS_Rasterizer.h"
#include "RAS_OpenGLRasterizer.h"
#include "RAS_OpenGLDebugDraw.h"
#include "RAS_IMaterial.h"
#include "RAS_DisplayArrayBucket.h"
#include "RAS_InstancingBuffer.h"
#include "RAS_2DFilterManager.h"

#include "RAS_ICanvas.h"
#include "RAS_OffScreen.h"
#include "RAS_Rect.h"
#include "RAS_TextUser.h"
#include "RAS_ILightObject.h"

#include "RAS_OpenGLLight.h"
#include "RAS_OpenGLSync.h"

#include "GPU_draw.h"
#include "GPU_extensions.h"
#include "GPU_material.h"
#include "GPU_shader.h"
#include "GPU_texture.h"

#include "BLI_math_vector.h"

extern "C" {
#  include "BKE_global.h"
#  include "BLF_api.h"
#  include "DNA_material_types.h"
}

#include "MEM_guardedalloc.h"

// XXX Clean these up <<<
#include "KX_RayCast.h"
#include "KX_GameObject.h"
// >>>

#include "CM_Message.h"
#include "CM_List.h"

RAS_Rasterizer::RAS_Rasterizer()
	:m_time(0.0f),
	m_frameCount(0),
	m_ambient(mt::zero3),
	m_viewmatrix(mt::mat4::Identity()),
	m_viewinvmatrix(mt::mat4::Identity()),
	m_campos(mt::zero3),
	m_camortho(false),
	m_camnegscale(false),
	m_stereomode(RAS_STEREO_NOSTEREO),
	m_curreye(RAS_STEREO_LEFTEYE),
	m_eyeseparation(0.0f),
	m_focallength(0.0f),
	m_setfocallength(false),
	m_noOfScanlines(32),
	m_motionblur(0),
	m_motionblurvalue(-1.0f),
	m_clientobject(nullptr),
	m_auxilaryClientInfo(nullptr),
	m_drawingmode(RAS_TEXTURED),
	m_shadowMode(RAS_SHADOW_NONE),
	m_invertFrontFace(false),
	m_overrideShader(RAS_OVERRIDE_SHADER_NONE),
	m_currentGPUMaterial(nullptr)
{
	m_state.frontFace = true;
	m_state.cullFace = false;
	m_state.alphablend = -1;
	m_state.polyOffset[0] = 0.0f;
	m_state.polyOffset[1] = 0.0f;

	m_impl.reset(new RAS_OpenGLRasterizer(this));
	m_debugDrawImpl.reset(new RAS_OpenGLDebugDraw());

	m_numgllights = m_impl->GetNumLights();

	InitOverrideShadersInterface();

	m_state.frontFace = -1;
	m_state.cullFace = -1;
	m_state.alphablend = -1;
	m_state.blend = -1;
	m_state.alphaTest = -1;
	m_state.depthTest = -1;
	m_state.multisample = -1;
	m_state.scissor = -1;
	m_state.polyOffset[0] = -1.0f;
	m_state.polyOffset[1] = -1.0f;
}

RAS_Rasterizer::~RAS_Rasterizer()
{
}

void RAS_Rasterizer::Enable(RAS_Rasterizer::EnableBit bit)
{
	switch (bit) {
		case RAS_BLEND:
			if (m_state.blend == 1) return;
			m_state.blend = 1;
			break;
		case RAS_ALPHA_TEST:
			if (m_state.alphaTest == 1) return;
			m_state.alphaTest = 1;
			break;
		case RAS_DEPTH_TEST:
			if (m_state.depthTest == 1) return;
			m_state.depthTest = 1;
			break;
		case RAS_MULTISAMPLE:
			if (m_state.multisample == 1) return;
			m_state.multisample = 1;
			break;
		case RAS_SCISSOR_TEST:
			if (m_state.scissor == 1) return;
			m_state.scissor = 1;
			break;
		default:
			break;
	}
	m_impl->Enable(bit);
}

void RAS_Rasterizer::Disable(RAS_Rasterizer::EnableBit bit)
{
	switch (bit) {
		case RAS_BLEND:
			if (m_state.blend == 0) return;
			m_state.blend = 0;
			break;
		case RAS_ALPHA_TEST:
			if (m_state.alphaTest == 0) return;
			m_state.alphaTest = 0;
			break;
		case RAS_DEPTH_TEST:
			if (m_state.depthTest == 0) return;
			m_state.depthTest = 0;
			break;
		case RAS_MULTISAMPLE:
			if (m_state.multisample == 0) return;
			m_state.multisample = 0;
			break;
		case RAS_SCISSOR_TEST:
			if (m_state.scissor == 0) return;
			m_state.scissor = 0;
			break;
		default:
			break;
	}
	m_impl->Disable(bit);
}

void RAS_Rasterizer::SetDepthFunc(RAS_Rasterizer::DepthFunc func)
{
	m_impl->SetDepthFunc(func);
}

void RAS_Rasterizer::SetBlendFunc(BlendFunc src, BlendFunc dst)
{
	m_impl->SetBlendFunc(src, dst);
}

void RAS_Rasterizer::SetAmbientColor(const mt::vec3& color)
{
	m_ambient = color;
}

void RAS_Rasterizer::SetAmbient(float factor)
{
	m_impl->SetAmbient(m_ambient, factor);
}

void RAS_Rasterizer::SetFog(short type, float start, float dist, float intensity, const mt::vec3& color)
{
	m_impl->SetFog(type, start, dist, intensity, color);
}

void RAS_Rasterizer::ResetBlendState()
{
    if (m_state.blend != 0)
        Disable(RAS_BLEND);
    if (m_state.alphaTest != 0)
        Disable(RAS_ALPHA_TEST);
    SetAlphaBlend(GPU_BLEND_SOLID);
}

void RAS_Rasterizer::ResetRasterizerState()
{
    SetFrontFace(true);
    SetColorMask(true, true, true, true);
}

void RAS_Rasterizer::Init()
{
    GPU_state_init();

    ResetBlendState();
    SetFrontFace(true);
    SetColorMask(true, true, true, true);
    
    // Inicializar culling como enabled (padrão da engine)
    // Materiais individuais podem desabilitar via sua flag GEMAT_BACKCULL
    SetCullFace(true);

    m_impl->Init();

    m_state.multisample = -1;
    m_state.scissor = -1;
}

void RAS_Rasterizer::BeginFrame(double time)
{
	m_time = time;

	m_state.polyOffset[0] = -1.0f;
	m_state.polyOffset[1] = -1.0f;

	SetCullFace(true);
	Enable(RAS_DEPTH_TEST);

	Disable(RAS_BLEND);
	Disable(RAS_ALPHA_TEST);

	GPU_set_material_alpha_blend(GPU_BLEND_SOLID);

	SetFrontFace(true);

	m_impl->BeginFrame();

	if (m_state.multisample != 1)
		Enable(RAS_MULTISAMPLE);
	if (m_state.scissor != 1)
		Enable(RAS_SCISSOR_TEST);

	SetDepthFunc(RAS_LEQUAL);

	// Render Tools
	m_clientobject = nullptr;
	m_lastlightlayer = -1;
	m_lastauxinfo = nullptr;
	m_lastlighting = true; 

	DisableLights();
}

bool RAS_Rasterizer::NeedsMeshUserUpdate(void *material, const mt::mat4& matrix, const mt::vec4& color,
                                         unsigned int layer, float random, short passIndex)
{
	if (m_lastMeshUserState.material != material) return true;
	if (m_lastMeshUserState.layer != layer) return true;
	if (m_lastMeshUserState.passIndex != passIndex) return true;
	if (m_lastMeshUserState.random != random) return true;
	if (m_lastMeshUserState.color != color) return true;
	
	// Exact comparison for matrix to avoid memcmp as requested
	for (int i = 0; i < 4; ++i) {
		if (m_lastMeshUserState.matrix.GetColumn(i) != matrix.GetColumn(i)) return true;
	}
	return false;
}

void RAS_Rasterizer::UpdateMeshUserState(void *material, const mt::mat4& matrix, const mt::vec4& color,
                                         unsigned int layer, float random, short passIndex)
{
	m_lastMeshUserState.material = material;
	m_lastMeshUserState.matrix = matrix;
	m_lastMeshUserState.color = color;
	m_lastMeshUserState.layer = layer;
	m_lastMeshUserState.random = random;
	m_lastMeshUserState.passIndex = passIndex;
}

void RAS_Rasterizer::Exit()
{
    ResetRasterizerState();
    Enable(RAS_DEPTH_TEST);

    SetClearDepth(1.0f);
    SetClearColor(0.0f, 0.0f, 0.0f, 0.0f);

    Clear(RAS_COLOR_BUFFER_BIT | RAS_DEPTH_BUFFER_BIT);
    SetDepthMask(RAS_DEPTHMASK_ENABLED);
    SetDepthFunc(RAS_LEQUAL);
    SetBlendFunc(RAS_ONE, RAS_ZERO);

    Disable(RAS_POLYGON_STIPPLE);
    Disable(RAS_LIGHTING);

    m_impl->Exit();

    ResetGlobalDepthTexture();
    EndFrame();
}

void RAS_Rasterizer::EndFrame()
{
	SetColorMask(true, true, true, true);

	Disable(RAS_MULTISAMPLE);
}

void RAS_Rasterizer::SetDrawingMode(RAS_Rasterizer::DrawType drawingmode)
{
	m_drawingmode = drawingmode;
}

RAS_Rasterizer::DrawType RAS_Rasterizer::GetDrawingMode()
{
	return m_drawingmode;
}

void RAS_Rasterizer::SetShadowMode(RAS_Rasterizer::ShadowType shadowmode)
{
	m_shadowMode = shadowmode;
}

RAS_Rasterizer::ShadowType RAS_Rasterizer::GetShadowMode()
{
	return m_shadowMode;
}

void RAS_Rasterizer::SetCurrentGPUMaterial(GPUMaterial *material)
{
	m_currentGPUMaterial = material;
}

GPUMaterial *RAS_Rasterizer::GetCurrentGPUMaterial() const
{
	return m_currentGPUMaterial;
}

void RAS_Rasterizer::SetDepthMask(DepthMask depthmask)
{
	m_impl->SetDepthMask(depthmask);
}

unsigned int *RAS_Rasterizer::MakeScreenshot(int x, int y, int width, int height)
{
	return m_impl->MakeScreenshot(x, y, width, height);
}

void RAS_Rasterizer::Clear(int clearbit)
{
	m_impl->Clear(clearbit);
}

void RAS_Rasterizer::SetClearColor(float r, float g, float b, float a)
{
	m_impl->SetClearColor(r, g, b, a);
}

void RAS_Rasterizer::SetClearDepth(float d)
{
	m_impl->SetClearDepth(d);
}

void RAS_Rasterizer::SetColorMask(bool r, bool g, bool b, bool a)
{
	m_impl->SetColorMask(r, g, b, a);
}

void RAS_Rasterizer::DrawOverlayPlane()
{
	m_impl->DrawOverlayPlane();
}

inline void RAS_Rasterizer::PrepareDrawOffScreen(RAS_ICanvas *canvas)
{
	RAS_OffScreen::RestoreScreen();
	const int *vp = canvas->GetViewPort();
	int w, h;

	if (canvas->IsEmbedded()) {
		w = canvas->GetWidth();
		h = canvas->GetHeight();
	}
	else {
		w = g_vpwi;
		h = g_vphi;
	}

	SetViewport(vp[0], vp[1], w, h);
	SetScissor(vp[0], vp[1], w, h);

	SetFrontFace(true);
	SetDepthFunc(RAS_ALWAYS);
}
/*
inline void RAS_Rasterizer::PrepareDrawOffScreen(RAS_ICanvas *canvas)
{
    RAS_OffScreen::RestoreScreen();
    const int *vp = canvas->GetViewPort();
    SetViewport(vp[0], vp[1], g_vpw, g_vph);
    SetScissor(vp[0], vp[1], g_vpw, g_vph);
    SetFrontFace(true);
    SetDepthFunc(RAS_ALWAYS);
}
*/
inline void RAS_Rasterizer::DrawOffScreenWithShader(RAS_OffScreen *src, GPUShader *shader, int tex0, int tex1, bool bindTex0, bool bindTex1)
{
    if (bindTex0) src->BindColorTexture(0, tex0);
    if (bindTex1 && tex1 >= 0) src->BindColorTexture(1, tex1);

    GPU_shader_bind(shader);

    if (bindTex0) GPU_shader_uniform_int(shader, 0, tex0);
    if (bindTex1 && tex1 >= 0) GPU_shader_uniform_int(shader, 1, tex1);

    DrawOverlayPlane();  

    GPU_shader_unbind();

    if (bindTex0) src->UnbindColorTexture(0);
    if (bindTex1 && tex1 >= 0) src->UnbindColorTexture(0);
}


void RAS_Rasterizer::DrawOffScreen(RAS_OffScreen *srcOffScreen, RAS_OffScreen *dstOffScreen)
{
    if (srcOffScreen->GetSamples() > 0) {
        srcOffScreen->Blit(dstOffScreen, true);
        if (dstOffScreen) dstOffScreen->Bind();
        return;
    }

    GPUShader *shader = GPU_shader_get_builtin_shader(GPU_SHADER_DRAW_FRAME_BUFFER);
    DrawOffScreenWithShader(srcOffScreen, shader);
}

void RAS_Rasterizer::DrawOffScreen(RAS_ICanvas *canvas, RAS_OffScreen *offScreen)
{
    if (offScreen->GetSamples() > 0)
        offScreen = offScreen->Blit(canvas->GetOffScreen(RAS_OffScreen::RAS_OFFSCREEN_EYE_LEFT1), false);

    PrepareDrawOffScreen(canvas);
    DrawOffScreen(offScreen, nullptr);
    SetDepthFunc(RAS_LEQUAL);
}

void RAS_Rasterizer::DrawStereoOffScreen(RAS_ICanvas *canvas, RAS_OffScreen *leftOffScreen, RAS_OffScreen *rightOffScreen, StereoMode stereoMode)
{
    if (leftOffScreen->GetSamples() > 0)
        leftOffScreen = leftOffScreen->Blit(canvas->GetOffScreen(RAS_OffScreen::RAS_OFFSCREEN_EYE_LEFT1), false);
    if (rightOffScreen->GetSamples() > 0)
        rightOffScreen = rightOffScreen->Blit(canvas->GetOffScreen(RAS_OffScreen::RAS_OFFSCREEN_EYE_RIGHT1), false);

    PrepareDrawOffScreen(canvas);

    if (stereoMode == RAS_STEREO_VINTERLACE || stereoMode == RAS_STEREO_INTERLACED) {
        GPUShader *shader = GPU_shader_get_builtin_shader(GPU_SHADER_STEREO_STIPPLE);
        OverrideShaderStereoStippleInterface *interface = (OverrideShaderStereoStippleInterface *)GPU_shader_get_interface(shader);
        DrawOffScreenWithShader(leftOffScreen, shader, 0, 1, true, true);
        GPU_shader_bind(shader);
        GPU_shader_uniform_int(shader, interface->stippleIdLoc, (stereoMode == RAS_STEREO_INTERLACED) ? 1 : 0);
        GPU_shader_unbind();
    }
    else if (stereoMode == RAS_STEREO_ANAGLYPH) {
        GPUShader *shader = GPU_shader_get_builtin_shader(GPU_SHADER_STEREO_ANAGLYPH);
        DrawOffScreenWithShader(leftOffScreen, shader, 0, 1, true, true);
    }

    SetDepthFunc(RAS_LEQUAL);
}

RAS_Rect RAS_Rasterizer::GetRenderArea(RAS_ICanvas *canvas, StereoMode stereoMode, StereoEye eye)
{
	RAS_Rect area;
	// only above/below stereo method needs viewport adjustment
	switch (stereoMode) {
		case RAS_STEREO_ABOVEBELOW:
		{
			switch (eye) {
				case RAS_STEREO_LEFTEYE:
				{
					// upper half of window
					area.SetLeft(0);
					area.SetBottom(canvas->GetHeight() - (canvas->GetHeight() - m_noOfScanlines - 1) / 2);

					area.SetRight(canvas->GetMaxX());
					area.SetTop(canvas->GetMaxY());
					break;
				}
				case RAS_STEREO_RIGHTEYE:
				{
					// lower half of window
					area.SetLeft(0);
					area.SetBottom(0);
					area.SetRight(canvas->GetMaxX());
					area.SetTop((canvas->GetMaxY() - m_noOfScanlines) / 2);
					break;
				}
				default:
				{
					break;
				}
			}
			break;
		}
		case RAS_STEREO_3DTVTOPBOTTOM:
		{
			switch (eye) {
				case RAS_STEREO_LEFTEYE:
				{
					// upper half of window
					area.SetLeft(0);
					area.SetBottom(canvas->GetHeight() - canvas->GetHeight() / 2);

					area.SetRight(canvas->GetWidth() - 1);
					area.SetTop(canvas->GetHeight() - 1);
					break;
				}
				case RAS_STEREO_RIGHTEYE:
				{
					// lower half of window
					area.SetLeft(0);
					area.SetBottom(0);
					area.SetRight(canvas->GetWidth() - 1);
					area.SetTop((canvas->GetHeight() - 1) / 2);
					break;
				}
				default:
				{
					break;
				}
			}
			break;
		}
		case RAS_STEREO_SIDEBYSIDE:
		{
			switch (eye) {
				case RAS_STEREO_LEFTEYE:
				{
					// Left half of window
					area.SetLeft(0);
					area.SetBottom(0);
					area.SetRight((canvas->GetWidth() - 1) / 2);
					area.SetTop(canvas->GetHeight() - 1);
					break;
				}
				case RAS_STEREO_RIGHTEYE:
				{
					// Right half of window
					area.SetLeft(canvas->GetWidth() / 2);
					area.SetBottom(0);
					area.SetRight(canvas->GetWidth() - 1);
					area.SetTop(canvas->GetHeight() - 1);
					break;
				}
				default:
				{
					break;
				}
			}
			break;
		}
		default:
		{
			// every available pixel
			area.SetLeft(0);
			area.SetBottom(0);
			area.SetRight(canvas->GetWidth() - 1);
			area.SetTop(canvas->GetHeight() - 1);
			break;
		}
	}

	return area;
}
RAS_Rect RAS_Rasterizer::GetRenderAreaMouse(RAS_ICanvas *canvas, StereoMode stereoMode, StereoEye eye)
{
	RAS_Rect area;
	// only above/below stereo method needs viewport adjustment
	switch (stereoMode) {
		case RAS_STEREO_ABOVEBELOW:
		{
			switch (eye) {
				case RAS_STEREO_LEFTEYE:
				{
					// upper half of window
					area.SetLeft(0);
					area.SetBottom(canvas->GetHeight() - (canvas->GetHeight() - m_noOfScanlines - 1) / 2);

					area.SetRight(canvas->GetMaxX());
					area.SetTop(canvas->GetMaxY());
					break;
				}
				case RAS_STEREO_RIGHTEYE:
				{
					// lower half of window
					area.SetLeft(0);
					area.SetBottom(0);
					area.SetRight(canvas->GetMaxX());
					area.SetTop((canvas->GetMaxY() - m_noOfScanlines) / 2);
					break;
				}
				default:
				{
					break;
				}
			}
			break;
		}
		case RAS_STEREO_3DTVTOPBOTTOM:
		{
			switch (eye) {
				case RAS_STEREO_LEFTEYE:
				{
					// upper half of window
					area.SetLeft(0);
					area.SetBottom(canvas->GetHeight() - canvas->GetHeight() / 2);

					area.SetRight(canvas->GetWidth() - 1);
					area.SetTop(canvas->GetHeight() - 1);
					break;
				}
				case RAS_STEREO_RIGHTEYE:
				{
					// lower half of window
					area.SetLeft(0);
					area.SetBottom(0);
					area.SetRight(canvas->GetWidth() - 1);
					area.SetTop((canvas->GetHeight() - 1) / 2);
					break;
				}
				default:
				{
					break;
				}
			}
			break;
		}
		case RAS_STEREO_SIDEBYSIDE:
		{
			switch (eye) {
				case RAS_STEREO_LEFTEYE:
				{
					// Left half of window
					area.SetLeft(0);
					area.SetBottom(0);
					area.SetRight((canvas->GetWidth() - 1) / 2);
					area.SetTop(canvas->GetHeight() - 1);
					break;
				}
				case RAS_STEREO_RIGHTEYE:
				{
					// Right half of window
					area.SetLeft(canvas->GetWidth() / 2);
					area.SetBottom(0);
					area.SetRight(canvas->GetWidth() - 1);
					area.SetTop(canvas->GetHeight() - 1);
					break;
				}
				default:
				{
					break;
				}
			}
			break;
		}
		default:
		{
			// every available pixel
			area.SetLeft(0);
			area.SetBottom(0);
			area.SetRight(canvas->GetWidth() - 1);
			area.SetTop(canvas->GetHeight() - 1);
			break;
		}
	}

	return area;
}
void RAS_Rasterizer::SetStereoMode(const StereoMode stereomode)
{
	m_stereomode = stereomode;
}

RAS_Rasterizer::StereoMode RAS_Rasterizer::GetStereoMode()
{
	return m_stereomode;
}

void RAS_Rasterizer::SetEye(const StereoEye eye)
{
	m_curreye = eye;
}

RAS_Rasterizer::StereoEye RAS_Rasterizer::GetEye()
{
	return m_curreye;
}

void RAS_Rasterizer::SetEyeSeparation(const float eyeseparation)
{
	m_eyeseparation = eyeseparation;
}

float RAS_Rasterizer::GetEyeSeparation()
{
	return m_eyeseparation;
}

void RAS_Rasterizer::SetFocalLength(const float focallength)
{
	m_focallength = focallength;
	m_setfocallength = true;
}

float RAS_Rasterizer::GetFocalLength()
{
	return m_focallength;
}

RAS_ISync *RAS_Rasterizer::CreateSync(int type)
{
	RAS_ISync *sync = new RAS_OpenGLSync();

	if (!sync->Create((RAS_ISync::RAS_SYNC_TYPE)type)) {
		delete sync;
		return nullptr;
	}
	return sync;
}

const mt::mat4& RAS_Rasterizer::GetViewMatrix() const
{
	return m_viewmatrix;
}

const mt::mat4& RAS_Rasterizer::GetViewInvMatrix() const
{
	return m_viewinvmatrix;
}

void RAS_Rasterizer::IndexPrimitivesText(RAS_MeshSlot *ms)
{
	RAS_TextUser *textUser = (RAS_TextUser *)ms->m_meshUser;

	float mat[16];
	textUser->GetMatrix().Pack(mat);

	const mt::vec3& spacing = textUser->GetSpacing();
	const mt::vec3& offset = textUser->GetOffset();

	mat[12] += offset[0];
	mat[13] += offset[1];
	mat[14] += offset[2];

	for (unsigned short int i = 0, size = textUser->GetTexts().size(); i < size; ++i) {
		if (i != 0) {
			mat[12] -= spacing[0];
			mat[13] -= spacing[1];
			mat[14] -= spacing[2];
		}
		RenderText3D(textUser->GetFontId(), textUser->GetTexts()[i], textUser->GetSize(), textUser->GetDpi(),
		             textUser->GetColor().Data(), mat, textUser->GetAspect());
	}
}

void RAS_Rasterizer::SetProjectionMatrix(const mt::mat4 & mat)
{
	SetMatrixMode(RAS_PROJECTION);
	LoadMatrix((float *)mat.Data());

	m_camortho = (mat(3, 3) != 0.0f);
}

mt::mat4 RAS_Rasterizer::GetFrustumMatrix(StereoMode /*stereoMode*/, StereoEye eye, float focallength,
                                          float left, float right, float bottom, float top, float frustnear, float frustfar)
{
    if (!m_setfocallength) {
        m_focallength = (focallength == 0.0f) ? m_eyeseparation * 30.0f : focallength;
    }

    const float near_div_focallength = frustnear / m_focallength;
    const float offset = 0.5f * m_eyeseparation * near_div_focallength;

    float eyeSign = 0.0f;
    if (eye == RAS_STEREO_LEFTEYE) eyeSign = 1.0f;
    else if (eye == RAS_STEREO_RIGHTEYE) eyeSign = -1.0f;

    left  += eyeSign * offset;
    right += eyeSign * offset;

    if (/* stereoMode == RAS_STEREO_3DTVTOPBOTTOM */ false) {
        bottom *= 2.0f;
        top *= 2.0f;
    }

    return GetFrustumMatrix(left, right, bottom, top, frustnear, frustfar);
}

mt::mat4 RAS_Rasterizer::GetFrustumMatrix(float left, float right, float bottom, float top, float frustnear, float frustfar)
{
	return mt::mat4::Perspective(left, right, bottom, top, frustnear, frustfar);
}

mt::mat4 RAS_Rasterizer::GetOrthoMatrix(float left,
                                        float right,
                                        float bottom,
                                        float top,
                                        float frustnear,
                                        float frustfar)
{
	return mt::mat4::Ortho(left, right, bottom, top, frustnear, frustfar);
}

// next arguments probably contain redundant info, for later...
mt::mat4 RAS_Rasterizer::GetViewMatrix(StereoMode /*stereoMode*/, StereoEye eye, const mt::mat3x4 &camtrans, bool perspective)
{
    if (perspective) {
        static const mt::vec3 unitViewDir = -mt::axisY3;
        static const mt::vec3 unitViewupVec = mt::axisZ3;

        const mt::mat3& camOrientMat3x3 = camtrans.RotationMatrix().Transpose();
        const mt::vec3 viewDir   = camOrientMat3x3 * unitViewDir;
        const mt::vec3 viewupVec = camOrientMat3x3 * unitViewupVec;

        const mt::vec3 eyeline = mt::cross(viewDir, viewupVec);

        mt::mat3x4 trans = camtrans;

        // Simplificação do switch
        float eyeSign = 0.0f;
        if (eye == RAS_STEREO_LEFTEYE) eyeSign = -0.5f;
        else if (eye == RAS_STEREO_RIGHTEYE) eyeSign = 0.5f;

        if (eyeSign != 0.0f) {
            const mt::mat3x4 transform(mt::mat3::Identity(), eyeline * m_eyeseparation * eyeSign);
            trans *= transform;
        }

        return mt::mat4::FromAffineTransform(trans);
    }

    return mt::mat4::FromAffineTransform(camtrans);
}

void RAS_Rasterizer::SetViewMatrix(const mt::mat4& viewmat, bool negscale)
{
	m_viewmatrix = viewmat;
	m_viewinvmatrix = m_viewmatrix.Inverse();
	m_campos = m_viewinvmatrix.TranslationVector3D();
	m_camnegscale = negscale;

	SetMatrixMode(RAS_MODELVIEW);
	LoadMatrix((float *)m_viewmatrix.Data());
}

void RAS_Rasterizer::SetViewMatrix(const mt::mat4& viewmat)
{
	SetViewMatrix(viewmat, false);
}

void RAS_Rasterizer::SetViewMatrix(const mt::mat4 &viewmat, const mt::vec3& scale)
{
	mt::mat4 mat = viewmat;
	for (unsigned short i = 0; i < 3; ++i) {
		// Negate row scaling if the scale is negative.
		if (scale[i] < 0.0f) {
			for (unsigned short j = 0; j < 4; ++j) {
				mat(i, j) *= -1.0f;
			}
		}
	}

	const bool negscale = (scale.x * scale.y * scale.z) < 0.0f;
	SetViewMatrix(mat, negscale);
}

void RAS_Rasterizer::SetViewport(int x, int y, int width, int height)
{
    m_impl->SetViewport(x, y, width, height);
}

void RAS_Rasterizer::GetViewport(int *rect)
{
	m_impl->GetViewport(rect);
}

void RAS_Rasterizer::SetScissor(int x, int y, int width, int height)
{
	m_impl->SetScissor(x, y, width, height);
}

const mt::vec3& RAS_Rasterizer::GetCameraPosition()
{
	return m_campos;
}

bool RAS_Rasterizer::GetCameraOrtho()
{
	return m_camortho;
}

void RAS_Rasterizer::SetCullFace(bool enable)
{
	//if (enable == m_state.cullFace) {
	//	return;
	//}
	m_state.cullFace = enable;

	if (enable) {
		Enable(RAS_CULL_FACE);
	}
	else {
		Disable(RAS_CULL_FACE);
	}
}

void RAS_Rasterizer::EnableClipPlane(unsigned short index, const mt::vec4& plane)
{
	m_impl->EnableClipPlane(index, plane);
}

void RAS_Rasterizer::DisableClipPlane(unsigned short index)
{
	m_impl->DisableClipPlane(index);
}

void RAS_Rasterizer::SetLines(bool enable)
{
	m_impl->SetLines(enable);
}

void RAS_Rasterizer::SetSpecularity(float specX,
                                    float specY,
                                    float specZ,
                                    float specval)
{
	m_impl->SetSpecularity(specX, specY, specZ, specval);
}

void RAS_Rasterizer::SetShinyness(float shiny)
{
	m_impl->SetShinyness(shiny);
}

void RAS_Rasterizer::SetDiffuse(float difX, float difY, float difZ, float diffuse)
{
	m_impl->SetDiffuse(difX, difY, difZ, diffuse);
}

void RAS_Rasterizer::SetEmissive(float eX, float eY, float eZ, float e)
{
	m_impl->SetEmissive(eX, eY, eZ, e);
}

double RAS_Rasterizer::GetTime()
{
	return m_time;
}

unsigned int RAS_Rasterizer::GetFrameCount() const
{
	return m_frameCount;
}

void RAS_Rasterizer::SetPolygonOffset(DrawType drawingMode, float mult, float add)
{
	if (m_state.polyOffset[0] == mult && m_state.polyOffset[1] == add) {
		return;
	}

	m_impl->SetPolygonOffset(mult, add);

	EnableBit mode = RAS_POLYGON_OFFSET_FILL;
	if (drawingMode < RAS_TEXTURED) {
		mode = RAS_POLYGON_OFFSET_LINE;
	}
	if (mult != 0.0f || add != 0.0f) {
		Enable(mode);
	}
	else {
		Disable(mode);
	}

	m_state.polyOffset[0] = mult;
	m_state.polyOffset[1] = add;
}

void RAS_Rasterizer::EnableMotionBlur(float motionblurvalue)
{
	/* don't just set m_motionblur to 1, but check if it is 0 so
	 * we don't reset a motion blur that is already enabled */
	if (m_motionblur == 0) {
		m_motionblur = 1;
	}
	m_motionblurvalue = motionblurvalue;
}

void RAS_Rasterizer::DisableMotionBlur()
{
	m_motionblur = 0;
	m_motionblurvalue = -1.0f;
}

void RAS_Rasterizer::SetMotionBlur(unsigned short state)
{
	m_motionblur = state;
}

void RAS_Rasterizer::SetAlphaBlend(int alphablend)
{
	if (m_state.alphablend == alphablend) {
		return;
	}

	GPU_set_material_alpha_blend(alphablend);
	m_state.alphablend = alphablend;
}

void RAS_Rasterizer::SetFrontFace(bool ccw)
{
	// Invert the front face if the camera has a negative scale or if we force to inverse the front face.
	ccw ^= (m_camnegscale || m_invertFrontFace);

	if (m_state.frontFace == (int)ccw) {
		return;
	}

	m_impl->SetFrontFace(ccw);

	m_state.frontFace = (int)ccw;
}

void RAS_Rasterizer::SetInvertFrontFace(bool invert)
{
	m_invertFrontFace = invert;
}

void RAS_Rasterizer::SetAnisotropicFiltering(short level)
{
	GPU_set_anisotropic(G.main, (float)level);
}

short RAS_Rasterizer::GetAnisotropicFiltering()
{
	return (short)GPU_get_anisotropic();
}

void RAS_Rasterizer::SetMipmapping(MipmapOption val)
{
	bool new_mipmap = (val != RAS_MIPMAP_NONE);
	bool new_linear = (val == RAS_MIPMAP_LINEAR);

	// If the state didn't change, force a global refresh by manually freeing images.
	// This ensures bge.render.setMipmapping acts as a "Hard Reset" for all object-specific filters.
	if (GPU_get_mipmap() == new_mipmap && GPU_get_linear_mipmap() == new_linear) {
		GPU_free_images(G.main);
	}

	switch (val) {
		case RAS_Rasterizer::RAS_MIPMAP_LINEAR:
		{
			GPU_set_linear_mipmap(1);
			GPU_set_mipmap(G.main, 1);
			break;
		}
		case RAS_Rasterizer::RAS_MIPMAP_NEAREST:
		{
			GPU_set_linear_mipmap(0);
			GPU_set_mipmap(G.main, 1);
			break;
		}
		default:
		{
			GPU_set_linear_mipmap(0);
			GPU_set_mipmap(G.main, 0);
		}
	}
}

RAS_Rasterizer::MipmapOption RAS_Rasterizer::GetMipmapping()
{
	if (GPU_get_mipmap()) {
		if (GPU_get_linear_mipmap()) {
			return RAS_Rasterizer::RAS_MIPMAP_LINEAR;
		}
		else {
			return RAS_Rasterizer::RAS_MIPMAP_NEAREST;
		}
	}
	else {
		return RAS_Rasterizer::RAS_MIPMAP_NONE;
	}
}

void RAS_Rasterizer::InitOverrideShadersInterface()
{
	// Find uniform location for FBO shaders.

	// Draw frame buffer shader.
	{
		GPUShader *shader = GPU_shader_get_builtin_shader(GPU_SHADER_DRAW_FRAME_BUFFER);
		if (!GPU_shader_get_interface(shader)) {
			OverrideShaderDrawFrameBufferInterface *interface = (OverrideShaderDrawFrameBufferInterface *)MEM_mallocN(sizeof(OverrideShaderDrawFrameBufferInterface), "OverrideShaderDrawFrameBufferInterface");

			interface->colorTexLoc = GPU_shader_get_uniform(shader, "colortex");

			GPU_shader_set_interface(shader, interface);
		}
	}

	// Stipple stereo shader.
	{
		GPUShader *shader = GPU_shader_get_builtin_shader(GPU_SHADER_STEREO_STIPPLE);
		if (!GPU_shader_get_interface(shader)) {
			OverrideShaderStereoStippleInterface *interface = (OverrideShaderStereoStippleInterface *)MEM_mallocN(sizeof(OverrideShaderStereoStippleInterface), "OverrideShaderStereoStippleInterface");

			interface->leftEyeTexLoc = GPU_shader_get_uniform(shader, "lefteyetex");
			interface->rightEyeTexLoc = GPU_shader_get_uniform(shader, "righteyetex");
			interface->stippleIdLoc = GPU_shader_get_uniform(shader, "stippleid");

			GPU_shader_set_interface(shader, interface);
		}
	}

	// Anaglyph stereo shader.
	{
		GPUShader *shader = GPU_shader_get_builtin_shader(GPU_SHADER_STEREO_ANAGLYPH);
		if (!GPU_shader_get_interface(shader)) {
			OverrideShaderStereoAnaglyph *interface = (OverrideShaderStereoAnaglyph *)MEM_mallocN(sizeof(OverrideShaderStereoAnaglyph), "OverrideShaderStereoAnaglyph");

			interface->leftEyeTexLoc = GPU_shader_get_uniform(shader, "lefteyetex");
			interface->rightEyeTexLoc = GPU_shader_get_uniform(shader, "righteyetex");

			GPU_shader_set_interface(shader, interface);
		}
	}
}

GPUShader *RAS_Rasterizer::GetOverrideGPUShader(OverrideShaderType type)
{
	GPUShader *shader = nullptr;
	GPUMaterial *currentMat = GetCurrentGPUMaterial();
	
	switch (type) {
		case RAS_OVERRIDE_SHADER_NONE:
		{
			break;
		}
		case RAS_OVERRIDE_SHADER_BLACK:
		{
			// Try to get custom shadow shader from current material
			if (currentMat) {
				shader = GPU_shader_get_material_shadow_shader(currentMat, GPU_SHADOW_SHADER_BLACK);
			}
			else {
				shader = GPU_shader_get_builtin_shader(GPU_SHADER_BLACK);
			}
			break;
		}
		case RAS_OVERRIDE_SHADER_BLACK_INSTANCING:
		{
			// Try to get custom shadow shader with instancing from current material
			if (currentMat) {
				shader = GPU_shader_get_material_shadow_shader(currentMat, GPU_SHADOW_SHADER_BLACK_INSTANCING);
			}
			else {
				shader = GPU_shader_get_builtin_shader(GPU_SHADER_BLACK_INSTANCING);
			}
			break;
		}
		case RAS_OVERRIDE_SHADER_SHADOW_VARIANCE:
		{
			// Try to get custom VSM shadow shader from current material
			if (currentMat) {
				shader = GPU_shader_get_material_shadow_shader(currentMat, GPU_SHADOW_SHADER_VSM);
			}
			else {
				shader = GPU_shader_get_builtin_shader(GPU_SHADER_VSM_STORE);
			}
			break;
		}
		case RAS_OVERRIDE_SHADER_SHADOW_VARIANCE_INSTANCING:
		{
			// Try to get custom VSM shadow shader with instancing from current material
			if (currentMat) {
				shader = GPU_shader_get_material_shadow_shader(currentMat, GPU_SHADOW_SHADER_VSM_INSTANCING);
			}
			else {
				shader = GPU_shader_get_builtin_shader(GPU_SHADER_VSM_STORE_INSTANCING);
			}
			break;
		}
	}

	return shader;
}

void RAS_Rasterizer::BindOverrideShaderUniforms()
{
	// Bind TIME uniform for custom shadow shaders
	GPUMaterial *currentMat = GetCurrentGPUMaterial();
	if (currentMat && GPU_material_has_custom_vertex_shader(currentMat)) {
		int *time_loc = GPU_material_get_shadow_time_loc(currentMat);
		if (time_loc && *time_loc >= 0) {
			GPUShader *shader = GetOverrideGPUShader(m_overrideShader);
			if (shader) {
				float time_value = (float)GetTime();
				GPU_shader_uniform_float(shader, *time_loc, time_value);
			}
		}
	}
}

void RAS_Rasterizer::SetOverrideShader(RAS_Rasterizer::OverrideShaderType type)
{
	// Check if we're already using this type
	bool typeChanged = (type != m_overrideShader);
	
	// Always get the shader (might be different per-material even with same type)
	GPUShader *shader = GetOverrideGPUShader(type);
	
	// Cache the last bound shader to avoid redundant binds
	static GPUShader *lastBoundShader = nullptr;
	
	if (shader) {
		// Only bind if the shader actually changed (optimization)
		if (shader != lastBoundShader) {
			GPU_shader_bind(shader);
			lastBoundShader = shader;
		}
		
		// CORREÇÃO: Sempre atualizar TIME, não só quando shader muda
		// Isso garante que animações vertex funcionem corretamente em alpha shadows
		BindOverrideShaderUniforms();
	}
	else {
		if (typeChanged || lastBoundShader != nullptr) {
			GPU_shader_unbind();
			lastBoundShader = nullptr;
		}
	}
	
	m_overrideShader = type;
}

RAS_Rasterizer::OverrideShaderType RAS_Rasterizer::GetOverrideShader()
{
	return m_overrideShader;
}

void RAS_Rasterizer::ActivateOverrideShaderInstancing(RAS_InstancingBuffer *buffer)
{
	GPUShader *shader = GetOverrideGPUShader(m_overrideShader);
	if (shader) {
		GPU_shader_bind_instancing_attrib(shader, (void *)buffer->GetMatrixOffset(), (void *)buffer->GetPositionOffset());
		
		// Bind custom uniforms for every draw call
		BindOverrideShaderUniforms();
	}
}

/**
 * Render Tools
 */

/* ProcessLighting performs lighting on objects. the layer is a bitfield that
 * contains layer information. There are 20 'official' layers in blender. A
 * light is applied on an object only when they are in the same layer. OpenGL
 * has a maximum of 8 lights (simultaneous), so 20 * 8 lights are possible in
 * a scene. */

void RAS_Rasterizer::ProcessLighting(bool uselights, const mt::mat3x4& viewmat)
{
	bool enable = false;
	int layer = -1;

	/* find the layer */
	if (uselights) {
		if (m_clientobject) {
			layer = KX_GameObject::GetClientObject((KX_ClientObjectInfo *)m_clientobject)->GetLayer();
		}
	}

	/* avoid state switching */
	if (m_lastlightlayer == layer && m_lastauxinfo == m_auxilaryClientInfo) {
		return;
	}

	m_lastlightlayer = layer;
	m_lastauxinfo = m_auxilaryClientInfo;

	/* enable/disable lights as needed */
	if (layer >= 0) {
		//enable = ApplyLights(layer, viewmat);
		// taken from blender source, incompatibility between Blender Object / GameObject
		KX_Scene *kxscene = (KX_Scene *)m_auxilaryClientInfo;
		float glviewmat[16];
		unsigned int count;
		std::vector<RAS_OpenGLLight *>::iterator lit = m_lights.begin();

		for (count = 0; count < m_numgllights; count++) {
			m_impl->DisableLight(count);
		}

		viewmat.PackFromAffineTransform(glviewmat);

		PushMatrix();
		LoadMatrix(glviewmat);
		for (lit = m_lights.begin(), count = 0; !(lit == m_lights.end()) && count < m_numgllights; ++lit) {
			RAS_OpenGLLight *light = (*lit);

			if (light->ApplyFixedFunctionLighting(kxscene, layer, count)) {
				count++;
			}
		}
		PopMatrix();

		enable = count > 0;
	}

	if (enable) {
		EnableLights();
	}
	else {
		DisableLights();
	}
}


void RAS_Rasterizer::EnableLights()
{
	if (m_lastlighting == true) {
		return;
	}

	Enable(RAS_Rasterizer::RAS_LIGHTING);
	Enable(RAS_Rasterizer::RAS_COLOR_MATERIAL);

	m_impl->EnableLights();

	m_lastlighting = true;
}

void RAS_Rasterizer::DisableLights()
{
	if (m_lastlighting == false) {
		return;
	}

	Disable(RAS_Rasterizer::RAS_LIGHTING);
	Disable(RAS_Rasterizer::RAS_COLOR_MATERIAL);

	m_lastlighting = false;
}

RAS_ILightObject *RAS_Rasterizer::CreateLight()
{
	return new RAS_OpenGLLight(this);
}

void RAS_Rasterizer::AddLight(RAS_ILightObject *lightobject)
{
	RAS_OpenGLLight *gllight = static_cast<RAS_OpenGLLight *>(lightobject);
	BLI_assert(gllight);
	m_lights.push_back(gllight);
}

void RAS_Rasterizer::RemoveLight(RAS_ILightObject *lightobject)
{
	RAS_OpenGLLight *gllight = static_cast<RAS_OpenGLLight *>(lightobject);
	BLI_assert(gllight);

	CM_ListRemoveIfFound(m_lights, gllight);
}

bool RAS_Rasterizer::RayHit(KX_ClientObjectInfo *client, KX_RayCast *result, RayCastTranform *raytransform)
{
    if (!result->m_hitMesh) {
        return false;
    }

    const RAS_Mesh::PolygonInfo poly = result->m_hitMesh->GetPolygon(result->m_hitPolygon);
    if (!(poly.flags & RAS_Mesh::PolygonInfo::VISIBLE)) {
        return false;
    }

    const mt::mat4& origmat = raytransform->origmat;
    float *mat = raytransform->mat;
    const mt::vec3& scale = raytransform->scale;
    const mt::vec3& point = result->m_hitPoint;
    mt::vec3 resultnormal(result->m_hitNormal);

    mt::vec3 left = origmat.GetColumn(0).xyz();
    mt::vec3 dir = -(mt::cross(left, resultnormal)).SafeNormalized(mt::axisX3);
    left = (mt::cross(dir, resultnormal)).SafeNormalized(mt::axisX3);
    
    left *= scale[0];
    dir *= scale[1];
    resultnormal *= scale[2];

    mat[0] = left[0];    mat[1] = left[1];    mat[2] = left[2];    mat[3] = 0.0f;
    mat[4] = dir[0];     mat[5] = dir[1];     mat[6] = dir[2];     mat[7] = 0.0f;
    mat[8] = resultnormal[0]; mat[9] = resultnormal[1]; mat[10] = resultnormal[2]; mat[11] = 0.0f;
    mat[12] = point[0];  mat[13] = point[1];  mat[14] = point[2];  mat[15] = 1.0f;

    return true;
}


bool RAS_Rasterizer::NeedRayCast(KX_ClientObjectInfo *UNUSED(info), void *UNUSED(data))
{
	return true;
}

void RAS_Rasterizer::GetTransform(const mt::mat4& origmat, int objectdrawmode, float mat[16])
{
    if (objectdrawmode == RAS_IMaterial::RAS_NORMAL) {
        origmat.Pack(mat);
        return;
    }

    mt::vec3 left, up, dir;
    mt::vec3 scale;

    if (ELEM(objectdrawmode, RAS_IMaterial::RAS_HALO, RAS_IMaterial::RAS_BILLBOARD)) {
        if (m_camortho) {
            left = m_viewmatrix.GetColumn(2).xyz().SafeNormalized(mt::axisX3);
        } else {
            mt::vec3 objpos(&origmat[12]);
            const mt::vec3& campos = GetCameraPosition();
            left = (campos - objpos).SafeNormalized(mt::axisX3);
        }

        up = mt::vec3(&origmat[8]).SafeNormalized(mt::axisX3);

        scale = mt::vec3(len_v3(&origmat[0]), len_v3(&origmat[4]), len_v3(&origmat[8]));

        if (objectdrawmode & (RAS_IMaterial::RAS_HALO | RAS_IMaterial::RAS_BILLBOARD)) {
            up = (up - mt::dot(up, left) * left).SafeNormalized(mt::axisX3);
        } else {
            left = (left - mt::dot(up, left) * up).SafeNormalized(mt::axisX3);
        }

        dir = mt::cross(up, left).Normalized();

        left *= scale[0];
        dir *= scale[1];
        up *= scale[2];

        mat[0] = left[0]; mat[1] = left[1]; mat[2] = left[2]; mat[3] = 0.0f;
        mat[4] = dir[0];  mat[5] = dir[1];  mat[6] = dir[2];  mat[7] = 0.0f;
        mat[8] = up[0];   mat[9] = up[1];   mat[10] = up[2];  mat[11] = 0.0f;
        mat[12] = origmat[12]; mat[13] = origmat[13]; mat[14] = origmat[14]; mat[15] = 1.0f;
    }
    else {
        const mt::vec3 frompoint(&origmat[12]);
        KX_GameObject *gameobj = KX_GameObject::GetClientObject((KX_ClientObjectInfo *)m_clientobject);
        mt::vec3 direction = -mt::axisZ3;
        direction.Normalize();
        direction *= 100000.0f;

        const mt::vec3 topoint = frompoint + direction;

        KX_Scene *kxscene = (KX_Scene *)m_auxilaryClientInfo;
        PHY_IPhysicsEnvironment *physics_environment = kxscene->GetPhysicsEnvironment();
        PHY_IPhysicsController *physics_controller = gameobj->GetPhysicsController();

        KX_GameObject *parent = gameobj->GetParent();
        if (!physics_controller && parent) {
            physics_controller = parent->GetPhysicsController();
        }

        RayCastTranform raytransform;
        raytransform.origmat = origmat;
        raytransform.mat = mat;
        raytransform.scale = gameobj->NodeGetWorldScaling();

        KX_RayCast::Callback<RAS_Rasterizer, RayCastTranform> callback(this, physics_controller, &raytransform);
        if (!KX_RayCast::RayTest(physics_environment, frompoint, topoint, callback)) {
            origmat.Pack(mat);
        }
    }
}


void RAS_Rasterizer::FlushDebug(RAS_ICanvas *canvas, RAS_DebugDraw *debugDraw)
{
	m_debugDrawImpl->Flush(this, canvas, debugDraw);
}

void RAS_Rasterizer::DisableForText()
{
	SetAlphaBlend(GPU_BLEND_ALPHA);
	SetLines(false);

	DisableLights();

	m_impl->DisableForText();
}

void RAS_Rasterizer::RenderText3D(int fontid, const std::string& text, int size, int dpi,
                                  const float color[4], const float mat[16], float aspect)
{
	m_impl->RenderText3D(fontid, text, size, dpi, color, mat, aspect);
}

void RAS_Rasterizer::PushMatrix()
{
	m_impl->PushMatrix();
}

void RAS_Rasterizer::PopMatrix()
{
	m_impl->PopMatrix();
}

void RAS_Rasterizer::SetMatrixMode(RAS_Rasterizer::MatrixMode mode)
{
	m_impl->SetMatrixMode(mode);
}

void RAS_Rasterizer::MultMatrix(const float mat[16])
{
	m_impl->MultMatrix(mat);
}

void RAS_Rasterizer::LoadMatrix(const float mat[16])
{
	m_impl->LoadMatrix(mat);
}

void RAS_Rasterizer::LoadIdentity()
{
	m_impl->LoadIdentity();
}

void RAS_Rasterizer::UpdateGlobalDepthTexture(RAS_OffScreen *offScreen, RAS_ICanvas *canvas)
{
	/* In case of multisamples the depth off screen must be blit to be used in shader.
	 * But the original off screen must be kept bound after the blit. */
	if (offScreen->GetSamples()) {
		RAS_OffScreen *dstOffScreen = canvas->GetOffScreen(RAS_OffScreen::RAS_OFFSCREEN_BLIT_DEPTH);
		offScreen->Blit(dstOffScreen, true);
		// Restore original off screen.
		offScreen->Bind();
		offScreen = dstOffScreen;
	}
	GPUTexture* gputex = offScreen->GetDepthTexture();
	GPU_texture_set_global_depth(gputex);
}

void RAS_Rasterizer::ResetGlobalDepthTexture()
{
	GPU_texture_set_global_depth(nullptr);
}

void RAS_Rasterizer::MotionBlur()
{
	m_impl->MotionBlur(m_motionblur, m_motionblurvalue);
}

void RAS_Rasterizer::SetClientObject(void *obj)
{
	if (m_clientobject == obj)
		return;
	m_clientobject = obj;
}

void RAS_Rasterizer::SetAuxilaryClientInfo(void *inf)
{
	m_auxilaryClientInfo = inf;
}

void RAS_Rasterizer::PrintHardwareInfo()
{
	m_impl->PrintHardwareInfo();
}
void RAS_Rasterizer::PrintVRAMUsage()
{
	m_impl->PrintVRAMUsage();
}
