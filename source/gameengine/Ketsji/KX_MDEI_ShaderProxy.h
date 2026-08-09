/*
 * KX_MDEI_ShaderProxy.h — Python-visible wrapper around MDEI_Shader.
 *
 * Exposes the full BL_Shader-equivalent uniform API to Python for objects
 * that use the MDEI fast-path (OB_FAST_RENDER).
 *
 * ── Shader source override ──
 *   setSource(vert, frag)
 *   setSourceList({"vertex":..., "fragment":...})
 *   isValid() -> bool
 *
 * ── Uniform setters ──
 *   setUniform1f/2f/3f/4f(name, ...)
 *   setUniform1i/2i/3i/4i(name, ...)
 *   setUniformfv(name, list)         — 2, 3 or 4 floats
 *   setUniformiv(name, list)         — 2, 3 or 4 ints
 *   setUniformMatrix3(name, list9,  transpose=False)
 *   setUniformMatrix4(name, list16, transpose=False)
 *
 * ── Classic (unit-based) samplers ──
 *   setSampler(name, glTexId, unit)
 *       Bind a 2D texture using a raw GL id and an explicit texture unit.
 *   setSamplerArray(name, [glTexId,...], mipmap=False)
 *       Build a GL_TEXTURE_2D_ARRAY; unit is allocated automatically.
 *
 * ── Bindless samplers (GL_ARB_bindless_texture) ──
 *   setSamplerBindless(name, handle)
 *       Upload a 64-bit bindless handle to the named sampler uniform.
 *       Mirrors BL_Shader.setSampler / KX_BlenderMaterial::ApplyTextures().
 *   setSamplerArrayFromGPUTextures(name, [gpuTexPtr,...], mipmap=False)
 *       Build a GL_TEXTURE_2D_ARRAY from GPUTexture* pointers (passed as
 *       Python ints = ctypes.cast pointer); unit allocated automatically.
 *
 * ── Mipmap control ──
 *   setMipmapping(enabled, glFilterType=-1)
 *       Enable/disable mipmap on all sampler arrays; forces full rebuild.
 *   updateMipmappingFilter(glFilterType, slot=-1)
 *       Re-apply glTexParameteri only — no rebuild.
 */

#ifndef __KX_MDEI_SHADER_PROXY_H__
#define __KX_MDEI_SHADER_PROXY_H__

#ifdef WITH_PYTHON

#include "EXP_Value.h"

class MDEI_Shader;

class KX_MDEI_ShaderProxy : public EXP_Value
{
	Py_Header

public:
	explicit KX_MDEI_ShaderProxy(MDEI_Shader *shader);
	virtual ~KX_MDEI_ShaderProxy();

	virtual std::string GetName();

	void SetShader(MDEI_Shader *shader) { m_shader = shader; }
	MDEI_Shader *GetShader() const      { return m_shader; }

	/* ── Python methods ───────────────────────────────────────────────── */

	EXP_PYMETHOD_DOC(KX_MDEI_ShaderProxy, setSource);
	EXP_PYMETHOD_DOC(KX_MDEI_ShaderProxy, setSourceList);
	EXP_PYMETHOD_DOC(KX_MDEI_ShaderProxy, isValid);

	/* Scalar / vector uniforms */
	EXP_PYMETHOD_DOC(KX_MDEI_ShaderProxy, setUniform1f);
	EXP_PYMETHOD_DOC(KX_MDEI_ShaderProxy, setUniform2f);
	EXP_PYMETHOD_DOC(KX_MDEI_ShaderProxy, setUniform3f);
	EXP_PYMETHOD_DOC(KX_MDEI_ShaderProxy, setUniform4f);
	EXP_PYMETHOD_DOC(KX_MDEI_ShaderProxy, setUniform1i);
	EXP_PYMETHOD_DOC(KX_MDEI_ShaderProxy, setUniform2i);
	EXP_PYMETHOD_DOC(KX_MDEI_ShaderProxy, setUniform3i);
	EXP_PYMETHOD_DOC(KX_MDEI_ShaderProxy, setUniform4i);

	/* List-based uniforms */
	EXP_PYMETHOD_DOC(KX_MDEI_ShaderProxy, setUniformfv);
	EXP_PYMETHOD_DOC(KX_MDEI_ShaderProxy, setUniformiv);

	/* Matrix uniforms */
	EXP_PYMETHOD_DOC(KX_MDEI_ShaderProxy, setUniformMatrix3);
	EXP_PYMETHOD_DOC(KX_MDEI_ShaderProxy, setUniformMatrix4);

	/* ── Classic samplers ────────────────────────────────────────────── */
	EXP_PYMETHOD_DOC(KX_MDEI_ShaderProxy, setSampler);
	EXP_PYMETHOD_DOC(KX_MDEI_ShaderProxy, setSamplerArray);

	/* ── Bindless samplers ───────────────────────────────────────────── */
	EXP_PYMETHOD_DOC(KX_MDEI_ShaderProxy, setSamplerBindless);
	EXP_PYMETHOD_DOC(KX_MDEI_ShaderProxy, setSamplerArrayFromGPUTextures);

	/* ── Mipmap control ──────────────────────────────────────────────── */
	EXP_PYMETHOD_DOC(KX_MDEI_ShaderProxy, setMipmapping);
	EXP_PYMETHOD_DOC(KX_MDEI_ShaderProxy, updateMipmappingFilter);

private:
	MDEI_Shader *m_shader;  /* not owned — owned by MDEI_Renderer */
};

#endif /* WITH_PYTHON */

#endif /* __KX_MDEI_SHADER_PROXY_H__ */
