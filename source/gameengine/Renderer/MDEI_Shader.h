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

#include <string>

struct GPUShader;
struct GPUPass;
struct GPUMaterial;
struct Scene;
class  RAS_Rasterizer;

class MDEI_Shader {
public:
	MDEI_Shader();
	~MDEI_Shader();

	/** Build both solid and shadow shader programs from an existing GPUPass.
	 *  Call once per unique material during scene conversion.
	 *  @param pass      GPUPass* from GPU_material_get_pass()
	 *  @param gpuMat    GPUMaterial owning the pass (needed at bind time)
	 *  @param scene     Blender Scene* (for lamp/layer queries)
	 *  @param variance  True if the scene uses Variance Shadow Maps */
	bool CompileFromPass(GPUPass *pass, GPUMaterial *gpuMat,
	                     Scene *scene, bool variance);

	/** Bind the solid shader and upload ALL uniforms:
	 *  textures, lamp dynamics, view/object builtins, custom uniforms, UBO. */
	void BindSolid(RAS_Rasterizer *rasty);

	/** Unbind after solid rendering. */
	void UnbindSolid();

	/** Bind the depth-only shader for shadow pass.
	 *  Uploads minimal uniforms (view matrix, time). */
	void BindShadow(RAS_Rasterizer *rasty);
	void UnbindShadow();

	bool IsReady() const { return m_solidShader != nullptr; }

	/** Fill \p out with the vertex attribute locations for the solid shader.
	 *  Must be called after CompileFromPass() succeeds.
	 *  Returns false if no GPUMaterial is available. */
	bool GetGPUVertexAttribs(GPUVertexAttribs &out) const;

	/* Cached uniform locations (looked up once after compilation) */
	int m_locViewMat;     /* unfviewmat  (solid) */
	int m_locInvViewMat;  /* unfinvviewmat (solid) */
	int m_locTime;        /* unftime (solid) */

	/* Same set for shadow shader */
	int m_shadowLocViewMat;
	int m_shadowLocTime;

private:
	static std::string PatchVertexCode(const char *original);
	bool CompileShadowShader(const char *originalVertCode, bool variance);

	GPUShader   *m_solidShader;
	GPUShader   *m_shadowShader;

	/* Stored at compile time; used every BindSolid() call */
	GPUMaterial *m_gpuMat;
	Scene       *m_scene;
};

#endif /* __MDEI_SHADER_H__ */
