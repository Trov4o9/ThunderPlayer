/*
 * MDEI_Shader.cpp
 */

#include "MDEI_Shader.h"

#include "GPU_material.h"
#include "GPU_shader.h"

#include "RAS_Rasterizer.h"

#include "DNA_scene_types.h"

#include <cstring>
#include <cstdio>

/* ─────────────────────────────────────────────────────────────
 * SSBO block injected before void main() in every vert shader.
 * The struct layout MUST match MDEI_Instance exactly (80 bytes).
 *
 * gl_BaseInstance requires #version 460.  Instead we use
 * GL_ARB_shader_draw_parameters (available since GL 4.0 drivers)
 * which provides gl_BaseInstanceARB under any #version >= 130.
 * The extension is enabled via the 'defines' parameter of
 * GPU_shader_create_ex — placed after #version but before any code.
 * ──────────────────────────────────────────────────────────── */

/* Passed as 'defines' to GPU_shader_create_ex (source slot 4, after #version). */
static const char *MDEI_VERT_DEFINES =
	"#extension GL_ARB_shader_draw_parameters : require\n";

/* Injected just before void main().
 * Notes:
 *   • varinstcolor is guarded with #ifndef USE_INSTANCING because the
 *     gpu_shader_vertex.glsl template already declares it inside the
 *     USE_INSTANCING block; without the guard we'd get a duplicate.
 *   • SSBO binding point 0 is used exclusively by MDEI.
 */
static const char *SSBO_INJECTION = R"GLSL(
/* ── MDEI SSBO Patch ─────────────────────────────────── */
struct MDEI_Instance {
    mat4 modelMatrix;  /* 64 bytes */
    vec4 color;        /* 16 bytes */
};
layout(std430, binding = 0) readonly buffer MDEI_InstanceBuffer {
    MDEI_Instance mdei_instances[];
};
#ifndef USE_INSTANCING
/* Declare varinstcolor when the template has not already declared it */
out vec4 varinstcolor;
#endif
/* ── End MDEI SSBO Patch ─────────────────────────────── */
)GLSL";

/* ── Line patterns to search and replace inside void main() ────────────
 * The codegen produces one of two forms depending on whether a custom
 * vertex shader is present:
 *   Form A (no custom vert): "vec4 co = gl_ModelViewMatrix * position;"
 *   Form B (custom vert):    "vec4 co = gl_ModelViewMatrix * vec4(VERTEX, 1.0);"
 * We try Form A first, then fall back to Form B.
 * ---------------------------------------------------------------------- */

/* Form A — standard template (no custom vertex code) */
static const char *ORIG_CO_LINE_A =
	"vec4 co = gl_ModelViewMatrix * position;";
static const char *PATCHED_CO_LINES_A =
	"MDEI_Instance _inst = mdei_instances[gl_BaseInstanceARB + gl_InstanceID];\n"
	"\tvarinstcolor = _inst.color;\n"
	"\tposition     = _inst.modelMatrix * position;\n"
	"\tnormal        = mat3(transpose(inverse(mat3(_inst.modelMatrix)))) * normal;\n"
	"\tvec4 co = gl_ModelViewMatrix * position;";

/* Form B — custom vertex code path (VERTEX/NORMAL aliases) */
static const char *ORIG_CO_LINE_B =
	"vec4 co = gl_ModelViewMatrix * vec4(VERTEX, 1.0);";
static const char *PATCHED_CO_LINES_B =
	"MDEI_Instance _inst = mdei_instances[gl_BaseInstanceARB + gl_InstanceID];\n"
	"\tvarinstcolor = _inst.color;\n"
	"\tVERTEX = vec3(_inst.modelMatrix * vec4(VERTEX, 1.0));\n"
	"\tNORMAL = mat3(transpose(inverse(mat3(_inst.modelMatrix)))) * NORMAL;\n"
	"\tvec4 co = gl_ModelViewMatrix * vec4(VERTEX, 1.0);";

/* ─────────────────────────────────────────────────────────────────────── */

std::string MDEI_Shader::PatchVertexCode(const char *original)
{
	std::string code(original);

	/* 1. Inject SSBO declaration before void main(). */
	size_t mainPos = code.find("void main()");
	if (mainPos == std::string::npos) {
		fprintf(stderr, "[MDEI] PatchVertexCode: could not find void main()\n");
		return code;
	}
	code.insert(mainPos, SSBO_INJECTION);

	/* 2. After insertion the position has shifted — try Form A first, then B. */
	size_t coPos = code.find(ORIG_CO_LINE_A);
	if (coPos != std::string::npos) {
		code.replace(coPos, strlen(ORIG_CO_LINE_A), PATCHED_CO_LINES_A);
		return code;
	}

	coPos = code.find(ORIG_CO_LINE_B);
	if (coPos != std::string::npos) {
		code.replace(coPos, strlen(ORIG_CO_LINE_B), PATCHED_CO_LINES_B);
		return code;
	}

	fprintf(stderr, "[MDEI] PatchVertexCode: could not find gl_ModelViewMatrix line "
	        "(neither Form A nor Form B) — object will render at origin\n");
	return code;
}

/* ─────────────────────────────────────────────────────────────────────── */

MDEI_Shader::MDEI_Shader()
	: m_solidShader(nullptr), m_shadowShader(nullptr),
	  m_locViewMat(-1), m_locInvViewMat(-1), m_locTime(-1),
	  m_shadowLocViewMat(-1), m_shadowLocTime(-1),
	  m_gpuMat(nullptr), m_scene(nullptr)
{
}

MDEI_Shader::~MDEI_Shader()
{
	if (m_solidShader)  { GPU_shader_free(m_solidShader);  m_solidShader  = nullptr; }
	if (m_shadowShader) { GPU_shader_free(m_shadowShader); m_shadowShader = nullptr; }
}

bool MDEI_Shader::CompileFromPass(GPUPass     *pass,
                                  GPUMaterial *gpuMat,
                                  Scene       *scene,
                                  bool         variance)
{
	const char *vertCode = GPU_pass_get_vertexcode(pass);
	const char *fragCode = GPU_pass_get_fragmentcode(pass);
	const char *libCode  = GPU_pass_get_libcode(pass);

	if (!vertCode || !fragCode) {
		fprintf(stderr, "[MDEI] CompileFromPass: GPUPass has no code\n");
		return false;
	}

	/* Store for use in BindSolid every frame */
	m_gpuMat = gpuMat;
	m_scene  = scene;

	/* Patch vertex code */
	std::string patchedVert = PatchVertexCode(vertCode);

	/* Compile solid shader: patched vert + original frag + lib */
	m_solidShader = GPU_shader_create_ex(
		patchedVert.c_str(),
		fragCode,
		nullptr,
		libCode,
		MDEI_VERT_DEFINES,
		0, 0, 0,
		GPU_SHADER_FLAGS_NONE);

	if (!m_solidShader) {
		fprintf(stderr, "[MDEI] Solid shader compilation failed\n");
		return false;
	}

	/* Cache the handful of locations we set manually every frame for speed.
	 * All other uniforms are resolved by name in GPU_material_bind_to_shader. */
	m_locViewMat    = GPU_shader_get_uniform(m_solidShader, "unfviewmat");
	m_locInvViewMat = GPU_shader_get_uniform(m_solidShader, "unfinvviewmat");
	m_locTime       = GPU_shader_get_uniform(m_solidShader, "unftime");

	return CompileShadowShader(vertCode, variance);
}

bool MDEI_Shader::CompileShadowShader(const char *originalVertCode, bool variance)
{
	std::string patchedVert = PatchVertexCode(originalVertCode);

	/* Fragment: simple depth-only, or VSM moment output.
	 * NO #version here — GPU_shader_create_ex prepends it. */
	const char *shadowFrag;
	if (variance) {
		shadowFrag =
			"out vec4 fragColor;\n"
			"void main() {\n"
			"    float d = gl_FragCoord.z;\n"
			"    fragColor = vec4(d, d * d, 0.0, 1.0);\n"
			"}\n";
	}
	else {
		shadowFrag = "void main() {}\n";
	}

	m_shadowShader = GPU_shader_create_ex(
		patchedVert.c_str(),
		shadowFrag,
		nullptr,
		nullptr,
		MDEI_VERT_DEFINES,
		0, 0, 0,
		GPU_SHADER_FLAGS_NONE);

	if (!m_shadowShader) {
		fprintf(stderr, "[MDEI] Shadow shader compilation failed\n");
		return false;
	}

	m_shadowLocViewMat = GPU_shader_get_uniform(m_shadowShader, "unfviewmat");
	m_shadowLocTime    = GPU_shader_get_uniform(m_shadowShader, "unftime");
	return true;
}

void MDEI_Shader::BindSolid(RAS_Rasterizer *rasty)
{
	GPU_shader_bind(m_solidShader);

	/* Upload ALL material uniforms (textures, lamps, builtins, custom) into
	 * m_solidShader using the GPUMaterial's pass inputs as the source of
	 * truth.  GPU_material_bind_to_shader() resolves each uniform by name in
	 * our external shader rather than in pass->shader. */
	if (m_gpuMat) {
		const mt::mat4& vm  = rasty->GetViewMatrix();
		const mt::mat4& vim = rasty->GetViewInvMatrix();

		/* GPU_material_update_lamps: transform lamp positions to view space */
		GPU_material_update_lamps(m_gpuMat, vm.Data(), vim.Data());

		/* Bind all uniforms to our m_solidShader */
		GPU_material_bind_to_shader(
			m_gpuMat, m_solidShader,
			m_scene ? m_scene->lay : 0xFFFF,
			rasty->GetTime(),
			1,           /* mipmap = 1 */
			vm.Data(),
			vim.Data(),
			nullptr      /* camerafactors = nullptr → defaults */
		);
	}
}

void MDEI_Shader::UnbindSolid()
{
	/* Unbind textures that were bound in BindSolid.
	 * GPU_pass_unbind releases texture refs; we do a minimal version. */
	if (m_gpuMat) {
		GPUPass *pass = GPU_material_get_pass(m_gpuMat);
		if (pass) {
			/* Iterate inputs and unbind textures from their units */
			/* We can call GPU_pass_unbind only if we had called GPU_pass_bind.
			 * Since we did NOT call GPU_pass_bind (we went direct to our shader),
			 * we just unbind the shader — texture units are cleaned up by the
			 * next GPU_pass_bind call of the regular RAS path. */
		}
	}
	GPU_shader_unbind();
}

void MDEI_Shader::BindShadow(RAS_Rasterizer *rasty)
{
	GPU_shader_bind(m_shadowShader);

	/* Shadow shaders only need the view matrix and time */
	if (m_shadowLocViewMat >= 0) {
		const mt::mat4& vm = rasty->GetViewMatrix();
		GPU_shader_uniform_vector(m_shadowShader, m_shadowLocViewMat, 16, 1,
		                          &vm.Data()[0][0]);
	}
	if (m_shadowLocTime >= 0) {
		float t = (float)rasty->GetTime();
		GPU_shader_uniform_vector(m_shadowShader, m_shadowLocTime, 1, 1, &t);
	}
}

void MDEI_Shader::UnbindShadow()
{
	GPU_shader_unbind();
}

bool MDEI_Shader::GetGPUVertexAttribs(GPUVertexAttribs &out) const
{
	if (!m_gpuMat) return false;
	GPU_material_vertex_attributes(m_gpuMat, &out);
	return true;
}
