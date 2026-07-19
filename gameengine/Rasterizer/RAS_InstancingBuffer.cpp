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
 * The Original Code is: all of this file.
 *
 * Contributor(s): Porteries Tristan.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file gameengine/Rasterizer/RAS_InstancingBuffer.cpp
 *  \ingroup bgerast
 */

#include "RAS_InstancingBuffer.h"
#include "RAS_Rasterizer.h"
#include "RAS_MeshUser.h"
#include "RAS_IMaterial.h"

#include "GPU_glew.h"

#include <cstring>

static inline std::size_t AlignUp(std::size_t v, std::size_t a)
{
	return ((v + a - 1) / a) * a;
}

RAS_InstancingBuffer::RAS_InstancingBuffer(Attrib attribs)
	:m_vbo(0),
	m_persistentPtr(nullptr),
	m_segmentSize(0),
	m_ringSize(1),
	m_nextRingIndex(0),
	m_writeRingIndex(0),
	m_fences{nullptr, nullptr, nullptr},
	m_capacity(0),
	m_baseOffset(0),
	m_attribs(attribs)
{
}

RAS_InstancingBuffer::~RAS_InstancingBuffer()
{
	DestroyGLResources();
}

void RAS_InstancingBuffer::Realloc(unsigned int size)
{
	EnsureCapacity(size);
}

void RAS_InstancingBuffer::Bind()
{
	glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
}

void RAS_InstancingBuffer::Unbind()
{
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void RAS_InstancingBuffer::Update(RAS_Rasterizer *rasty, int drawingmode, short matPassIndex, const RAS_MeshSlotList &meshSlots)
{
	const unsigned int count = meshSlots.size();
	if (count == 0) {
		return;
	}

	EnsureCapacity(count);
	BeginWrite();

	std::uint8_t *basePtr = m_persistentPtr;
	if (!basePtr) {
		glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
		glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)m_segmentSize, nullptr, GL_STREAM_DRAW);
		basePtr = (std::uint8_t *)glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
		if (!basePtr) {
			glBindBuffer(GL_ARRAY_BUFFER, 0);
			return;
		}
	}

	const intptr_t buffer = (intptr_t)(basePtr + m_baseOffset);

	for (unsigned int i = 0; i < count; ++i) {
		RAS_MeshSlot *ms = meshSlots[i];
		RAS_MeshUser *meshUser = ms->m_meshUser;

		// Pack matrix and position.
		float (&matrixData)[9] = *(float (*)[9])(buffer + m_matrixOffset + MATRIX_MEMORY_SIZE * i);
		float (&positionData)[3] = *(float (*)[3])(buffer + m_positionOffset + POSITION_MEMORY_SIZE * i);
		if (drawingmode == RAS_IMaterial::RAS_NORMAL) {
			const RAS_MeshUser::PackedTransform& packed = meshUser->GetPackedTransformNormal();
			std::memcpy(matrixData, packed.matrix, sizeof(packed.matrix));
			std::memcpy(positionData, packed.position, sizeof(packed.position));
		}
		else {
			float mat[16];
			rasty->SetClientObject(meshUser->GetClientObject());
			rasty->GetTransform(meshUser->GetMatrix(), drawingmode, mat);

			matrixData[0] = mat[0];
			matrixData[1] = mat[4];
			matrixData[2] = mat[8];
			matrixData[3] = mat[1];
			matrixData[4] = mat[5];
			matrixData[5] = mat[9];
			matrixData[6] = mat[2];
			matrixData[7] = mat[6];
			matrixData[8] = mat[10];

			positionData[0] = mat[12];
			positionData[1] = mat[13];
			positionData[2] = mat[14];
		}

		// Pack color.
		if (m_attribs & COLOR_ATTRIB) {
			float (&colorData)[4] = *(float (*)[4])(buffer + m_colorOffset + COLOR_MEMORY_SIZE * i);
			meshUser->GetColor().Pack(colorData);
		}

		// Pack layer.
		if (m_attribs & LAYER_ATTRIB) {
			unsigned int &layerData = *(unsigned int *)(buffer + m_layerOffset + LAYER_MEMORY_SIZE * i);
			layerData = meshUser->GetLayer();
		}

		// Pack info.
		if (m_attribs & INFO_ATTRIB) {
			float (&infoData)[3] = *(float (*)[3])(buffer + m_infoOffset + INFO_MEMORY_SIZE * i);
			infoData[0] = float(meshUser->GetPassIndex());
			infoData[1] = float(matPassIndex);
			infoData[2] = meshUser->GetRandom();
		}
	}

	if (!m_persistentPtr) {
		glUnmapBuffer(GL_ARRAY_BUFFER);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}
}

void RAS_InstancingBuffer::SubmitFence()
{
	if (!m_persistentPtr || m_ringSize <= 1) {
		return;
	}

	GLsync &fence = *(GLsync *)&m_fences[m_writeRingIndex];
	if (fence) {
		glDeleteSync(fence);
		fence = nullptr;
	}
	fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

void RAS_InstancingBuffer::DestroyGLObjectsOnly()
{
	for (unsigned int i = 0; i < 3; ++i) {
		GLsync &fence = *(GLsync *)&m_fences[i];
		if (fence) {
			glDeleteSync(fence);
			fence = nullptr;
		}
	}

	if (m_vbo) {
		if (m_persistentPtr) {
			glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
			glUnmapBuffer(GL_ARRAY_BUFFER);
			glBindBuffer(GL_ARRAY_BUFFER, 0);
			m_persistentPtr = nullptr;
		}
		glDeleteBuffers(1, &m_vbo);
		m_vbo = 0;
	}
}

void RAS_InstancingBuffer::DestroyGLResources()
{
	DestroyGLObjectsOnly();

	m_capacity = 0;
	m_segmentSize = 0;
	m_baseOffset = 0;
	m_ringSize = 1;
	m_nextRingIndex = 0;
	m_writeRingIndex = 0;
}

void RAS_InstancingBuffer::EnsureCapacity(unsigned int size)
{
	if (size == 0) {
		return;
	}
	if (m_vbo && size <= m_capacity) {
		return;
	}

	unsigned int newCapacity = (m_capacity > 0) ? m_capacity : 1;
	while (newCapacity < size) {
		newCapacity *= 2;
	}

	std::size_t offset = 0;
	m_matrixOffset = (intptr_t)offset;
	offset += (std::size_t)MATRIX_MEMORY_SIZE * newCapacity;

	m_positionOffset = (intptr_t)offset;
	offset += (std::size_t)POSITION_MEMORY_SIZE * newCapacity;

	if (m_attribs & COLOR_ATTRIB) {
		m_colorOffset = (intptr_t)offset;
		offset += (std::size_t)COLOR_MEMORY_SIZE * newCapacity;
	} else {
		m_colorOffset = 0;
	}
	if (m_attribs & LAYER_ATTRIB) {
		m_layerOffset = (intptr_t)offset;
		offset += (std::size_t)LAYER_MEMORY_SIZE * newCapacity;
	} else {
		m_layerOffset = 0;
	}
	if (m_attribs & INFO_ATTRIB) {
		m_infoOffset = (intptr_t)offset;
		offset += (std::size_t)INFO_MEMORY_SIZE * newCapacity;
	} else {
		m_infoOffset = 0;
	}

	const bool canPersistent = (GLEW_ARB_buffer_storage || GLEW_VERSION_4_4);
	DestroyGLObjectsOnly();
	m_capacity = newCapacity;
	m_ringSize = canPersistent ? 3u : 1u;
	m_segmentSize = AlignUp(offset, 256);
	m_nextRingIndex = 0;
	m_writeRingIndex = 0;
	m_baseOffset = 0;

	glGenBuffers(1, &m_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, m_vbo);

	const GLsizeiptr totalSize = (GLsizeiptr)(m_segmentSize * m_ringSize);
	if (canPersistent) {
		const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
		glBufferStorage(GL_ARRAY_BUFFER, totalSize, nullptr, flags);
		m_persistentPtr = (std::uint8_t *)glMapBufferRange(GL_ARRAY_BUFFER, 0, totalSize, flags);
	} else {
		glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)m_segmentSize, nullptr, GL_STREAM_DRAW);
		m_persistentPtr = nullptr;
	}

	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void RAS_InstancingBuffer::BeginWrite()
{
	if (!m_persistentPtr || m_ringSize <= 1) {
		m_baseOffset = 0;
		m_writeRingIndex = 0;
		return;
	}

	m_writeRingIndex = m_nextRingIndex;

	GLsync &fence = *(GLsync *)&m_fences[m_writeRingIndex];
	if (fence) {
		GLenum res = glClientWaitSync(fence, 0, 0);
		while (res == GL_TIMEOUT_EXPIRED) {
			res = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000);
		}
		glDeleteSync(fence);
		fence = nullptr;
	}

	m_baseOffset = (intptr_t)(m_segmentSize * m_writeRingIndex);
	m_nextRingIndex = (m_nextRingIndex + 1) % m_ringSize;
}
