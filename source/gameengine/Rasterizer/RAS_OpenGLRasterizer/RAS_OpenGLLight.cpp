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
 * Contributor(s): Mitchell Stokes
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#include "GPU_glew.h"

#include <stdio.h>

#include "RAS_OpenGLLight.h"
#include "RAS_Rasterizer.h"
#include "RAS_ICanvas.h"

#include "KX_Camera.h"
#include "KX_LightObject.h"
#include "KX_Scene.h"

#include "DNA_lamp_types.h"
#include "DNA_scene_types.h"
#include <sstream>
#include <string>
#include "GPU_material.h"
#include <fstream>
#include <string>

RAS_OpenGLLight::RAS_OpenGLLight(RAS_Rasterizer *ras)
	:m_rasterizer(ras)
{
}

RAS_OpenGLLight::~RAS_OpenGLLight()
{
	GPULamp *lamp;
	KX_LightObject *kxlight = (KX_LightObject *)m_light;
	Lamp *la = (Lamp *)kxlight->GetBlenderObject()->data;

	if ((lamp = GetGPULamp())) {
		float obmat[4][4] = {{0}};
		GPU_lamp_update(lamp, 0, 0, obmat);
		GPU_lamp_update_distance(lamp, la->dist, la->att1, la->att2, la->coeff_const, la->coeff_lin, la->coeff_quad);
		GPU_lamp_update_spot(lamp, la->spotsize, la->spotblend);
	}
}

bool RAS_OpenGLLight::ApplyFixedFunctionLighting(KX_Scene * /*kxscene*/, int /*oblayer*/, int /*slot*/)
{
	// FFP GL_LIGHT* slots removed — light data is passed via shader UBO/SSBO.
	return false;
}

GPULamp *RAS_OpenGLLight::GetGPULamp()
{
	KX_LightObject *kxlight = (KX_LightObject *)m_light;

	KX_GameObject *groupObj = kxlight->GetDupliGroupObject();
	Object *blenderGroup = groupObj ? groupObj->GetBlenderObject() : nullptr;
	return GPU_lamp_from_blender(kxlight->GetScene()->GetBlenderScene(), kxlight->GetBlenderObject(), blenderGroup);
}

bool RAS_OpenGLLight::HasShadowBuffer()
{
	GPULamp *lamp;

	if ((lamp = GetGPULamp())) {
		return GPU_lamp_has_shadow_buffer(lamp);
	}
	else {
		return false;
	}
}


bool RAS_OpenGLLight::NeedShadowUpdate()
{
	if (!m_staticShadow) {
		return true;
	}
	return m_requestShadowUpdate;
}

int RAS_OpenGLLight::GetShadowBindCode()
{
	GPULamp *lamp;

	if ((lamp = GetGPULamp())) {
		return GPU_lamp_shadow_bind_code(lamp);
	}
	return -1;
}

mt::mat4 RAS_OpenGLLight::GetViewMat()
{
	GPULamp *lamp = GetGPULamp();
	if (lamp) {
		return mt::mat4(GPU_lamp_get_viewmat(lamp));
	}
	return mt::mat4::Identity();
}

mt::mat4 RAS_OpenGLLight::GetWinMat()
{
	GPULamp *lamp = GetGPULamp();
	if (lamp) {
		return mt::mat4(GPU_lamp_get_winmat(lamp));
	}
	return mt::mat4::Identity();
}

mt::mat4 RAS_OpenGLLight::GetShadowMatrix()
{
	GPULamp *lamp;

	if ((lamp = GetGPULamp())) {
		return mt::mat4(GPU_lamp_dynpersmat(lamp));
	}

	return mt::mat4::Identity();
}

int RAS_OpenGLLight::GetShadowLayer()
{
	GPULamp *lamp;

	if ((lamp = GetGPULamp())) {
		return GPU_lamp_shadow_layer(lamp);
	}
	else {
		return 0;
	}
}

void RAS_OpenGLLight::BindShadowBuffer(RAS_ICanvas *canvas, KX_Camera *cam, mt::mat3x4& camtrans)
{
	GPULamp *lamp;
	float viewmat[4][4], winmat[4][4];
	int winsize;

	/* bind framebuffer */
	lamp = GetGPULamp();
	GPU_lamp_shadow_buffer_bind(lamp, viewmat, &winsize, winmat);

	if (GPU_lamp_shadow_buffer_type(lamp) == LA_SHADMAP_VARIANCE) {
		m_rasterizer->SetShadowMode(RAS_Rasterizer::RAS_SHADOW_VARIANCE);
	}
	else {
		m_rasterizer->SetShadowMode(RAS_Rasterizer::RAS_SHADOW_SIMPLE);
	}

	/* GPU_lamp_shadow_buffer_bind() changes the viewport, so update the canvas */
	canvas->UpdateViewPort(0, 0, winsize, winsize);

	/* setup camera transformation */
	mt::mat4 modelviewmat((float *)viewmat);
	mt::mat4 projectionmat((float *)winmat);

	camtrans = mt::mat3x4((float *)viewmat).Inverse();

	cam->SetModelviewMatrix(modelviewmat, RAS_Rasterizer::RAS_STEREO_LEFTEYE);
	cam->SetProjectionMatrix(projectionmat, RAS_Rasterizer::RAS_STEREO_LEFTEYE);

	cam->NodeSetLocalPosition(camtrans.TranslationVector3D());
	cam->NodeSetLocalOrientation(camtrans.RotationMatrix());
	cam->NodeUpdate();

	/* setup rasterizer transformations */
	m_rasterizer->SetProjectionMatrix(projectionmat);
	m_rasterizer->SetViewMatrix(modelviewmat);
}

void RAS_OpenGLLight::UnbindShadowBuffer()
{
	GPULamp *lamp = GetGPULamp();
	GPU_lamp_shadow_buffer_unbind(lamp);

	m_rasterizer->SetShadowMode(RAS_Rasterizer::RAS_SHADOW_NONE);

	m_requestShadowUpdate = false;
}

Image *RAS_OpenGLLight::GetTextureImage(short texslot)
{
	KX_LightObject *kxlight = (KX_LightObject *)m_light;
	Lamp *la = (Lamp *)kxlight->GetBlenderObject()->data;

	if (texslot >= MAX_MTEX || texslot < 0) {
		printf("KX_LightObject::GetTextureImage(): texslot exceeds slot bounds (0-%d)\n", MAX_MTEX - 1);
		return nullptr;
	}

	if (la->mtex[texslot]) {
		return la->mtex[texslot]->tex->ima;
	}

	return nullptr;
}

void RAS_OpenGLLight::Update(const mt::mat3x4& trans, bool hide)
{
	GPULamp *lamp = GetGPULamp();

	if (lamp) {
		float obmat[4][4];
		trans.PackFromAffineTransform(obmat);

		GPU_lamp_update(lamp, m_layer, hide, obmat);
		GPU_lamp_update_colors(lamp, m_color[0], m_color[1],
		                       m_color[2], m_energy);
		GPU_lamp_update_distance(lamp, m_distance, m_att1, m_att2, m_coeff_const, m_coeff_lin, m_coeff_quad);
		GPU_lamp_update_spot(lamp, m_spotsize, m_spotblend);
	}
}
