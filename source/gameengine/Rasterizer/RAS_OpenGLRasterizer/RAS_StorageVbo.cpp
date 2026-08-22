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

#include "RAS_StorageVbo.h"
#include "RAS_DisplayArray.h"

#include <cstring>
#include <cstdint>

RAS_StorageVbo::RAS_StorageVbo(RAS_DisplayArray *array)
	:m_array(array),
	m_indices(0),
	m_mode(m_array->GetOpenGLPrimitiveType())
{
	/* DSA: cria os buffers já inicializados, sem bind. */
	glCreateBuffers(1, &m_ibo);
	glCreateBuffers(1, &m_vbo);
}

RAS_StorageVbo::~RAS_StorageVbo()
{
	glDeleteBuffers(1, &m_ibo);
	glDeleteBuffers(1, &m_vbo);
}

void RAS_StorageVbo::BindVertexBuffer()
{
	glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
}

void RAS_StorageVbo::UnbindVertexBuffer()
{
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void RAS_StorageVbo::BindIndexBuffer()
{
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ibo);
}

void RAS_StorageVbo::UnbindIndexBuffer()
{
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

/* DSA: escreve diretamente no buffer sem bind. */
template <class Item>
static void copySubData(GLuint vbo, intptr_t offset, const std::vector<Item>& data)
{
	const GLsizeiptr size = (GLsizeiptr)(sizeof(Item) * data.size());
	glNamedBufferSubData(vbo, (GLintptr)offset, size, data.data());
}

void RAS_StorageVbo::CopyVertexData(const RAS_DisplayArrayLayout& layout, unsigned int modifiedFlag)
{
	const RAS_DisplayArray::Format& format = m_array->GetFormat();
	const RAS_DisplayArray::VertexData& data = m_array->m_vertexData;

	if (modifiedFlag & RAS_DisplayArray::POSITION_MODIFIED) {
		copySubData(m_vbo, layout.position, data.positions);
	}
	if (modifiedFlag & RAS_DisplayArray::NORMAL_MODIFIED) {
		copySubData(m_vbo, layout.normal, data.normals);
	}
	if (modifiedFlag & RAS_DisplayArray::TANGENT_MODIFIED) {
		copySubData(m_vbo, layout.tangent, data.tangents);
	}

	if (modifiedFlag & RAS_DisplayArray::UVS_MODIFIED) {
		for (unsigned short i = 0; i < format.uvSize; ++i) {
			copySubData(m_vbo, layout.uvs[i], data.uvs[i]);
		}
	}

	if (modifiedFlag & RAS_DisplayArray::COLORS_MODIFIED) {
		for (unsigned short i = 0; i < format.colorSize; ++i) {
			copySubData(m_vbo, layout.colors[i], data.colors[i]);
		}
	}
}

void RAS_StorageVbo::UpdateVertexData(unsigned int modifiedFlag)
{
    if (!modifiedFlag) return;

    const auto& layout = m_array->GetLayout();
    const auto& format = m_array->GetFormat();
    const auto& data   = m_array->m_vertexData;

    const size_t vertexCount = data.positions.size();
    if (vertexCount == 0) return;

    /* DSA: cada MapNamedBufferRange opera directamente no buffer sem bind. */

    if (modifiedFlag & RAS_DisplayArray::POSITION_MODIFIED)
    {
        const GLsizeiptr sz = (GLsizeiptr)(vertexCount * sizeof(data.positions[0]));
        uint8_t *ptr = static_cast<uint8_t *>(glMapNamedBufferRange(
            m_vbo, (GLintptr)layout.position, sz,
            GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT));
        if (ptr) {
            memcpy(ptr, data.positions.data(), (size_t)sz);
            glUnmapNamedBuffer(m_vbo);
        }
    }

    if ((modifiedFlag & RAS_DisplayArray::NORMAL_MODIFIED) && !data.normals.empty())
    {
        const GLsizeiptr sz = (GLsizeiptr)(data.normals.size() * sizeof(data.normals[0]));
        uint8_t *ptr = static_cast<uint8_t *>(glMapNamedBufferRange(
            m_vbo, (GLintptr)layout.normal, sz,
            GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT));
        if (ptr) {
            memcpy(ptr, data.normals.data(), (size_t)sz);
            glUnmapNamedBuffer(m_vbo);
        }
    }

    if ((modifiedFlag & RAS_DisplayArray::TANGENT_MODIFIED) && !data.tangents.empty())
    {
        const GLsizeiptr sz = (GLsizeiptr)(data.tangents.size() * sizeof(data.tangents[0]));
        uint8_t *ptr = static_cast<uint8_t *>(glMapNamedBufferRange(
            m_vbo, (GLintptr)layout.tangent, sz,
            GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT));
        if (ptr) {
            memcpy(ptr, data.tangents.data(), (size_t)sz);
            glUnmapNamedBuffer(m_vbo);
        }
    }

    if (modifiedFlag & RAS_DisplayArray::UVS_MODIFIED)
    {
        for (unsigned short i = 0; i < format.uvSize; ++i) {
            if (data.uvs[i].empty()) continue;
            const GLsizeiptr sz = (GLsizeiptr)(data.uvs[i].size() * sizeof(data.uvs[i][0]));
            uint8_t *ptr = static_cast<uint8_t *>(glMapNamedBufferRange(
                m_vbo, (GLintptr)layout.uvs[i], sz,
                GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT));
            if (ptr) {
                memcpy(ptr, data.uvs[i].data(), (size_t)sz);
                glUnmapNamedBuffer(m_vbo);
            }
        }
    }

    if (modifiedFlag & RAS_DisplayArray::COLORS_MODIFIED)
    {
        for (unsigned short i = 0; i < format.colorSize; ++i) {
            if (data.colors[i].empty()) continue;
            const GLsizeiptr sz = (GLsizeiptr)(data.colors[i].size() * sizeof(data.colors[i][0]));
            uint8_t *ptr = static_cast<uint8_t *>(glMapNamedBufferRange(
                m_vbo, (GLintptr)layout.colors[i], sz,
                GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT));
            if (ptr) {
                memcpy(ptr, data.colors[i].data(), (size_t)sz);
                glUnmapNamedBuffer(m_vbo);
            }
        }
    }
}


void RAS_StorageVbo::UpdateSize()
{
    m_indices = m_array->GetPrimitiveIndexCount();
    if (m_indices == 0) {
        printf("WARNING: IBO with zero indices.\n");
    }

    const auto& layout = m_array->GetLayout();
    const auto& data   = m_array->m_vertexData;
    const size_t vertexCount = data.positions.size();
    if (vertexCount == 0) return;

    /* ── VBO — DSA: aloca e mapeia sem bind ─────────────────────────── */
    glNamedBufferData(m_vbo, (GLsizeiptr)layout.size, nullptr, GL_DYNAMIC_DRAW);

    uint8_t *dst = static_cast<uint8_t *>(glMapNamedBufferRange(
        m_vbo, 0, (GLsizeiptr)layout.size,
        GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT));

    if (dst) {
        const auto& fmt = m_array->GetFormat();
        memcpy(dst + layout.position,
               data.positions.data(),
               data.positions.size() * sizeof(data.positions[0]));
        if (!data.normals.empty())
            memcpy(dst + layout.normal,
                   data.normals.data(),
                   data.normals.size() * sizeof(data.normals[0]));
        if (!data.tangents.empty())
            memcpy(dst + layout.tangent,
                   data.tangents.data(),
                   data.tangents.size() * sizeof(data.tangents[0]));
        for (unsigned short i = 0; i < fmt.uvSize; ++i)
            if (!data.uvs[i].empty())
                memcpy(dst + layout.uvs[i],
                       data.uvs[i].data(),
                       data.uvs[i].size() * sizeof(data.uvs[i][0]));
        for (unsigned short i = 0; i < fmt.colorSize; ++i)
            if (!data.colors[i].empty())
                memcpy(dst + layout.colors[i],
                       data.colors[i].data(),
                       data.colors[i].size() * sizeof(data.colors[i][0]));
        glUnmapNamedBuffer(m_vbo);
    }

    /* ── IBO — DSA: upload directo sem bind ─────────────────────────── */
    const GLsizeiptr iboSize = (GLsizeiptr)(m_indices > 0
        ? m_indices * sizeof(GLuint)
        : sizeof(GLuint));
    const void *iboData = (m_indices > 0)
        ? (const void *)m_array->m_primitiveIndices.data()
        : nullptr;
    glNamedBufferData(m_ibo, iboSize, iboData, GL_DYNAMIC_DRAW);
}

unsigned int *RAS_StorageVbo::GetIndexMap()
{
	/* DSA: mapeia o IBO directamente, sem requerer que esteja bound. */
	return static_cast<unsigned int *>(glMapNamedBufferRange(
	    m_ibo, 0, (GLsizeiptr)(m_indices * sizeof(GLuint)),
	    GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT));
}

void RAS_StorageVbo::FlushIndexMap()
{
	glUnmapNamedBuffer(m_ibo);
}

void RAS_StorageVbo::IndexPrimitives()
{
    glDrawElements(m_mode, m_indices, GL_UNSIGNED_INT, nullptr);
}

void RAS_StorageVbo::IndexPrimitivesInstancing(unsigned int numInstances)
{
    glDrawElementsInstanced(m_mode, m_indices, GL_UNSIGNED_INT, nullptr, numInstances);
}

void RAS_StorageVbo::IndexPrimitivesBatching(const std::vector<intptr_t>& indices,
                                             const std::vector<int>& counts)
{
    if(indices.empty() || counts.empty()) return;
    assert(indices.size() == counts.size());

    if (counts.size() == 1) {
        const GLsizei cnt = counts[0];
        const void* ptr = reinterpret_cast<const void*>(indices[0]);
        glDrawElements(m_mode, cnt, GL_UNSIGNED_INT, ptr);
        return;
    }

	const size_t count = indices.size();
	m_cachedMultiDrawPtrs.resize(count);
	for (size_t i = 0; i < count; ++i) {
		m_cachedMultiDrawPtrs[i] = reinterpret_cast<const void *>(indices[i]);
	}

    glMultiDrawElements(m_mode,
                        counts.data(),
                        GL_UNSIGNED_INT,
                        m_cachedMultiDrawPtrs.data(),
                        static_cast<GLsizei>(counts.size()));
}
