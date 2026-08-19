/*
 * MDEI_Shader.h — Captures vertexcode + fragmentcode from GPUPass,
 * patches the vertex shader for SSBO instancing, and compiles two
 * GPU programs: one for solid rendering, one for shadow depth.
 *
 * BindSolid() calls GPU_material_bind_to_shader() which mirrors the full
 * GPU_material_bind pipeline (textures, lamps, view builtins, custom
 * uniforms, UBO lighting) but writes into our external m_solidShader.
 */

#ifndef __MDEI_SHADER_H__
#define __MDEI_SHADER_H__

#include "GPU_glew.h"
#include "GPU_shader.h"   /* GPUVertexAttribs */
#include "GPU_texture.h"  /* GPUTexture */

#include <string>
#include <vector>

struct GPUShader;
struct GPUPass;
struct GPUMaterial;
struct GPUTexture;
struct Scene;
class  RAS_Rasterizer;
class  RAS_IMaterial;   /* for slot-based texture resolution */

class MDEI_Shader {
public:
	MDEI_Shader();
	~MDEI_Shader();

	/** Build both solid and shadow shader programs from an existing GPUPass. */
	bool CompileFromPass(GPUPass *pass, GPUMaterial *gpuMat,
	                     Scene *scene, bool variance);

	/** Replace solid shader with custom GLSL source.
	 *  The SSBO instancing block is injected automatically. */
	void SetVertexSource(const std::string &vertSrc);
	void SetFragmentSource(const std::string &fragSrc);

	/** Bind the solid shader + upload all uniforms. */
	void BindSolid(RAS_Rasterizer *rasty);
	void UnbindSolid();

	/** Shadow pass bind (depth only). */
	void BindShadow(RAS_Rasterizer *rasty);
	void UnbindShadow();

	bool IsReady() const { return m_solidShader != nullptr; }
	/** Retorna o programa GL do solid shader (para debug). */
	GPUShader *GetSolidShader() const { return m_solidShader; }

	/** Cull face: true = GL_CULL_FACE ativado para este material (padrão true). */
	void SetCullFace(bool cull) { m_cullFace = cull; }
	bool GetCullFace() const    { return m_cullFace; }

	bool GetGPUVertexAttribs(GPUVertexAttribs &out) const;

	/* ── Scalar / vector float uniforms ────────────────────────────────── */
	void SetUniform1f(const char *name, float v);
	void SetUniform2f(const char *name, float x, float y);
	void SetUniform3f(const char *name, float x, float y, float z);
	void SetUniform4f(const char *name, float x, float y, float z, float w);

	/* ── Scalar / vector int uniforms ───────────────────────────────────── */
	void SetUniform1i(const char *name, int v);
	void SetUniform2i(const char *name, int x, int y);
	void SetUniform3i(const char *name, int x, int y, int z);
	void SetUniform4i(const char *name, int x, int y, int z, int w);

	/* ── List-based uniforms ─────────────────────────────────────────────── */
	void SetUniformfv(const char *name, const float *v, int count);
	void SetUniformiv(const char *name, const int   *v, int count);

	/* ── Matrix uniforms ─────────────────────────────────────────────────── */
	void SetUniformMatrix3(const char *name, const float m9[9],  bool transpose);
	void SetUniformMatrix4(const char *name, const float m16[16], bool transpose);

	/* ── Material reference (for slot-based texture resolution) ─────────── */
	/** Store a non-owning pointer to the Blender material so that
	 *  SetSamplerArrayFromSlots can resolve slot indices (0-7) to GL texture
	 *  ids — exactly the same way BL_Shader::ApplyTextures does. */
	void SetMaterial(RAS_IMaterial *mat) { m_material = mat; }
	RAS_IMaterial *GetMaterial() const   { return m_material; }

	/* ── Classic (unit-based) samplers ──────────────────────────────────── */
	/** Bind a single 2D texture to a named sampler uniform.
	 *  glTexId: raw GL texture id.  unit: GL_TEXTURE0 + unit. */
	void SetSampler(const char *name, GLuint glTexId, int unit);

	/** Build a GL_TEXTURE_2D_ARRAY from plain 2D GL texture ids and bind it.
	 *  Unit is allocated automatically (first free slot >= 8). */
	void SetSamplerArray(const char *name, const GLuint *ids, int count, bool mipmap);

	/** Build a GL_TEXTURE_2D_ARRAY from Blender material slot indices (0-7).
	 *  Resolves each slot via m_material->GetTexture(slot)->GetGPUTexture()
	 *  and GPU_texture_opengl_bindcode() — identical to BL_Shader::ApplyTextures.
	 *  Requires SetMaterial() to have been called first.
	 *  Falls back silently if a slot is not ready (marks dirty for next frame). */
	void SetSamplerArrayFromSlots(const char *name, const int *slots, int count, bool mipmap);

	/* ── Bindless samplers (GL_ARB_bindless_texture) ─────────────────────── */
	/** Upload a pre-made bindless handle directly. */
	void SetSamplerBindless(const char *name, GLuint64 handle);

	/** Create a bindless handle from a plain GL texture id.
	 *  Makes the texture resident and uploads the handle as a uvec2 uniform.
	 *  mipmap: generate mipmaps before making resident. */
	void SetSamplerBindlessFromId(const char *name, GLuint glTexId, bool mipmap);

	/** Build array from GPUTexture* pointers and bind bindlessly. */
	void SetSamplerArrayFromGPUTextures(const char *name,
	                                    GPUTexture *const *textures,
	                                    int count, bool mipmap);

	/** Build a GL_TEXTURE_2D_ARRAY from plain 2D GL ids and expose via
	 *  a bindless handle (uvec2 uniform).
	 *  All source textures must share the same resolution.
	 *  mipmap: generate mipmaps before making resident. */
	void SetSamplerArrayBindless(const char *name, const GLuint *ids, int count, bool mipmap);

	/** Like SetSamplerArrayBindless but resolves Blender slot indices via m_material.
	 *  Requires SetMaterial() to have been called first. */
	void SetSamplerArrayBindlessFromSlots(const char *name, const int *slots, int count, bool mipmap);

	/* ── Mipmap control ──────────────────────────────────────────────────── */
	void SetMipmapping(bool enabled, int glFilterType);
	void UpdateMipmappingFilter(int glFilterType, int slot);

	/** Força a recriação de todos os sampler arrays preservando o estado
	 *  mipmap atual de cada entry.  Usado por SetAnisotropicFiltering para
	 *  que o novo GL_TEXTURE_MAX_ANISOTROPY_EXT seja aplicado. */
	void RebuildAllSamplerArrays();

	/* Cached uniform locations (looked up once after compilation) */
	int m_locViewMat;
	int m_locInvViewMat;
	int m_locTime;
	int m_shadowLocViewMat;
	int m_shadowLocTime;

private:
	static std::string PatchVertexCode(const char *original);
	bool CompileShadowShader(const char *originalVertCode, bool variance);
	void RebuildSolidFromSource();

	/* ── Sampler array entry ─────────────────────────────────────────────── */
	struct SamplerArrayEntry {
		std::string name;
		GLuint      arrayTex      = 0;  /* GL_TEXTURE_2D_ARRAY handle          */
		GLuint64    bindlessHandle = 0; /* bindless handle for arrayTex (0=none)*/
		int         unit          = -1; /* texture unit (classic path only)     */
		bool        mipmap        = false;
		bool        dirty         = false; /* needs rebuild on next BindSolid   */
		/* Source: raw GL ids */
		std::vector<GLuint>      srcIds;
		bool                     fromGpu   = false;
		/* Source: GPUTexture* pointers */
		std::vector<GPUTexture*> srcGpu;
		/* Source: Blender material slot indices */
		std::vector<int>         srcSlots;
		bool                     fromSlots = false;
		/* Last value actually sent to the shader — avoids redundant GL calls  */
		GLuint64    boundHandle   = 0; /* last bindless handle pushed           */
		GLuint      boundTex      = 0; /* last arrayTex bound (classic path)    */
	};
	void BindSamplerArrays();
	void RebuildSamplerArray(SamplerArrayEntry &e);
	int  AllocUnit();

	/* ── Bindless residency tracking ─────────────────────────────────────── */
	/* Handles created by SetSamplerBindlessFromId / SetSamplerArrayBindless.
	 * Stored so the destructor can call glMakeTextureHandleNonResidentARB. */
	struct BindlessEntry {
		std::string name;
		GLuint64    handle;  /* resident handle */
		GLuint      ownedTex; /* GL_TEXTURE_2D_ARRAY we created (0 = not owned) */
	};
	std::vector<BindlessEntry> m_bindlessHandles;

	GPUShader   *m_solidShader;
	GPUShader   *m_shadowShader;
	GPUMaterial *m_gpuMat;
	Scene       *m_scene;
	RAS_IMaterial *m_material;  /* non-owning; for slot → GL id resolution */

	bool         m_cullFace;    /* true = GL_CULL_FACE on para este material */

	/* Custom source override (set by SetVertexSource / SetFragmentSource) */
	std::string m_customVertSrc;
	std::string m_customFragSrc;

	/* Debug: conta quantos BindSolid ocorreram após o último Rebuild.
	 * Zerado em RebuildSolidFromSource. Limita o debug a 3 prints. */
	int m_dbgBindCount;

	/* Per-frame uniform queue: enfileirada por SetUniform*, re-aplicada em
	 * BindSolid() a cada frame e após RebuildSolidFromSource().
	 * Declarada public para permitir acesso às funções helper estáticas
	 * em MDEI_Shader.cpp (mdei_upsert / mdei_apply_entry). */
public:
	struct UniformEntry {
		enum Type { F1,F2,F3,F4, I1,I2,I3,I4, FV,IV, MAT3,MAT4, SAMPLER, BINDLESS } type;
		std::string name;
		float fv[16];
		int   iv[4];
		GLuint      glTex;    /* SAMPLER: raw GL id  */
		int         unit;     /* SAMPLER: unit index */
		GLuint64    handle;   /* BINDLESS handle     */
		bool        transpose;
		int         count;
		UniformEntry() : type(F1), glTex(0), unit(0), handle(0), transpose(false), count(0)
		{
			for (int i = 0; i < 16; ++i) fv[i] = 0.0f;
			for (int i = 0; i < 4;  ++i) iv[i] = 0;
		}
	};
	std::vector<UniformEntry>      m_uniformQueue;
private:
	std::vector<SamplerArrayEntry> m_samplerArrays;
};

#endif /* __MDEI_SHADER_H__ */
