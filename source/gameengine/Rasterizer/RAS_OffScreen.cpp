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
 * Contributor(s): Tristan Porteries.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Rasterizer/RAS_OffScreen.cpp
 *  \ingroup bgerast
 */

#include "RAS_OffScreen.h"

#include "GPU_framebuffer.h"

#include "BLI_utildefines.h"


inline void RAS_OffScreen::FreeSlot(Slot& slot, bool multisample)
{
	if (multisample) {
		if (slot.m_rb) GPU_renderbuffer_free(slot.m_rb);
	}
	else {
		if (slot.m_tex) GPU_texture_free(slot.m_tex);
	}
}

RAS_OffScreen *RAS_OffScreen::lastOffScreen = nullptr;
GPUTexture *g_DebugDepthTex = nullptr;

RAS_OffScreen::Slot::Slot()
	: m_rb(nullptr), m_tex(nullptr)
{
}

RAS_OffScreen::RAS_OffScreen(unsigned int width, unsigned int height, unsigned short samples, 
		const AttachmentList& attachments, Type type)
	: m_width(width),
	m_height(height),
	m_samples(samples),
	m_numColorSlots(attachments.size()),
	m_frameBuffer(GPU_framebuffer_create()),
	m_type(type)
{
	bool error = false;

	static const GPUHDRType hdrEnums[] = {
		GPU_HDR_NONE,
		GPU_HDR_HALF_FLOAT,
		GPU_HDR_FULL_FLOAT
	};

	for (unsigned short i = 0; i < m_numColorSlots; ++i) {
		const Attachment& attach = attachments[i];
		GPUHDRType hdr = hdrEnums[attach.hdr];

		if (m_samples > 0) {
			GPURenderBuffer *rb = GPU_renderbuffer_create(m_width, m_height, m_samples, hdr, GPU_RENDERBUFFER_COLOR, nullptr);
			if (!GPU_framebuffer_renderbuffer_attach(m_frameBuffer, rb, i, nullptr)) {
				GPU_renderbuffer_free(rb);
				error = true;
			}
			m_colorSlots[i].m_rb = rb;
		}
		else {
			GPUTexture *tex = GPU_texture_create_2D(m_width, m_height, nullptr, hdr, nullptr);
			if (!GPU_framebuffer_texture_attach(m_frameBuffer, tex, i, nullptr)) {
				GPU_texture_free(tex);
				error = true;
			}
			m_colorSlots[i].m_tex = tex;
		}
	}

	if (m_type == RAS_OffScreen::Type::RAS_OFFSCREEN_GBUFFER) {
		const unsigned short gbufferStart = m_numColorSlots;
		const unsigned short gbufferCount = 4;
		const GPUHDRType gbufferFormats[gbufferCount] = {
			GPU_HDR_NONE,        // Albedo (8-bit)
			GPU_HDR_HALF_FLOAT,  // Specular (16F)
			GPU_HDR_NONE,        // Metal (8-bit)
			GPU_HDR_NONE         // Emit (8-bit)
		};
		for (unsigned short i = 0; i < gbufferCount; ++i) {
			const GPUHDRType hdr = gbufferFormats[i];
			const unsigned short slot = gbufferStart + i;

			if (m_samples > 0) {
				GPURenderBuffer *rb = GPU_renderbuffer_create(m_width, m_height, m_samples, hdr, GPU_RENDERBUFFER_COLOR, nullptr);
				if (!GPU_framebuffer_renderbuffer_attach(m_frameBuffer, rb, slot, nullptr)) {
					GPU_renderbuffer_free(rb);
					error = true;
				}
				m_colorSlots[slot].m_rb = rb;
			}
			else {
				GPUTexture *tex = GPU_texture_create_2D(m_width, m_height, nullptr, hdr, nullptr);
				if (!GPU_framebuffer_texture_attach(m_frameBuffer, tex, slot, nullptr)) {
					GPU_texture_free(tex);
					error = true;
				}
				m_colorSlots[slot].m_tex = tex;
			}
		}

		m_numColorSlots += gbufferCount;
	}

	if (m_samples > 0) {
		GPURenderBuffer *rb = GPU_renderbuffer_create(m_width, m_height, m_samples, GPU_HDR_NONE, GPU_RENDERBUFFER_DEPTH, nullptr);
		if (!GPU_framebuffer_renderbuffer_attach(m_frameBuffer, rb, 0, nullptr)) {
			GPU_renderbuffer_free(rb);
			error = true;
		}
		m_depthSlot.m_rb = rb;
	}
	else {
		GPUTexture *tex = GPU_texture_create_depth(m_width, m_height, false, nullptr);
		if (!GPU_framebuffer_texture_attach(m_frameBuffer, tex, -1, nullptr)) {
			GPU_texture_free(tex);
			error = true;
		}
		m_depthSlot.m_tex = tex;

		if (!g_DebugDepthTex) {
			g_DebugDepthTex = tex;
		}
	}

	if (error) {
		GPU_framebuffer_free(m_frameBuffer);
		m_frameBuffer = nullptr;
	}
}

RAS_OffScreen::~RAS_OffScreen()
{
	if (!GetValid()) return;

	GPU_framebuffer_free(m_frameBuffer);

	for (unsigned short i = 0; i < m_numColorSlots; ++i) {
		FreeSlot(m_colorSlots[i], m_samples > 0);
	}
	FreeSlot(m_depthSlot, m_samples > 0);
}

bool RAS_OffScreen::GetValid() const
{
	return (m_frameBuffer != nullptr);
}

void RAS_OffScreen::Bind()
{
	if (lastOffScreen == this) return;

	GPU_framebuffer_bind_all_attachments(m_frameBuffer, m_numColorSlots);
	lastOffScreen = this;
}

RAS_OffScreen *RAS_OffScreen::Blit(RAS_OffScreen *dstOffScreen, bool depth)
{
	GPU_framebuffer_blit(m_frameBuffer, dstOffScreen->m_frameBuffer, m_width, m_height, m_numColorSlots, depth);
	return dstOffScreen;
}


void RAS_OffScreen::BindColorTexture(unsigned short slot, unsigned short unit)
{
	GPU_texture_bind(m_colorSlots[slot].m_tex, unit);
}

void RAS_OffScreen::BindDepthTexture(unsigned short unit)
{
	GPU_texture_bind(m_depthSlot.m_tex, unit);
}

void RAS_OffScreen::UnbindColorTexture(unsigned short slot)
{
	GPU_texture_unbind(m_colorSlots[slot].m_tex);
}

void RAS_OffScreen::UnbindDepthTexture()
{
	GPU_texture_unbind(m_depthSlot.m_tex);
}

void RAS_OffScreen::MipmapTextures()
{
	for (unsigned short i = 0; i < m_numColorSlots; ++i) {
		GPUTexture *tex = m_colorSlots[i].m_tex;
		if (tex) {
			GPU_texture_filter_mode(tex, false, true, true);
			GPU_texture_generate_mipmap(tex);
		}
	}
}

void RAS_OffScreen::UnmipmapTextures()
{
	for (unsigned short i = 0; i < m_numColorSlots; ++i) {
		GPUTexture *tex = m_colorSlots[i].m_tex;
		if (tex) {
			GPU_texture_filter_mode(tex, false, true, false);
		}
	}
}

int RAS_OffScreen::GetColorBindCode() const
{
	return GPU_texture_opengl_bindcode(m_colorSlots[0].m_tex);
}

unsigned short RAS_OffScreen::GetSamples() const
{
	return m_samples;
}

unsigned int RAS_OffScreen::GetWidth() const
{
	return m_width;
}

unsigned int RAS_OffScreen::GetHeight() const
{
	return m_height;
}

RAS_OffScreen::Type RAS_OffScreen::GetType() const
{
	return m_type;
}

unsigned short RAS_OffScreen::GetNumColorSlot() const
{
	return m_numColorSlots;
}

GPUTexture *RAS_OffScreen::GetDepthTexture()
{
	return m_depthSlot.m_tex;
}

RAS_OffScreen *RAS_OffScreen::GetLastOffScreen()
{
	return lastOffScreen;
}

void RAS_OffScreen::RestoreScreen()
{
	GPU_framebuffer_restore();
	lastOffScreen = nullptr;
}

RAS_OffScreen::Type RAS_OffScreen::NextFilterOffScreen(Type index)
{
	switch (index) {
		case RAS_OFFSCREEN_FILTER0:
		{
			return RAS_OFFSCREEN_FILTER1;
		}
		case RAS_OFFSCREEN_FILTER1:
		// Passing a non-filter frame buffer is allowed.
		default:
		{
			return RAS_OFFSCREEN_FILTER0;
		}
	}
}

RAS_OffScreen::Type RAS_OffScreen::NextRenderOffScreen(Type index)
{
	switch (index) {
		case RAS_OFFSCREEN_EYE_LEFT0:
		{
			return RAS_OFFSCREEN_EYE_LEFT1;
		}
		case RAS_OFFSCREEN_EYE_LEFT1:
		{
			return RAS_OFFSCREEN_EYE_LEFT0;
		}
		case RAS_OFFSCREEN_EYE_RIGHT0:
		{
			return RAS_OFFSCREEN_EYE_RIGHT1;
		}
		case RAS_OFFSCREEN_EYE_RIGHT1:
		{
			return RAS_OFFSCREEN_EYE_RIGHT0;
		}
		// Passing a non-eye frame buffer is disallowed.
		default:
		{
			BLI_assert(false);
			return RAS_OFFSCREEN_EYE_LEFT0;
		}
	}
}
