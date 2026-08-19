/*
 * MDEI_Shader.cpp
 */

#include "MDEI_Shader.h"

#include "GPU_material.h"
#include "GPU_shader.h"
#include "GPU_texture.h"
#include "GPU_draw.h"   /* GPU_get_anisotropic, GPU_get_mipmap, GPU_get_mipmap_filter */

/* gpu_codegen.h exposes GPUInput / GPUPass::inputs for direct texture access */
#include "intern/gpu_codegen.h"

#include "RAS_Rasterizer.h"
#include "RAS_IMaterial.h"
#include "RAS_Texture.h"

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

	/* 1. Inject SSBO declaration before void main().
	 * After the insert, mainPos still points at the start of the injected block.
	 * We advance it past the injection so it points at "void main()" again. */
	size_t mainPos = code.find("void main()");
	if (mainPos == std::string::npos) {
		fprintf(stderr, "[MDEI] PatchVertexCode: could not find void main()\n");
		return code;
	}
	code.insert(mainPos, SSBO_INJECTION);
	/* mainPos now points at SSBO_INJECTION[0]; advance to "void main()". */
	mainPos += strlen(SSBO_INJECTION);

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

	/* 3. Form C — legacy gl_Vertex / gl_Normal shader (user-written raw GLSL).
	 *
	 * Strategy: text search-and-replace inside void main() body only.
	 * We CANNOT use #define to shadow GLSL built-ins (illegal in GLSL >= 130,
	 * which is what gpu_shader_version() emits on modern drivers).
	 *
	 * Instead:
	 *   a) Detect whether the shader body uses gl_Vertex / gl_Normal.
	 *   b) Declare shadow variables _mdei_gl_Vertex / _mdei_gl_Normal at the
	 *      very top of main() and initialise them from the SSBO model matrix.
	 *   c) Rename every occurrence of gl_Vertex / gl_Normal inside main() to
	 *      the shadow variable — skipping gl_NormalMatrix (different built-in).
	 *
	 * This produces valid GLSL for any version and any driver.
	 */
	{
		/* Find opening brace of void main() */
		size_t bracePos = code.find('{', mainPos);
		if (bracePos == std::string::npos) {
			fprintf(stderr, "[MDEI] PatchVertexCode: could not find '{' of main()\n");
			return code;
		}
		/* The body starts right after the brace. */
		const size_t bodyStart = bracePos + 1;

		/* Scan the body for whole-word gl_Vertex / gl_Normal occurrences.
		 * "Whole-word": the character immediately after the token must NOT be
		 * an identifier char — this prevents matching gl_NormalMatrix etc. */
		auto countToken = [&](const std::string &tok) -> int {
			int n = 0;
			size_t pos = bodyStart;
			while ((pos = code.find(tok, pos)) != std::string::npos) {
				size_t end = pos + tok.size();
				if (end >= code.size() || !(isalnum((unsigned char)code[end]) || code[end] == '_'))
					++n;
				pos += tok.size();
			}
			return n;
		};

		const bool usesGlVertex = countToken("gl_Vertex") > 0;
		/* gl_Normal is used — but we must not match gl_NormalMatrix */
		const bool usesGlNormal = countToken("gl_Normal") > 0;

		if (!usesGlVertex && !usesGlNormal) {
			fprintf(stderr, "[MDEI] PatchVertexCode: Form C — no gl_Vertex/gl_Normal in body; "
			        "SSBO injected, no rename needed\n");
			return code;
		}

		/* ── Step 1: rename occurrences INSIDE main() body ─────────────────
		 * We walk the string from bodyStart to the end, replacing whole-word
		 * matches only.  The replacements are longer than the originals so we
		 * accumulate into a new string to avoid index invalidation. */
		auto renameToken = [&](const std::string &src, const std::string &dst) {
			size_t pos = bodyStart;
			while ((pos = code.find(src, pos)) != std::string::npos) {
				size_t end = pos + src.size();
				bool boundOk = (end >= code.size() ||
				                !(isalnum((unsigned char)code[end]) || code[end] == '_'));
				if (boundOk) {
					code.replace(pos, src.size(), dst);
					pos += dst.size();
				} else {
					pos += src.size();
				}
			}
		};

		if (usesGlVertex) renameToken("gl_Vertex", "_mdei_gl_Vertex");
		if (usesGlNormal) renameToken("gl_Normal", "_mdei_gl_Normal");

		/* ── Step 2: inject declarations at the top of main() ──────────────
		 * bracePos has not moved (renaming happens after it), so insert after
		 * the opening brace. */
		std::string decls;
		decls += "\n\t/* ── MDEI Form C: instance transform (text-rename, no #define) ── */\n";
		decls += "\tMDEI_Instance _inst = mdei_instances[gl_BaseInstanceARB + gl_InstanceID];\n";
		decls += "\tvarinstcolor = _inst.color;\n";
		if (usesGlVertex) {
			decls += "\tvec4 _mdei_gl_Vertex = _inst.modelMatrix * gl_Vertex;\n";
		}
		if (usesGlNormal) {
			decls += "\tvec3 _mdei_gl_Normal = "
			         "mat3(transpose(inverse(mat3(_inst.modelMatrix)))) * gl_Normal;\n";
		}
		code.insert(bracePos + 1, decls);

		fprintf(stderr, "[MDEI] PatchVertexCode: Form C applied via text-rename "
		        "(usesGlVertex=%d usesGlNormal=%d)\n",
		        (int)usesGlVertex, (int)usesGlNormal);
		return code;
	}
}

/* ─────────────────────────────────────────────────────────────────────── */

MDEI_Shader::MDEI_Shader()
	: m_solidShader(nullptr), m_shadowShader(nullptr),
	  m_locViewMat(-1), m_locInvViewMat(-1), m_locTime(-1),
	  m_shadowLocViewMat(-1), m_shadowLocTime(-1),
	  m_gpuMat(nullptr), m_scene(nullptr), m_material(nullptr),
	  m_cullFace(true),  /* padrão: cull face ativado */
	  m_dbgBindCount(0)
{
}

MDEI_Shader::~MDEI_Shader()
{
	/* Free sampler arrays + their bindless handles */
	if (GLEW_ARB_bindless_texture) {
		for (auto &e : m_samplerArrays) {
			if (e.bindlessHandle && glIsTextureHandleResidentARB(e.bindlessHandle))
				glMakeTextureHandleNonResidentARB(e.bindlessHandle);
			e.bindlessHandle = 0;
		}
	}
	for (auto &e : m_samplerArrays)
		if (e.arrayTex) { glDeleteTextures(1, &e.arrayTex); e.arrayTex = 0; }
	m_samplerArrays.clear();

	/* Release bindless handles and any owned GL_TEXTURE_2D_ARRAY objects */
	if (GLEW_ARB_bindless_texture) {
		for (auto &b : m_bindlessHandles) {
			if (b.handle && glIsTextureHandleResidentARB(b.handle))
				glMakeTextureHandleNonResidentARB(b.handle);
			if (b.ownedTex)
				glDeleteTextures(1, &b.ownedTex);
		}
	}
	m_bindlessHandles.clear();

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

/* Forward declarations das funções helper de uniform queue definidas abaixo. */
static MDEI_Shader::UniformEntry &mdei_upsert(std::vector<MDEI_Shader::UniformEntry> &q, const char *name);
static void mdei_apply_entry(GPUShader *sh, const MDEI_Shader::UniformEntry &e);

void MDEI_Shader::BindSolid(RAS_Rasterizer *rasty)
{
	if (!m_solidShader) return;

	GPU_shader_bind(m_solidShader);

	if (m_gpuMat) {
		const int lay = m_scene ? m_scene->lay : 0xFFFF;
		if (m_customVertSrc.empty()) {
			/* Shader padrão: bind completo — texturas + uniforms do material. */
			const mt::mat4& vm  = rasty->GetViewMatrix();
			const mt::mat4& vim = rasty->GetViewInvMatrix();
			GPU_material_update_lamps(m_gpuMat, vm.Data(), vim.Data());
			GPU_material_bind_to_shader(
				m_gpuMat, m_solidShader,
				lay, rasty->GetTime(),
				1, vm.Data(), vim.Data(), nullptr);
		}
		else {
			/* Shader customizado: apenas carrega inp->tex nos slots do GPUPass
			 * (necessário para que ResolveSlotsThroughPass encontre as texturas).
			 * Nenhum glUniform* do material Blender é escrito — o shader customizado
			 * não tem esses uniforms e eles colidiriam com as locações 0/2/3. */
			GPU_material_prepare_textures(m_gpuMat, lay, rasty->GetTime(), 1);
		}
	}

	/* Shader customizado: re-aplica a queue de uniforms via glProgramUniform*
	 * uma única vez por frame (após prepare_textures, que não escreve uniforms). */
	if (!m_customVertSrc.empty() && !m_uniformQueue.empty()) {
		/* DEBUG one-shot: mostra loc de cada entrada na primeira chamada */
		if (!m_dbgBindCount) {
			GLuint prog = (GLuint)GPU_shader_program(m_solidShader);
			fprintf(stderr, "[MDEI_BIND] prog=%u  queue=%d entries\n",
			        prog, (int)m_uniformQueue.size());
			for (const auto &ue : m_uniformQueue) {
				int l = GPU_shader_get_uniform(m_solidShader, ue.name.c_str());
				fprintf(stderr, "  '%s' loc=%d  fv=(%.3f,%.3f,%.3f)\n",
				        ue.name.c_str(), l, ue.fv[0], ue.fv[1], ue.fv[2]);
			}
		}
		m_dbgBindCount++;
		for (const auto &e : m_uniformQueue)
			mdei_apply_entry(m_solidShader, e);
	}

	/* Apply custom sampler arrays (set via SetSamplerArray / setSamplerArray) */
	BindSamplerArrays();
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
	if (!m_shadowShader) return;
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

/* ══════════════════════════════════════════════════════════════════════════
 * Custom source override
 * ══════════════════════════════════════════════════════════════════════════ */

void MDEI_Shader::SetVertexSource(const std::string &vertSrc)
{
	m_customVertSrc = vertSrc;
	RebuildSolidFromSource();
}

void MDEI_Shader::SetFragmentSource(const std::string &fragSrc)
{
	m_customFragSrc = fragSrc;
	RebuildSolidFromSource();
}

void MDEI_Shader::RebuildSolidFromSource()
{
	if (m_customVertSrc.empty() || m_customFragSrc.empty())
		return; /* wait until both are set */

	if (m_solidShader) {
		GPU_shader_free(m_solidShader);
		m_solidShader = nullptr;
	}

	std::string patchedVert = PatchVertexCode(m_customVertSrc.c_str());

	m_solidShader = GPU_shader_create_ex(
		patchedVert.c_str(),
		m_customFragSrc.c_str(),
		nullptr, nullptr,
		MDEI_VERT_DEFINES,
		0, 0, 0,
		GPU_SHADER_FLAGS_NONE);

	if (!m_solidShader) {
		fprintf(stderr, "[MDEI] RebuildSolidFromSource: compilation FAILED\n"
		        "=== PATCHED VERTEX (first 3000 chars) ===\n%.3000s\n"
		        "=== FRAGMENT (first 1000 chars) ===\n%.1000s\n"
		        "=========================================\n",
		        patchedVert.c_str(), m_customFragSrc.c_str());
		return;
	}

	m_locViewMat    = GPU_shader_get_uniform(m_solidShader, "unfviewmat");
	m_locInvViewMat = GPU_shader_get_uniform(m_solidShader, "unfinvviewmat");
	m_locTime       = GPU_shader_get_uniform(m_solidShader, "unftime");

	/* Re-aplica a fila de uniforms customizados ao novo programa GL via
	 * glProgramUniform* — não precisa de bind/unbind. */
	for (const auto &e : m_uniformQueue)
		mdei_apply_entry(m_solidShader, e);

	/* Marca todos os sampler arrays como dirty para que BindSamplerArrays()
	 * reupload as texturas no novo programa na próxima BindSolid(). */
	for (auto &e : m_samplerArrays)
		e.dirty = true;

}

/* ══════════════════════════════════════════════════════════════════════════
 * Uniform helpers
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Uniform queue helpers ──────────────────────────────────────────────────
 * SetUniform* enfileira o valor na m_uniformQueue (upsert por nome).
 * BindSolid re-aplica toda a fila a cada frame, garantindo que uniforms
 * sobrevivam a RebuildSolidFromSource (novo programa GL).
 * ──────────────────────────────────────────────────────────────────────── */

/* Encontra entrada existente pelo nome, ou insere nova no final e retorna ref.
 * Nota: push_back pode realocar o vector e invalidar referências existentes.
 * Para evitar isso, localizamos o índice antes do push_back e retornamos por índice. */
static MDEI_Shader::UniformEntry &mdei_upsert(std::vector<MDEI_Shader::UniformEntry> &q,
                                               const char *name)
{
	for (size_t i = 0; i < q.size(); ++i)
		if (q[i].name == name)
			return q[i];
	/* Não encontrado — adiciona nova entrada. push_back pode realocar, por isso
	 * guardamos o índice antes e retornamos via q[idx] após o push_back. */
	size_t idx = q.size();
	q.push_back(MDEI_Shader::UniformEntry{});
	q[idx].name = name;
	return q[idx];
}

/* Aplica uma entrada da fila.
 * Usa glProgramUniform* (GL_ARB_separate_shader_objects / GL 4.1+) quando
 * disponível — independente do programa ativo via glUseProgram.
 * Fallback: bind explícito + glUniform* (sempre suportado). */
static void mdei_apply_entry(GPUShader *sh, const MDEI_Shader::UniformEntry &e)
{
	if (!sh) return;
	GLuint prog = (GLuint)GPU_shader_program(sh);
	if (!prog) return;
	int loc = GPU_shader_get_uniform(sh, e.name.c_str());
	if (loc < 0) return;

	/* GL_ARB_separate_shader_objects está em todo driver GL 4.1+.
	 * GLEW expõe a flag GLEW_ARB_separate_shader_objects. */
	if (GLEW_ARB_separate_shader_objects) {
		switch (e.type) {
			case MDEI_Shader::UniformEntry::F1:
				glProgramUniform1fv(prog, loc, 1, e.fv); break;
			case MDEI_Shader::UniformEntry::F2:
				glProgramUniform2fv(prog, loc, 1, e.fv); break;
			case MDEI_Shader::UniformEntry::F3:
				glProgramUniform3fv(prog, loc, 1, e.fv); break;
			case MDEI_Shader::UniformEntry::F4:
				glProgramUniform4fv(prog, loc, 1, e.fv); break;
			case MDEI_Shader::UniformEntry::I1:
				glProgramUniform1iv(prog, loc, 1, e.iv); break;
			case MDEI_Shader::UniformEntry::I2:
				glProgramUniform2iv(prog, loc, 1, e.iv); break;
			case MDEI_Shader::UniformEntry::I3:
				glProgramUniform3iv(prog, loc, 1, e.iv); break;
			case MDEI_Shader::UniformEntry::I4:
				glProgramUniform4iv(prog, loc, 1, e.iv); break;
			case MDEI_Shader::UniformEntry::FV:
				glProgramUniform1fv(prog, loc, e.count, e.fv); break;
			case MDEI_Shader::UniformEntry::IV:
				glProgramUniform1iv(prog, loc, e.count, e.iv); break;
			case MDEI_Shader::UniformEntry::MAT3:
				glProgramUniformMatrix3fv(prog, loc, 1, GL_FALSE, e.fv); break;
			case MDEI_Shader::UniformEntry::MAT4:
				glProgramUniformMatrix4fv(prog, loc, 1, GL_FALSE, e.fv); break;
			default: break;
		}
	}
	else {
		/* Fallback: o shader já está bindado pelo caller (BindSolid chama
		 * GPU_shader_bind antes deste ponto). */
		switch (e.type) {
			case MDEI_Shader::UniformEntry::F1:
				GPU_shader_uniform_vector(sh, loc, 1, 1, e.fv); break;
			case MDEI_Shader::UniformEntry::F2:
				GPU_shader_uniform_vector(sh, loc, 2, 1, e.fv); break;
			case MDEI_Shader::UniformEntry::F3:
				GPU_shader_uniform_vector(sh, loc, 3, 1, e.fv); break;
			case MDEI_Shader::UniformEntry::F4:
				GPU_shader_uniform_vector(sh, loc, 4, 1, e.fv); break;
			case MDEI_Shader::UniformEntry::I1:
				GPU_shader_uniform_int(sh, loc, e.iv[0]); break;
			case MDEI_Shader::UniformEntry::I2:
				GPU_shader_uniform_vector_int(sh, loc, 2, 1, e.iv); break;
			case MDEI_Shader::UniformEntry::I3:
				GPU_shader_uniform_vector_int(sh, loc, 3, 1, e.iv); break;
			case MDEI_Shader::UniformEntry::I4:
				GPU_shader_uniform_vector_int(sh, loc, 4, 1, e.iv); break;
			case MDEI_Shader::UniformEntry::FV:
				GPU_shader_uniform_vector(sh, loc, e.count, 1, e.fv); break;
			case MDEI_Shader::UniformEntry::IV:
				GPU_shader_uniform_vector_int(sh, loc, e.count, 1, e.iv); break;
			case MDEI_Shader::UniformEntry::MAT3:
				GPU_shader_uniform_vector(sh, loc, 9, 1, e.fv); break;
			case MDEI_Shader::UniformEntry::MAT4:
				GPU_shader_uniform_vector(sh, loc, 16, 1, e.fv); break;
			default: break;
		}
	}
}

void MDEI_Shader::SetUniform1f(const char *name, float v)
{
	auto &e = mdei_upsert(m_uniformQueue, name);
	e.type = UniformEntry::F1; e.fv[0] = v;
	if (m_solidShader) mdei_apply_entry(m_solidShader, e);
}
void MDEI_Shader::SetUniform2f(const char *name, float x, float y)
{
	auto &e = mdei_upsert(m_uniformQueue, name);
	e.type = UniformEntry::F2; e.fv[0]=x; e.fv[1]=y;
	if (m_solidShader) mdei_apply_entry(m_solidShader, e);
}
void MDEI_Shader::SetUniform3f(const char *name, float x, float y, float z)
{
	auto &e = mdei_upsert(m_uniformQueue, name);
	e.type = UniformEntry::F3; e.fv[0]=x; e.fv[1]=y; e.fv[2]=z;
	if (m_solidShader) mdei_apply_entry(m_solidShader, e);
}
void MDEI_Shader::SetUniform4f(const char *name, float x, float y, float z, float w)
{
	auto &e = mdei_upsert(m_uniformQueue, name);
	e.type = UniformEntry::F4; e.fv[0]=x; e.fv[1]=y; e.fv[2]=z; e.fv[3]=w;
	if (m_solidShader) mdei_apply_entry(m_solidShader, e);
}
void MDEI_Shader::SetUniform1i(const char *name, int v)
{
	auto &e = mdei_upsert(m_uniformQueue, name);
	e.type = UniformEntry::I1; e.iv[0] = v;
	if (m_solidShader) mdei_apply_entry(m_solidShader, e);
}
void MDEI_Shader::SetUniform2i(const char *name, int x, int y)
{
	auto &e = mdei_upsert(m_uniformQueue, name);
	e.type = UniformEntry::I2; e.iv[0]=x; e.iv[1]=y;
	if (m_solidShader) mdei_apply_entry(m_solidShader, e);
}
void MDEI_Shader::SetUniform3i(const char *name, int x, int y, int z)
{
	auto &e = mdei_upsert(m_uniformQueue, name);
	e.type = UniformEntry::I3; e.iv[0]=x; e.iv[1]=y; e.iv[2]=z;
	if (m_solidShader) mdei_apply_entry(m_solidShader, e);
}
void MDEI_Shader::SetUniform4i(const char *name, int x, int y, int z, int w)
{
	auto &e = mdei_upsert(m_uniformQueue, name);
	e.type = UniformEntry::I4; e.iv[0]=x; e.iv[1]=y; e.iv[2]=z; e.iv[3]=w;
	if (m_solidShader) mdei_apply_entry(m_solidShader, e);
}
void MDEI_Shader::SetUniformfv(const char *name, const float *v, int count)
{
	auto &e = mdei_upsert(m_uniformQueue, name);
	e.type = UniformEntry::FV; e.count = count;
	int n = count < 16 ? count : 16;
	for (int i = 0; i < n; ++i) e.fv[i] = v[i];
	if (m_solidShader) mdei_apply_entry(m_solidShader, e);
}
void MDEI_Shader::SetUniformiv(const char *name, const int *v, int count)
{
	auto &e = mdei_upsert(m_uniformQueue, name);
	e.type = UniformEntry::IV; e.count = count;
	int n = count < 4 ? count : 4;
	for (int i = 0; i < n; ++i) e.iv[i] = v[i];
	if (m_solidShader) mdei_apply_entry(m_solidShader, e);
}
void MDEI_Shader::SetUniformMatrix3(const char *name, const float m9[9], bool transpose)
{
	auto &e = mdei_upsert(m_uniformQueue, name);
	e.type = UniformEntry::MAT3; e.transpose = transpose;
	for (int i = 0; i < 9; ++i) e.fv[i] = m9[i];
	if (m_solidShader) mdei_apply_entry(m_solidShader, e);
}
void MDEI_Shader::SetUniformMatrix4(const char *name, const float m16[16], bool transpose)
{
	auto &e = mdei_upsert(m_uniformQueue, name);
	e.type = UniformEntry::MAT4; e.transpose = transpose;
	for (int i = 0; i < 16; ++i) e.fv[i] = m16[i];
	if (m_solidShader) mdei_apply_entry(m_solidShader, e);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Classic samplers
 * ══════════════════════════════════════════════════════════════════════════ */

void MDEI_Shader::SetSampler(const char *name, GLuint glTexId, int unit)
{
	if (!m_solidShader) return;
	int loc = GPU_shader_get_uniform(m_solidShader, name);
	if (loc < 0) return;
	GPU_shader_bind(m_solidShader);
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_2D, glTexId);
	GPU_shader_uniform_int(m_solidShader, loc, unit);
	GPU_shader_unbind();
}

int MDEI_Shader::AllocUnit()
{
	/* Collect used units */
	int used = 0;
	for (auto &e : m_samplerArrays)
		if (e.unit >= 0) used |= (1 << e.unit);
	for (int u = 8; u < 32; ++u)
		if (!(used & (1 << u))) return u;
	return 8; /* fallback */
}

/* ── Helper: resolve slot indices [0,1,2] → GPUTexture* via GPUPass inputs ──
 * This mirrors GPU_material_bind_to_shader's texture loop, but we iterate by
 * texid (slot number) instead of by name.  Works entirely from m_gpuMat —
 * no RAS_IMaterial needed.
 *
 * Returns true when ALL slots were resolved; false if any was not ready yet.
 * On success, fills `out` with one bindcode per slot in order.
 * ──────────────────────────────────────────────────────────────────────────── */
static bool ResolveSlotsThroughPass(GPUMaterial *gpuMat,
                                    const std::vector<int> &slots,
                                    std::vector<GLuint> &out)
{
	if (!gpuMat) {
		fprintf(stderr, "[MDEI] ResolveSlotsThroughPass: gpuMat is null\n");
		return false;
	}
	GPUPass *pass = GPU_material_get_pass(gpuMat);
	if (!pass) {
		fprintf(stderr, "[MDEI] ResolveSlotsThroughPass: GPUPass is null\n");
		return false;
	}

	out.clear();
	out.reserve(slots.size());

	for (int slot : slots) {
		GPUTexture *found = nullptr;
		/* Walk the pass input list looking for texid == slot */
		for (GPUInput *inp = (GPUInput*)pass->inputs.first; inp; inp = inp->next) {
			if (!inp->bindtex) continue;
			if (inp->texid != slot) continue;
			/* inp->tex is filled by GPU_texture_from_blender inside bind_to_shader */
			GPUTexture *gt = inp->tex ? inp->tex :
			                 (inp->texptr ? *inp->texptr : nullptr);
			if (gt) { found = gt; break; }
		}
		if (!found) {
			/* Slot ainda não carregado — será retentado no próximo frame */
			return false;
		}
		int tw = GPU_texture_width(found);
		int th = GPU_texture_height(found);
		if (tw <= 4 || th <= 4) {
			/* Textura ainda não pronta — será retentada */
			return false;
		}
		GLuint bc = (GLuint)GPU_texture_opengl_bindcode(found);
		out.push_back(bc);
	}
	return true;
}

void MDEI_Shader::RebuildSamplerArray(SamplerArrayEntry &e)
{
	/* ── 1. Resolve source GL ids ────────────────────────────────────────── */
	std::vector<GLuint> ids;

	if (e.fromSlots) {
		if (!ResolveSlotsThroughPass(m_gpuMat, e.srcSlots, ids)) {
			/* Not ready yet — dirty flag stays, retry next frame */
			return;
		}
		e.dirty = false;

		/* Short-circuit: same sources → keep existing array, just ensure residency */
		if (e.arrayTex && ids == e.srcIds) {
			if (GLEW_ARB_bindless_texture && e.bindlessHandle &&
			    !glIsTextureHandleResidentARB(e.bindlessHandle)) {
				glMakeTextureHandleResidentARB(e.bindlessHandle);
			}
			return;
		}
		e.srcIds = ids;
	}
	else if (e.fromGpu) {
		ids.reserve(e.srcGpu.size());
		for (GPUTexture *t : e.srcGpu)
			ids.push_back(t ? (GLuint)GPU_texture_opengl_bindcode(t) : 0);
	}
	else {
		ids = e.srcIds;
	}

	/* ── 2. Release old bindless handle + array ──────────────────────────── */
	if (GLEW_ARB_bindless_texture && e.bindlessHandle) {
		if (glIsTextureHandleResidentARB(e.bindlessHandle))
			glMakeTextureHandleNonResidentARB(e.bindlessHandle);
		e.bindlessHandle = 0;
	}
	if (e.arrayTex) { glDeleteTextures(1, &e.arrayTex); e.arrayTex = 0; }

	/* ── 3. Build GL_TEXTURE_2D_ARRAY ────────────────────────────────────── */
	int count = (int)ids.size();
	if (count == 0) return;

	GLint w = 0, h = 0;
	glGetTextureLevelParameteriv(ids[0], 0, GL_TEXTURE_WIDTH,  &w);
	glGetTextureLevelParameteriv(ids[0], 0, GL_TEXTURE_HEIGHT, &h);

	if (w <= 0 || h <= 0) {
		fprintf(stderr, "[MDEI] RebuildSamplerArray '%s': invalid dimensions (%dx%d)\n",
		        e.name.c_str(), (int)w, (int)h);
		return;
	}

	/* GL 4.5 DSA: lê cada textura fonte com glGetTextureImage — descomprime
	 * DXT/BC automaticamente para RGBA8 independente do target real (2D, ARRAY,
	 * CUBE, etc.).  O array destino é sempre RGBA8 — sem problema de formato. */
	const size_t pixelBytes = (size_t)w * (size_t)h * 4;
	std::vector<unsigned char> pixels(pixelBytes);

	/* Sincroniza com o estado global de mipmap do RAS ao criar a textura.
	 * e.mipmap pode ter sido setado explicitamente; se não, reflete GPU_get_mipmap(). */
	const bool doMipmap = e.mipmap;

	/* Número de níveis mipmap — apenas 1 se mipmap não for solicitado. */
	int mipLevels = 1;
	if (doMipmap) {
		int maxDim = w > h ? w : h;
		while (maxDim > 1) { maxDim >>= 1; mipLevels++; }
	}

	glGenTextures(1, &e.arrayTex);
	/* Aloca armazenamento imutável RGBA8 para o array destino. */
	glBindTexture(GL_TEXTURE_2D_ARRAY, e.arrayTex);
	glTexStorage3D(GL_TEXTURE_2D_ARRAY, mipLevels, GL_RGBA8, w, h, count);
	glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

	for (int i = 0; i < count; ++i) {
		/* glGetTextureImage: DSA, GL 4.5 — não precisa de bind, não precisa
		 * saber o target real, descomprime automaticamente para GL_RGBA/UBYTE. */
		glGetError(); /* limpa erros pendentes */
		glGetTextureImage(ids[i], 0, GL_RGBA, GL_UNSIGNED_BYTE,
		                  (GLsizei)pixelBytes, pixels.data());
		GLenum err = glGetError();
		if (err != GL_NO_ERROR) {
			fprintf(stderr, "[MDEI] RebuildSamplerArray '%s': glGetTextureImage layer %d error 0x%x\n",
			        e.name.c_str(), i, err);
			continue;
		}
		/* Upload da camada no array destino via DSA. */
		glTextureSubImage3D(e.arrayTex, 0,
		                    0, 0, i,
		                    w, h, 1,
		                    GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
		err = glGetError();
		if (err != GL_NO_ERROR)
			fprintf(stderr, "[MDEI] RebuildSamplerArray '%s': glTextureSubImage3D layer %d error 0x%x\n",
			        e.name.c_str(), i, err);
	}

	/* Calcula o filtro MIN a partir do estado global, igual ao RAS.
	 * MipmapFilter→GL map:
	 *   0 NEAREST_MIPMAP_NEAREST → GL_NEAREST_MIPMAP_NEAREST
	 *   1 NEAREST_MIPMAP_LINEAR  → GL_NEAREST_MIPMAP_LINEAR
	 *   2 LINEAR_MIPMAP_NEAREST  → GL_LINEAR_MIPMAP_NEAREST
	 *   3 LINEAR_MIPMAP_LINEAR   → GL_LINEAR_MIPMAP_LINEAR  (default) */
	static const GLenum kMipmapFilterGL[4] = {
		GL_NEAREST_MIPMAP_NEAREST,
		GL_NEAREST_MIPMAP_LINEAR,
		GL_LINEAR_MIPMAP_NEAREST,
		GL_LINEAR_MIPMAP_LINEAR,
	};
	GLenum minFilter;
	if (doMipmap) {
		const GPU_MipmapFilter gf = GPU_get_mipmap_filter();
		minFilter = kMipmapFilterGL[gf < 4 ? (int)gf : 3];
	} else {
		minFilter = GL_LINEAR;
	}

	glBindTexture(GL_TEXTURE_2D_ARRAY, e.arrayTex);
	if (doMipmap) {
		glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
	}
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, minFilter);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
	/* Anisotropic: mesmo valor que o RAS usa para texturas Blender. */
	if (GLEW_EXT_texture_filter_anisotropic) {
		glTexParameterf(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_ANISOTROPY_EXT,
		                GPU_get_anisotropic());
	}
	glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

	/* ── 4. Make the array bindless ──────────────────────────────────────── */
	if (GLEW_ARB_bindless_texture) {
		e.bindlessHandle = glGetTextureHandleARB(e.arrayTex);
		if (e.bindlessHandle)
			glMakeTextureHandleResidentARB(e.bindlessHandle);
		else
			fprintf(stderr, "[MDEI] RebuildSamplerArray '%s': glGetTextureHandleARB=0!\n",
			        e.name.c_str());
	}
}

void MDEI_Shader::SetSamplerArray(const char *name, const GLuint *ids, int count, bool mipmap)
{
	/* Find existing entry or create new */
	for (auto &e : m_samplerArrays) {
		if (e.name == name) {
			e.srcIds.assign(ids, ids + count);
			e.fromGpu = false;
			e.mipmap  = mipmap;
			RebuildSamplerArray(e);
			return;
		}
	}
	SamplerArrayEntry e;
	e.name    = name;
	e.arrayTex= 0;
	e.unit    = AllocUnit();
	e.mipmap  = mipmap;
	e.fromGpu = false;
	e.srcIds.assign(ids, ids + count);
	RebuildSamplerArray(e);
	m_samplerArrays.push_back(std::move(e));
}

/* ── SetSamplerArrayFromSlots ────────────────────────────────────────────────
 * Just registers the slot list and marks the entry dirty.
 * The actual texture resolution happens lazily in RebuildSamplerArray via
 * ResolveSlotsThroughPass (uses m_gpuMat->pass->inputs, no RAS_IMaterial needed).
 * RebuildSamplerArray is called every frame from BindSamplerArrays until the
 * textures are ready (inp->tex filled by GPU_material_bind_to_shader).
 * ──────────────────────────────────────────────────────────────────────────── */
void MDEI_Shader::SetSamplerArrayFromSlots(const char *name, const int *slots, int count, bool mipmap)
{
	if (count <= 0) return;

	/* Update existing entry or create new */
	for (auto &e : m_samplerArrays) {
		if (e.name == name) {
			e.srcSlots.assign(slots, slots + count);
			e.fromSlots = true;
			e.mipmap    = mipmap;
			e.dirty     = true;
			return;
		}
	}
	SamplerArrayEntry ne;
	ne.name      = name;
	ne.arrayTex  = 0;
	ne.unit      = AllocUnit();
	ne.mipmap    = mipmap;
	ne.fromSlots = true;
	ne.dirty     = true;
	ne.srcSlots.assign(slots, slots + count);
	m_samplerArrays.push_back(std::move(ne));
}

void MDEI_Shader::BindSamplerArrays()
{
	if (!m_solidShader) return;
	GLuint prog = (GLuint)GPU_shader_program(m_solidShader);

	for (auto &e : m_samplerArrays) {
		/* Retry build every frame until textures are ready */
		if (e.fromSlots && (e.dirty || !e.arrayTex))
			RebuildSamplerArray(e);

		if (!e.arrayTex) continue;

		int loc = GPU_shader_get_uniform(m_solidShader, e.name.c_str());
		if (loc < 0) continue;

		if (GLEW_ARB_bindless_texture && e.bindlessHandle) {
			/* Só envia o uniform se o handle mudou desde o último envio */
			if (e.bindlessHandle != e.boundHandle) {
				glProgramUniformHandleui64ARB(prog, loc, e.bindlessHandle);
				e.boundHandle = e.bindlessHandle;
			}
		}
		else {
			/* Só faz bind e glUniform se a textura ou unidade mudaram */
			if (e.arrayTex != e.boundTex) {
				glActiveTexture(GL_TEXTURE0 + e.unit);
				glBindTexture(GL_TEXTURE_2D_ARRAY, e.arrayTex);
				GPU_shader_uniform_int(m_solidShader, loc, e.unit);
				e.boundTex = e.arrayTex;
			}
		}
	}
}

/* ══════════════════════════════════════════════════════════════════════════
 * Bindless samplers
 * ══════════════════════════════════════════════════════════════════════════ */

void MDEI_Shader::SetSamplerBindless(const char *name, GLuint64 handle)
{
	if (!m_solidShader || !GLEW_ARB_bindless_texture) return;
	int loc = GPU_shader_get_uniform(m_solidShader, name);
	if (loc < 0) return;
	GPU_shader_bind(m_solidShader);
	glUniformHandleui64ARB(loc, handle);
	GPU_shader_unbind();
}

void MDEI_Shader::SetSamplerBindlessFromId(const char *name, GLuint glTexId, bool mipmap)
{
	if (!m_solidShader || !GLEW_ARB_bindless_texture) {
		fprintf(stderr, "[MDEI] SetSamplerBindlessFromId: bindless not available or shader null\n");
		return;
	}

	/* Generate mipmaps if requested */
	if (mipmap) {
		glBindTexture(GL_TEXTURE_2D, glTexId);
		glGenerateMipmap(GL_TEXTURE_2D);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glBindTexture(GL_TEXTURE_2D, 0);
	}

	/* Get / create the bindless handle */
	GLuint64 handle = glGetTextureHandleARB(glTexId);
	if (!glIsTextureHandleResidentARB(handle))
		glMakeTextureHandleResidentARB(handle);

	/* Upload to shader */
	int loc = GPU_shader_get_uniform(m_solidShader, name);
	if (loc >= 0) {
		GPU_shader_bind(m_solidShader);
		glUniformHandleui64ARB(loc, handle);
		GPU_shader_unbind();
	}

	/* Track for cleanup — replace existing entry with the same name */
	for (auto &b : m_bindlessHandles) {
		if (b.name == name) {
			/* Release old handle / texture if owned */
			if (GLEW_ARB_bindless_texture && b.handle &&
			    glIsTextureHandleResidentARB(b.handle))
				glMakeTextureHandleNonResidentARB(b.handle);
			if (b.ownedTex) { glDeleteTextures(1, &b.ownedTex); b.ownedTex = 0; }
			b.handle   = handle;
			b.ownedTex = 0; /* GL id not owned by us */
			return;
		}
	}
	BindlessEntry be;
	be.name      = name;
	be.handle    = handle;
	be.ownedTex  = 0;
	m_bindlessHandles.push_back(std::move(be));
}

void MDEI_Shader::SetSamplerArrayBindless(const char *name,
                                           const GLuint *ids, int count, bool mipmap)
{
	if (!m_solidShader || !GLEW_ARB_bindless_texture) {
		fprintf(stderr, "[MDEI] SetSamplerArrayBindless: bindless not available or shader null\n");
		return;
	}
	if (count <= 0) return;

	/* ── Build / rebuild a GL_TEXTURE_2D_ARRAY ───────────────────────── */
	GLuint arrayTex = 0;

	/* Query first texture for dimensions */
	int w = 0, h = 0;
	glBindTexture(GL_TEXTURE_2D, ids[0]);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,  &w);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
	glBindTexture(GL_TEXTURE_2D, 0);
	if (w <= 0 || h <= 0) {
		fprintf(stderr, "[MDEI] SetSamplerArrayBindless: first texture has invalid size\n");
		return;
	}

	glGenTextures(1, &arrayTex);
	glBindTexture(GL_TEXTURE_2D_ARRAY, arrayTex);
	glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, w, h, count, 0,
	             GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	for (int i = 0; i < count; ++i) {
		glCopyImageSubData(ids[i], GL_TEXTURE_2D, 0, 0, 0, 0,
		                   arrayTex, GL_TEXTURE_2D_ARRAY, 0, 0, 0, i,
		                   w, h, 1);
	}
	if (mipmap) {
		glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	}
	else {
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	}
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	/* Anisotropic: mesmo valor que o RAS usa para texturas Blender. */
	if (GLEW_EXT_texture_filter_anisotropic) {
		glTexParameterf(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_ANISOTROPY_EXT,
		                GPU_get_anisotropic());
	}
	glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

	/* ── Make bindless ───────────────────────────────────────────────── */
	GLuint64 handle = glGetTextureHandleARB(arrayTex);
	if (!glIsTextureHandleResidentARB(handle))
		glMakeTextureHandleResidentARB(handle);

	/* Upload to shader */
	int loc = GPU_shader_get_uniform(m_solidShader, name);
	if (loc >= 0) {
		GPU_shader_bind(m_solidShader);
		glUniformHandleui64ARB(loc, handle);
		GPU_shader_unbind();
	}

	/* Track: replace existing entry or create new */
	for (auto &b : m_bindlessHandles) {
		if (b.name == name) {
			if (b.handle && glIsTextureHandleResidentARB(b.handle))
				glMakeTextureHandleNonResidentARB(b.handle);
			if (b.ownedTex) glDeleteTextures(1, &b.ownedTex);
			b.handle   = handle;
			b.ownedTex = arrayTex;
			return;
		}
	}
	BindlessEntry be;
	be.name     = name;
	be.handle   = handle;
	be.ownedTex = arrayTex; /* we own this GL_TEXTURE_2D_ARRAY */
	m_bindlessHandles.push_back(std::move(be));
}

/* ── SetSamplerArrayBindlessFromSlots ────────────────────────────────────── */
void MDEI_Shader::SetSamplerArrayBindlessFromSlots(const char *name,
                                                    const int *slots, int count,
                                                    bool mipmap)
{
	if (!m_solidShader || !GLEW_ARB_bindless_texture) {
		fprintf(stderr, "[MDEI] SetSamplerArrayBindlessFromSlots: bindless not available or shader null\n");
		return;
	}
	if (count <= 0) return;

	/* Resolve slots → GL ids via GPUPass inputs (same as ResolveSlotsThroughPass) */
	std::vector<GPUInput*> tmp; /* unused but keeps the call consistent */
	std::vector<GLuint> ids;
	if (!ResolveSlotsThroughPass(m_gpuMat, std::vector<int>(slots, slots + count), ids)) {
		fprintf(stderr, "[MDEI] SetSamplerArrayBindlessFromSlots '%s': textures not ready yet\n", name);
		return;
	}

	/* Delegate to the raw-ids bindless path */
	SetSamplerArrayBindless(name, ids.data(), (int)ids.size(), mipmap);
}

void MDEI_Shader::SetSamplerArrayFromGPUTextures(const char *name,
                                                  GPUTexture *const *textures,
                                                  int count, bool mipmap)
{
	for (auto &e : m_samplerArrays) {
		if (e.name == name) {
			e.srcGpu.assign(textures, textures + count);
			e.fromGpu = true;
			e.mipmap  = mipmap;
			RebuildSamplerArray(e);
			return;
		}
	}
	SamplerArrayEntry e;
	e.name    = name;
	e.arrayTex= 0;
	e.unit    = AllocUnit();
	e.mipmap  = mipmap;
	e.fromGpu = true;
	e.srcGpu.assign(textures, textures + count);
	RebuildSamplerArray(e);
	m_samplerArrays.push_back(std::move(e));
}

/* ══════════════════════════════════════════════════════════════════════════
 * Mipmap control
 * ══════════════════════════════════════════════════════════════════════════ */

void MDEI_Shader::SetMipmapping(bool enabled, int glFilterType)
{
	for (auto &e : m_samplerArrays) {
		e.mipmap = enabled;
		e.srcIds.clear(); /* força recriação mesmo com fontes iguais */
		e.dirty = true;
		RebuildSamplerArray(e);
		/* Se o caller passou um filtro GL explícito, aplica-o por cima do
		 * recalculado por RebuildSamplerArray (mantém compatibilidade com
		 * UpdateMipmappingFilter que usa GL constants diretamente). */
		if (glFilterType > 0 && e.arrayTex) {
			glBindTexture(GL_TEXTURE_2D_ARRAY, e.arrayTex);
			glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, glFilterType);
			glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
		}
	}
}

void MDEI_Shader::RebuildAllSamplerArrays()
{
	/* Força recriação de cada entry mantendo o estado mipmap atual.
	 * RebuildSamplerArray lerá GPU_get_anisotropic() ao recriar o
	 * GL_TEXTURE_2D_ARRAY, aplicando o novo nível de anisotropia. */
	for (auto &e : m_samplerArrays) {
		/* Reseta srcIds para forçar recriação mesmo se as fontes não mudaram */
		e.srcIds.clear();
		e.dirty = true;
		RebuildSamplerArray(e);
	}
}

void MDEI_Shader::UpdateMipmappingFilter(int glFilterType, int slot)
{
	for (int i = 0; i < (int)m_samplerArrays.size(); ++i) {
		if (slot >= 0 && i != slot) continue;
		if (!m_samplerArrays[i].arrayTex) continue;
		glBindTexture(GL_TEXTURE_2D_ARRAY, m_samplerArrays[i].arrayTex);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, glFilterType);
		glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
	}
}
