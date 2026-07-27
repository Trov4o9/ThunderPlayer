/*
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
 * The Original Code is Copyright (C) 2005 Blender Foundation.
 * All rights reserved.
 */

/** \file blender/gpu/intern/gpu_codegen.c
 *  \ingroup gpu
 *
 * Convert material node-trees to GLSL.
 */

#include "MEM_guardedalloc.h"
#include "DNA_text_types.h"
#include "../blenkernel/BKE_main.h" 

#include "DNA_customdata_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"

#include "BLI_blenlib.h"
#include "BLI_utildefines.h"
#include "BLI_dynstr.h"
#include "BLI_ghash.h"
#include "BLI_string.h" 

#include "GPU_extensions.h"
#include "GPU_glew.h"
#include "GPU_material.h"
#include "GPU_shader.h"
#include "GPU_texture.h"

#include "BLI_sys_types.h" /* for intptr_t support */

#include "gpu_codegen.h"

#include <string.h>

/* Function to parse custom shader and extract code sections */
void GPU_parse_custom_shader(
    const char *custom_shader,
    char **out_global_code,
    char **out_vertex_code,
    char **out_fragment_code)
{
	*out_global_code = NULL;
	*out_vertex_code = NULL;
	if (out_fragment_code) *out_fragment_code = NULL;
	
	if (!custom_shader || strlen(custom_shader) == 0)
		return;
	
	/* Extract vertex function */
	const char *vertex_start = strstr(custom_shader, "void vertex");
	if (vertex_start) {
		const char *brace_start = strchr(vertex_start, '{');
		if (brace_start) {
			/* Find matching closing brace */
			int brace_count = 1;
			const char *brace_end = brace_start + 1;
			while (*brace_end && brace_count > 0) {
				if (*brace_end == '{') brace_count++;
				else if (*brace_end == '}') brace_count--;
				brace_end++;
			}
			
			if (brace_count == 0) {
				/* Extract vertex code */
				size_t code_len = (brace_end - brace_start) - 2;
				*out_vertex_code = MEM_mallocN(code_len + 1, "custom_vertex");
				memcpy(*out_vertex_code, brace_start + 1, code_len);
				(*out_vertex_code)[code_len] = '\0';
				
				/* Extract global code (before vertex function) */
				size_t global_len = vertex_start - custom_shader;
				if (global_len > 0) {
					*out_global_code = MEM_mallocN(global_len + 1, "custom_global");
					memcpy(*out_global_code, custom_shader, global_len);
					(*out_global_code)[global_len] = '\0';
				}
			}
		}
	}
	
	/* TODO: Fragment parsing if needed in the future */
}
#include <stdarg.h>

#include "../../../gameengine/Rasterizer/RAS_RenderingFlags.h"

#ifdef WIN32
#include <windows.h>
#endif

#include "gpu_lighting_config.h"

extern char datatoc_gpu_shader_material_glsl[];
extern char datatoc_gpu_shader_ubo_lighting_glsl[];
extern const char *gpu_shader_atmospheric_scattering_glsl;
extern char datatoc_gpu_shader_vertex_glsl[];
extern char datatoc_gpu_shader_vertex_world_glsl[];
extern char datatoc_gpu_shader_geometry_glsl[];

static char *glsl_material_library = NULL;

/* type definitions and constants */

enum {
	MAX_FUNCTION_NAME = 64,
};
enum {
	MAX_PARAMETER = 32,
};

typedef enum {
	FUNCTION_QUAL_IN,
	FUNCTION_QUAL_OUT,
	FUNCTION_QUAL_INOUT
} GPUFunctionQual;

typedef struct GPUFunction {
	char name[MAX_FUNCTION_NAME];
	GPUType paramtype[MAX_PARAMETER];
	GPUFunctionQual paramqual[MAX_PARAMETER];
	int totparam;
} GPUFunction;

/* Indices match the GPUType enum */
static const char *GPU_DATATYPE_STR[18] = {
	"", "float", "vec2", "vec3", "vec4",
	NULL, NULL, NULL, NULL, "mat3", NULL, NULL, NULL, NULL, NULL, NULL, "mat4", "int"
};
/* Indices match the GPUType size */
static const unsigned int GPU_DATATYPE_SIZE[18] = {
	0, 1, 2, 3, 4, 0, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 16, 1
};

/* GLSL code parsing for finding function definitions.
 * These are stored in a hash for lookup when creating a material. */

static GHash *FUNCTION_HASH = NULL;
#if 0
static char *FUNCTION_PROTOTYPES = NULL;
static GPUShader *FUNCTION_LIB = NULL;
#endif

static int gpu_str_prefix(const char *str, const char *prefix)
{
	while (*str && *prefix) {
		if (*str != *prefix)
			return 0;

		str++;
		prefix++;
	}

	return (*prefix == '\0');
}

static char *gpu_str_skip_token(char *str, char *token, int max)
{
	int len = 0;

	/* skip a variable/function name */
	while (*str) {
		if (ELEM(*str, ' ', '(', ')', ',', '\t', '\n', '\r'))
			break;
		else {
			if (token && len < max - 1) {
				*token = *str;
				token++;
				len++;
			}
			str++;
		}
	}

	if (token)
		*token = '\0';

	/* skip the next special characters:
	 * note the missing ')' */
	while (*str) {
		if (ELEM(*str, ' ', '(', ',', '\t', '\n', '\r'))
			str++;
		else
			break;
	}

	return str;
}

static void gpu_parse_functions_string(GHash *hash, char *code)
{
	GPUFunction *function;
	GPUType type;
	GPUFunctionQual qual;
	int i;

	while ((code = strstr(code, "void "))) {
		function = MEM_callocN(sizeof(GPUFunction), "GPUFunction");

		code = gpu_str_skip_token(code, NULL, 0);
		code = gpu_str_skip_token(code, function->name, MAX_FUNCTION_NAME);

		/* get parameters */
		while (*code && *code != ')') {
			/* test if it's an input or output */
			qual = FUNCTION_QUAL_IN;
			if (gpu_str_prefix(code, "out "))
				qual = FUNCTION_QUAL_OUT;
			if (gpu_str_prefix(code, "inout "))
				qual = FUNCTION_QUAL_INOUT;
			if ((qual != FUNCTION_QUAL_IN) || gpu_str_prefix(code, "in "))
				code = gpu_str_skip_token(code, NULL, 0);

			/* test for type */
			type = GPU_NONE;
			for (i = 1; i < ARRAY_SIZE(GPU_DATATYPE_STR); i++) {
				if (GPU_DATATYPE_STR[i] && gpu_str_prefix(code, GPU_DATATYPE_STR[i])) {
					type = i;
					break;
				}
			}

			if (!type && gpu_str_prefix(code, "samplerCube")) {
				type = GPU_TEXCUBE;
			}
			if (!type && gpu_str_prefix(code, "sampler2DShadow")) {
				type = GPU_SHADOW2D;
			}
			if (!type && gpu_str_prefix(code, "sampler2D")) {
				type = GPU_TEX2D;
			}

			if (type) {
				/* add parameter */
				code = gpu_str_skip_token(code, NULL, 0);
				code = gpu_str_skip_token(code, NULL, 0);
				function->paramqual[function->totparam] = qual;
				function->paramtype[function->totparam] = type;
				function->totparam++;
			}
			else {
				fprintf(stderr, "GPU invalid function parameter in %s.\n", function->name);
				break;
			}
		}

		if (function->name[0] == '\0' || function->totparam == 0) {
			fprintf(stderr, "GPU functions parse error.\n");
			MEM_freeN(function);
			break;
		}

		BLI_ghash_insert(hash, function->name, function);
	}
}

#if 0
static char *gpu_generate_function_prototyps(GHash *hash)
{
	DynStr *ds = BLI_dynstr_new();
	GHashIterator *ghi;
	GPUFunction *function;
	char *name, *prototypes;
	int a;

	/* automatically generate function prototypes to add to the top of the
	 * generated code, to avoid have to add the actual code & recompile all */
	ghi = BLI_ghashIterator_new(hash);

	for (; !BLI_ghashIterator_done(ghi); BLI_ghashIterator_step(ghi)) {
		name = BLI_ghashIterator_getValue(ghi);
		function = BLI_ghashIterator_getValue(ghi);

		BLI_dynstr_appendf(ds, "void %s(", name);
		for (a = 0; a < function->totparam; a++) {
			if (function->paramqual[a] == FUNCTION_QUAL_OUT)
				BLI_dynstr_append(ds, "out ");
			else if (function->paramqual[a] == FUNCTION_QUAL_INOUT)
				BLI_dynstr_append(ds, "inout ");

			if (function->paramtype[a] == GPU_TEX2D)
				BLI_dynstr_append(ds, "sampler2D");
			else if (function->paramtype[a] == GPU_SHADOW2D)
				BLI_dynstr_append(ds, "sampler2DShadow");
			else
				BLI_dynstr_append(ds, GPU_DATATYPE_STR[function->paramtype[a]]);
#  if 0
			BLI_dynstr_appendf(ds, " param%d", a);
#  endif

			if (a != function->totparam - 1)
				BLI_dynstr_append(ds, ", ");
		}
		BLI_dynstr_append(ds, ");\n");
	}

	BLI_dynstr_append(ds, "\n");

	prototypes = BLI_dynstr_get_cstring(ds);
	BLI_dynstr_free(ds);

	return prototypes;
}
#endif

static GPUFunction *gpu_lookup_function(const char *name)
{
	if (!FUNCTION_HASH) {
		FUNCTION_HASH = BLI_ghash_str_new("GPU_lookup_function gh");
		gpu_parse_functions_string(FUNCTION_HASH, glsl_material_library);
	}

	return BLI_ghash_lookup(FUNCTION_HASH, (const void *)name);
}

void gpu_codegen_init(void)
{
	GPU_code_generate_glsl_lib();
}

void gpu_codegen_exit(void)
{
	extern Material defmaterial; /* render module abuse... */

	if (defmaterial.gpumaterial.first)
		GPU_material_free(&defmaterial.gpumaterial);

	if (FUNCTION_HASH) {
		BLI_ghash_free(FUNCTION_HASH, NULL, MEM_freeN);
		FUNCTION_HASH = NULL;
	}

	GPU_shader_free_builtin_shaders();

	if (glsl_material_library) {
		MEM_freeN(glsl_material_library);
		glsl_material_library = NULL;
	}

#if 0
	if (FUNCTION_PROTOTYPES) {
		MEM_freeN(FUNCTION_PROTOTYPES);
		FUNCTION_PROTOTYPES = NULL;
	}
	if (FUNCTION_LIB) {
		GPU_shader_free(FUNCTION_LIB);
		FUNCTION_LIB = NULL;
	}
#endif
}

/* GLSL code generation */

static void codegen_convert_datatype(DynStr *ds, int from, int to, const char *tmp, int id)
{
	char name[1024];

	BLI_snprintf(name, sizeof(name), "%s%d", tmp, id);

	if (from == to) {
		BLI_dynstr_append(ds, name);
	}
	else if (to == GPU_FLOAT) {
		if (from == GPU_VEC4)
			BLI_dynstr_appendf(ds, "convert_rgba_to_float(%s)", name);
		else if (from == GPU_VEC3)
			BLI_dynstr_appendf(ds, "(%s.r + %s.g + %s.b) / 3.0", name, name, name);
		else if (from == GPU_VEC2)
			BLI_dynstr_appendf(ds, "%s.r", name);
	}
	else if (to == GPU_VEC2) {
		if (from == GPU_VEC4)
			BLI_dynstr_appendf(ds, "vec2((%s.r + %s.g + %s.b) / 3.0, %s.a)", name, name, name, name);
		else if (from == GPU_VEC3)
			BLI_dynstr_appendf(ds, "vec2((%s.r + %s.g + %s.b) / 3.0, 1.0)", name, name, name);
		else if (from == GPU_FLOAT)
			BLI_dynstr_appendf(ds, "vec2(%s, 1.0)", name);
	}
	else if (to == GPU_VEC3) {
		if (from == GPU_VEC4)
			BLI_dynstr_appendf(ds, "%s.rgb", name);
		else if (from == GPU_VEC2)
			BLI_dynstr_appendf(ds, "vec3(%s.r, %s.r, %s.r)", name, name, name);
		else if (from == GPU_FLOAT)
			BLI_dynstr_appendf(ds, "vec3(%s, %s, %s)", name, name, name);
	}
	else {
		if (from == GPU_VEC3)
			BLI_dynstr_appendf(ds, "vec4(%s, 1.0)", name);
		else if (from == GPU_VEC2)
			BLI_dynstr_appendf(ds, "vec4(%s.r, %s.r, %s.r, %s.g)", name, name, name, name);
		else if (from == GPU_FLOAT)
			BLI_dynstr_appendf(ds, "vec4(%s, %s, %s, 1.0)", name, name, name);
	}
}

static void codegen_print_datatype(DynStr *ds, const GPUType type, float *data)
{
	int i;

	BLI_dynstr_appendf(ds, "%s(", GPU_DATATYPE_STR[type]);

	for (i = 0; i < GPU_DATATYPE_SIZE[type]; i++) {
		BLI_dynstr_appendf(ds, "%.12f", data[i]);
		if (i == type - 1)
			BLI_dynstr_append(ds, ")");
		else
			BLI_dynstr_append(ds, ", ");
	}
}

static int codegen_input_has_texture(GPUInput *input)
{
	if (input->link)
		return 0;
	else if (input->ima || input->prv)
		return 1;
	else
		return (input->tex != NULL || input->texptr != NULL);
}

const char *GPU_builtin_name(GPUBuiltin builtin)
{
	if (builtin == GPU_VIEW_MATRIX)
		return "unfviewmat";
	else if (builtin == GPU_OBJECT_MATRIX)
		return "unfobmat";
	else if (builtin == GPU_INVERSE_VIEW_MATRIX)
		return "unfinvviewmat";
	else if (builtin == GPU_INVERSE_OBJECT_MATRIX)
		return "unfinvobmat";
	else if (builtin == GPU_LOC_TO_VIEW_MATRIX)
		return "unflocaltoviewmat";
	else if (builtin == GPU_INVERSE_LOC_TO_VIEW_MATRIX)
		return "unfinvlocaltoviewmat";
	else if (builtin == GPU_VIEW_POSITION)
		return "varposition";
	else if (builtin == GPU_VIEW_NORMAL)
		return "varnormal";
	else if (builtin == GPU_OBCOLOR)
		return "unfobcolor";
	else if (builtin == GPU_AUTO_BUMPSCALE)
		return "unfobautobumpscale";
	else if (builtin == GPU_CAMERA_TEXCO_FACTORS)
		return "unfcameratexfactors";
	else if (builtin == GPU_PARTICLE_SCALAR_PROPS)
		return "unfparticlescalarprops";
	else if (builtin == GPU_PARTICLE_LOCATION)
		return "unfparticleco";
	else if (builtin == GPU_PARTICLE_VELOCITY)
		return "unfparticlevel";
	else if (builtin == GPU_PARTICLE_ANG_VELOCITY)
		return "unfparticleangvel";
	else if (builtin == GPU_INSTANCING_MATRIX)
		return "varinstmat";
	else if (builtin == GPU_INSTANCING_INVERSE_MATRIX)
		return "varinstinvmat";
	else if (builtin == GPU_INSTANCING_COLOR)
		return "varinstcolor";
	else if (builtin == GPU_INSTANCING_LAYER)
		return "varinstlayer";
	else if (builtin == GPU_INSTANCING_INFO)
		return "varinstinfo";
	else if (builtin == GPU_INSTANCING_COLOR_ATTRIB)
		return "ininstcolor";
	else if (builtin == GPU_INSTANCING_MATRIX_ATTRIB)
		return "ininstmatrix";
	else if (builtin == GPU_INSTANCING_POSITION_ATTRIB)
		return "ininstposition";
	else if (builtin == GPU_INSTANCING_LAYER_ATTRIB)
		return "ininstlayer";
	else if (builtin == GPU_INSTANCING_INFO_ATTRIB)
		return "ininstinfo";
	else if (builtin == GPU_TIME)
		return "unftime";
	else if (builtin == GPU_OBJECT_INFO)
		return "unfobjectinfo";
	else if (builtin == GPU_OBJECT_LAY)
		return "unfobjectlay";
	else
		return "";
}

/* assign only one texid per buffer to avoid sampling the same texture twice */
static void codegen_set_texid(GHash *bindhash, GPUInput *input, int *texid, void *key)
{
	if (BLI_ghash_haskey(bindhash, key)) {
		/* Reuse existing texid */
		input->texid = POINTER_AS_INT(BLI_ghash_lookup(bindhash, key));
	}
	else {
		/* Allocate new texid */
		input->texid = *texid;
		(*texid)++;
		input->bindtex = true;
		BLI_ghash_insert(bindhash, key, POINTER_FROM_INT(input->texid));
	}
}

static void codegen_set_unique_ids(ListBase *nodes)
{
	GHash *bindhash, *definehash;
	GPUNode *node;
	GPUInput *input;
	GPUOutput *output;
	int id = 1, texid = 0;

	bindhash = BLI_ghash_ptr_new("codegen_set_unique_ids1 gh");
	definehash = BLI_ghash_ptr_new("codegen_set_unique_ids2 gh");

	for (node = nodes->first; node; node = node->next) {
		for (input = node->inputs.first; input; input = input->next) {
			/* set id for unique names of uniform variables */
			input->id = id++;
			input->bindtex = false;
			input->definetex = false;

			/* set texid used for settings texture slot with multitexture */
			if (codegen_input_has_texture(input) &&
			    ((input->source == GPU_SOURCE_TEX) || (input->source == GPU_SOURCE_TEX_PIXEL)))
			{
				/* assign only one texid per buffer to avoid sampling
				 * the same texture twice */
				if (input->link) {
					/* input is texture from buffer */
					codegen_set_texid(bindhash, input, &texid, input->link);
				}
				else if (input->ima) {
					/* input is texture from image */
					codegen_set_texid(bindhash, input, &texid, input->ima);
				}
				else if (input->prv) {
					/* input is texture from preview render */
					codegen_set_texid(bindhash, input, &texid, input->prv);
				}
				else if (input->tex) {
					/* input is user created texture, check tex pointer */
					codegen_set_texid(bindhash, input, &texid, input->tex);
				}
				else if (input->texptr) {
					/* input is user created texture, check tex pointer */
					codegen_set_texid(bindhash, input, &texid, input->texptr);
				}

				/* make sure this pixel is defined exactly once */
				if (input->source == GPU_SOURCE_TEX_PIXEL) {
					if (input->ima) {
						if (!BLI_ghash_haskey(definehash, input->ima)) {
							input->definetex = true;
							BLI_ghash_insert(definehash, input->ima, POINTER_FROM_INT(input->texid));
						}
					}
					else {
						if (!BLI_ghash_haskey(definehash, input->link)) {
							input->definetex = true;
							BLI_ghash_insert(definehash, input->link, POINTER_FROM_INT(input->texid));
						}
					}
				}
			}
		}

		for (output = node->outputs.first; output; output = output->next)
			/* set id for unique names of tmp variables storing output */
			output->id = id++;
	}

	BLI_ghash_free(bindhash, NULL, NULL);
	BLI_ghash_free(definehash, NULL, NULL);
}

/* Structure to track material property IDs for custom shader injection */
typedef struct MaterialPropertyIDs {
	int albedo_id;           /* cons ID for diffuse RGB (albedo) */
	int specrgb_id;          /* cons ID for specular RGB */
	int alpha_id;            /* cons ID for alpha */
	int emit_id;             /* cons ID for emit value */
	int roughness_id;        /* cons ID for roughness (roughness_bsdf) */
	int metallic_id;         /* cons ID for metallic (metallic_bsdf) */
	int spec_id;             /* cons ID for specular intensity */
	int ubo_speccolor_id;    /* cons ID for UBO specular F0 colour (vec3) */
	int ubo_specstrength_id; /* cons ID for UBO specular strength (float) */
	int ubo_scattercolor_id; /* cons ID for UBO scatter tint colour (vec3) */
	int ubo_scatterfac_id;   /* cons ID for UBO scatter factor (float) */
	/* World/sky shader IDs */
	int world_horizon_id;    /* cons ID for GPU_DYNAMIC_HORIZON_COLOR  → HORIZON_COLOR */
	int world_zenith_id;     /* cons ID for GPU_DYNAMIC_ZENITH_COLOR   → ZENITH_COLOR  */
	int world_envlight_id;   /* cons ID for GPU_DYNAMIC_ENVLIGHT_ENERGY → ENV_ENERGY   */
} MaterialPropertyIDs;

static int codegen_print_uniforms_functions(DynStr *ds, ListBase *nodes, bool use_custom_fragment, MaterialPropertyIDs *mat_ids)
{
	GPUNode *node;
	GPUInput *input;
	const char *name;
	int builtins = 0;
	
	/* Initialize material property IDs to -1 (not found) */
	if (mat_ids) {
		mat_ids->albedo_id = -1;
		mat_ids->specrgb_id = -1;
		mat_ids->alpha_id = -1;
		mat_ids->emit_id = -1;
		mat_ids->roughness_id = -1;
		mat_ids->metallic_id = -1;
		mat_ids->spec_id = -1;
		mat_ids->ubo_speccolor_id = -1;
		mat_ids->ubo_specstrength_id = -1;
		mat_ids->ubo_scattercolor_id = -1;
		mat_ids->ubo_scatterfac_id = -1;
		mat_ids->world_horizon_id = -1;
		mat_ids->world_zenith_id  = -1;
		mat_ids->world_envlight_id = -1;
	}

	/* print uniforms */
	for (node = nodes->first; node; node = node->next) {
		for (input = node->inputs.first; input; input = input->next) {
			if ((input->source == GPU_SOURCE_TEX) || (input->source == GPU_SOURCE_TEX_PIXEL)) {
				/* create exactly one sampler for each texture */
				if (codegen_input_has_texture(input) && input->bindtex) {
					BLI_dynstr_appendf(ds, "uniform %s samp%d;\n",
						(input->textype == GPU_TEX2D) ? "sampler2D" :
						(input->textype == GPU_TEXCUBE) ? "samplerCube" : "sampler2DShadow",
						input->texid);
				}
			}
			else if (input->source == GPU_SOURCE_BUILTIN) {
				/* only define each builtin uniform/varying once */
				if (!(builtins & input->builtin)) {
					builtins |= input->builtin;
					name = GPU_builtin_name(input->builtin);

					if (gpu_str_prefix(name, "unf")) {
						BLI_dynstr_appendf(ds, "uniform %s %s;\n",
							GPU_DATATYPE_STR[input->type], name);
					}
					else {
						// GPU_INSTANCING_LAYER is an integer, it must be flat in GLSL.
						if (input->builtin == GPU_INSTANCING_LAYER) {
							BLI_dynstr_appendf(ds, "%s %s %s;\n",
								GLEW_VERSION_3_0 ? "flat in" : "flat varying",
								GPU_DATATYPE_STR[input->type], name);
						}
						else {
							BLI_dynstr_appendf(ds, "%s %s %s;\n",
								GLEW_VERSION_3_0 ? "in" : "varying",
								GPU_DATATYPE_STR[input->type], name);
						}
					}
				}
			}
			else if (input->source == GPU_SOURCE_VEC_UNIFORM) {
				if (input->dynamicvec) {
					/* only create uniforms for dynamic vectors */
					BLI_dynstr_appendf(ds, "uniform %s unf%d;\n",
						GPU_DATATYPE_STR[input->type], input->id);
	
					/* Track dynamic material property IDs for custom shader system */
					if (mat_ids) {
						if (input->dynamictype == GPU_DYNAMIC_MAT_DIFFRGB)
							mat_ids->albedo_id = input->id;
						else if (input->dynamictype == GPU_DYNAMIC_MAT_SPECRGB)
							mat_ids->specrgb_id = input->id;
						else if (input->dynamictype == GPU_DYNAMIC_MAT_ALPHA)
							mat_ids->alpha_id = input->id;
						else if (input->dynamictype == GPU_DYNAMIC_MAT_EMIT)
							mat_ids->emit_id = input->id;
						else if (input->dynamictype == GPU_DYNAMIC_MAT_ROUGHNESS)
							mat_ids->roughness_id = input->id;
						else if (input->dynamictype == GPU_DYNAMIC_MAT_METALLIC)
							mat_ids->metallic_id = input->id;
						else if (input->dynamictype == GPU_DYNAMIC_MAT_SPEC)
							mat_ids->spec_id = input->id;
						else if (input->dynamictype == GPU_DYNAMIC_MAT_UBO_SPECCOLOR)
							mat_ids->ubo_speccolor_id = input->id;
						else if (input->dynamictype == GPU_DYNAMIC_MAT_UBO_SPECSTRENGTH)
							mat_ids->ubo_specstrength_id = input->id;
						else if (input->dynamictype == GPU_DYNAMIC_MAT_UBO_SCATTERCOLOR)
							mat_ids->ubo_scattercolor_id = input->id;
						else if (input->dynamictype == GPU_DYNAMIC_MAT_UBO_SCATTERFAC)
							mat_ids->ubo_scatterfac_id = input->id;
						/* World dynamic uniforms */
						else if (input->dynamictype == GPU_DYNAMIC_HORIZON_COLOR)
							mat_ids->world_horizon_id = input->id;
						else if (input->dynamictype == GPU_DYNAMIC_ZENITH_COLOR)
							mat_ids->world_zenith_id = input->id;
						else if (input->dynamictype == GPU_DYNAMIC_ENVLIGHT_ENERGY)
							mat_ids->world_envlight_id = input->id;
					}
				}
				else {
					/* Track material property IDs for CONST values too (not just dynamic uniforms) */
					if (mat_ids) {
						if (input->dynamictype == GPU_DYNAMIC_MAT_DIFFRGB)
							mat_ids->albedo_id = input->id;
						else if (input->dynamictype == GPU_DYNAMIC_MAT_SPECRGB)
							mat_ids->specrgb_id = input->id;
						else if (input->dynamictype == GPU_DYNAMIC_MAT_ALPHA)
							mat_ids->alpha_id = input->id;
						else if (input->dynamictype == GPU_DYNAMIC_MAT_EMIT)
							mat_ids->emit_id = input->id;
						else if (input->dynamictype == GPU_DYNAMIC_MAT_ROUGHNESS)
							mat_ids->roughness_id = input->id;
						else if (input->dynamictype == GPU_DYNAMIC_MAT_METALLIC)
							mat_ids->metallic_id = input->id;
						else if (input->dynamictype == GPU_DYNAMIC_MAT_SPEC)
							mat_ids->spec_id = input->id;
						else if (input->dynamictype == GPU_DYNAMIC_MAT_UBO_SPECCOLOR)
							mat_ids->ubo_speccolor_id = input->id;
						else if (input->dynamictype == GPU_DYNAMIC_MAT_UBO_SPECSTRENGTH)
							mat_ids->ubo_specstrength_id = input->id;
						else if (input->dynamictype == GPU_DYNAMIC_MAT_UBO_SCATTERCOLOR)
							mat_ids->ubo_scattercolor_id = input->id;
						else if (input->dynamictype == GPU_DYNAMIC_MAT_UBO_SCATTERFAC)
							mat_ids->ubo_scatterfac_id = input->id;
						/* World const uniforms */
						else if (input->dynamictype == GPU_DYNAMIC_HORIZON_COLOR)
							mat_ids->world_horizon_id = input->id;
						else if (input->dynamictype == GPU_DYNAMIC_ZENITH_COLOR)
							mat_ids->world_zenith_id = input->id;
						else if (input->dynamictype == GPU_DYNAMIC_ENVLIGHT_ENERGY)
							mat_ids->world_envlight_id = input->id;
					}
	
					/* Use mutable variables when UBO lighting or custom shader is active
					 * so that ALBEDO/ROUGHNESS/etc. can be written back into the cons
					 * variables after calcLight() runs. Otherwise use const for compiler folding. */
					if (use_custom_fragment) {
						BLI_dynstr_appendf(ds, "%s cons%d = ",
							GPU_DATATYPE_STR[input->type], input->id);
						codegen_print_datatype(ds, input->type, input->vec);
						BLI_dynstr_append(ds, ";\n");
					}
					else {
						/* for others use const so the compiler can do folding */
						BLI_dynstr_appendf(ds, "const %s cons%d = ",
							GPU_DATATYPE_STR[input->type], input->id);
						codegen_print_datatype(ds, input->type, input->vec);
						BLI_dynstr_append(ds, ";\n");
					}
				}
			}
			else if (input->source == GPU_SOURCE_ATTRIB && input->attribfirst) {
#ifdef WITH_OPENSUBDIV
				bool skip_opensubdiv = input->attribtype == CD_TANGENT;
				if (skip_opensubdiv) {
					BLI_dynstr_appendf(ds, "#ifndef USE_OPENSUBDIV\n");
				}
#endif
				BLI_dynstr_appendf(ds, "%s %s var%d;\n",
					GLEW_VERSION_3_0 ? "in" : "varying",
					GPU_DATATYPE_STR[input->type], input->attribid);
#ifdef WITH_OPENSUBDIV
				if (skip_opensubdiv) {
					BLI_dynstr_appendf(ds, "#endif\n");
				}
#endif
			}
		}
	}

	BLI_dynstr_append(ds, "\n");
	
	/* Add SSBO declarations for persistent instancing system */
	if (USE_PERSISTENT_SSBO_RENDERING) {
		BLI_dynstr_append(ds,
			"#define USE_PERSISTENT_SSBO 1\n"
			"\n"
			"struct InstanceData {\n"
			"    mat4 modelMatrix;\n"
			"    mat4 normalMatrix;\n"
			"    vec4 color;\n"
			"    uvec4 info;\n"
			"};\n"
			"\n"
			"layout(std430, binding=3) readonly buffer InstanceDataBlock {\n"
			"    InstanceData instances[];\n"
			"};\n"
			"\n");
	}

	return builtins;
}

static void codegen_declare_tmps(DynStr *ds, ListBase *nodes)
{
	GPUNode *node;
	GPUInput *input;
	GPUOutput *output;

	for (node = nodes->first; node; node = node->next) {
		/* load pixels from textures */
		for (input = node->inputs.first; input; input = input->next) {
			if (input->source == GPU_SOURCE_TEX_PIXEL) {
				if (codegen_input_has_texture(input) && input->definetex) {
					BLI_dynstr_appendf(ds, "\tvec4 tex%d = texture2D(", input->texid);
					BLI_dynstr_appendf(ds, "samp%d, gl_TexCoord[%d].st);\n",
					                   input->texid, input->texid);
				}
			}
		}

		/* declare temporary variables for node output storage */
		for (output = node->outputs.first; output; output = output->next) {
			BLI_dynstr_appendf(ds, "\t%s tmp%d;\n",
			                   GPU_DATATYPE_STR[output->type], output->id);
		}
	}

	BLI_dynstr_append(ds, "\n");
}

static void codegen_call_functions(DynStr *ds, ListBase *nodes, GPUNodeLink *finaloutputs[8], MaterialPropertyIDs *mat_ids, bool sky_preprocess_cons)
{
	GPUNode *node;
	GPUInput *input;
	GPUOutput *output;

	for (node = nodes->first; node; node = node->next) {
		if (strcmp(node->name, "ubo_lighting_apply") == 0) {
			output = node->outputs.first;
			input = node->inputs.first;
			/* The diff input may be vec3 (set_rgb_zero path) or vec4 (shade_mul_emit_value
			 * path when emit > 0).  Always extract .rgb so the assignment to the vec3
			 * output tmp is type-safe regardless of the upstream node's output type. */
			if (input->link->output->type == GPU_VEC4) {
				BLI_dynstr_appendf(ds, "\ttmp%d = tmp%d.rgb + ubo_result;\n",
				                   output->id, input->link->output->id);
			}
			else {
				BLI_dynstr_appendf(ds, "\ttmp%d = tmp%d + ubo_result;\n",
				                   output->id, input->link->output->id);
			}
			continue;
		}

		BLI_dynstr_appendf(ds, "\t%s(", node->name);

		for (input = node->inputs.first; input; input = input->next) {
			if (input->source == GPU_SOURCE_TEX) {
				BLI_dynstr_appendf(ds, "samp%d", input->texid);
				if (input->link)
					BLI_dynstr_appendf(ds, ", gl_TexCoord[%d].st", input->texid);
			}
			else if (input->source == GPU_SOURCE_TEX_PIXEL) {
				codegen_convert_datatype(ds, input->link->output->type, input->type,
					"tmp", input->link->output->id);
			}
			else if (input->source == GPU_SOURCE_BUILTIN) {
				if (input->builtin == GPU_VIEW_NORMAL)
					BLI_dynstr_append(ds, "facingnormal");
				else
					BLI_dynstr_append(ds, GPU_builtin_name(input->builtin));
			}
			else if (input->source == GPU_SOURCE_VEC_UNIFORM) {
				if (input->dynamicvec) {
						/* Only route through cons{N} when the pre-process sky path
						 * explicitly declared those local aliases above.  Without a
						 * custom sky (or in override mode, which early-returns) the
						 * cons{N} variables do not exist and must not be referenced. */
						if (sky_preprocess_cons && mat_ids &&
						    (input->id == mat_ids->world_horizon_id ||
						     input->id == mat_ids->world_zenith_id  ||
						     input->id == mat_ids->world_envlight_id))
						{
							BLI_dynstr_appendf(ds, "cons%d", input->id);
						}
						else {
							BLI_dynstr_appendf(ds, "unf%d", input->id);
						}
					}
				else
					BLI_dynstr_appendf(ds, "cons%d", input->id);
			}
			else if (input->source == GPU_SOURCE_ATTRIB) {
				BLI_dynstr_appendf(ds, "var%d", input->attribid);
			}
			else if (input->source == GPU_SOURCE_OPENGL_BUILTIN) {
				if (input->oglbuiltin == GPU_MATCAP_NORMAL)
					BLI_dynstr_append(ds, "gl_SecondaryColor");
				else if (input->oglbuiltin == GPU_COLOR)
					BLI_dynstr_append(ds, "gl_Color");
			}

			BLI_dynstr_append(ds, ", ");
		}

		for (output = node->outputs.first; output; output = output->next) {
			BLI_dynstr_appendf(ds, "tmp%d", output->id);
			if (output->next)
				BLI_dynstr_append(ds, ", ");
		}

		BLI_dynstr_append(ds, ");\n");
	}

	for (unsigned short i = 0; i < 8; ++i) {
		if (finaloutputs[i]) {
			output = finaloutputs[i]->output;
			BLI_dynstr_appendf(ds, "\n\tgl_FragData[%i] = ", i);
			codegen_convert_datatype(ds, output->type, GPU_VEC4, "tmp", output->id);
			BLI_dynstr_append(ds, ";\n");
		}
	}
}
bool g_useDeferred_GGG = false;
static char *code_generate_fragment(ListBase *nodes, GPUNodeLink *outputs[8], const char *custom_shader, const bool use_ubo_lighting, const bool is_world, const bool custom_sky_override)
{
    DynStr *ds = BLI_dynstr_new();
    char *code;
    int builtins;
    bool has_custom_fragment = (custom_shader && strlen(custom_shader) > 0);
    bool sky_preprocess_cons = false;  /* true only in sky pre-process path */
    MaterialPropertyIDs mat_ids;

#ifdef WITH_OPENSUBDIV
    GPUNode *node;
    GPUInput *input;
#endif

    codegen_set_unique_ids(nodes);
    builtins = codegen_print_uniforms_functions(ds, nodes, (has_custom_fragment || use_ubo_lighting), &mat_ids);

    BLI_dynstr_append(ds, "uniform vec3 u_MatColor;\n");
    BLI_dynstr_append(ds, "uniform float u_MatAlpha;\n");
    BLI_dynstr_append(ds, "uniform float u_MatSpec;\n");
    BLI_dynstr_append(ds, "uniform float u_MatRoughness;\n");
    BLI_dynstr_append(ds, "uniform float u_MatMetallic;\n");
    BLI_dynstr_append(ds, "uniform float u_MatEmission;\n");
    
    /* Process custom fragment shader */
    char *custom_global_frag = NULL;
    char *custom_fragment_code = NULL;
    
    if (custom_shader && strlen(custom_shader) > 0) {
        /* Find the fragment function */
        const char *fragment_start = strstr(custom_shader, "void fragment");
        if (fragment_start) {
            /* Find the opening brace */
            const char *brace_start = strchr(fragment_start, '{');
            if (brace_start) {
                /* Find the matching closing brace */
                int brace_count = 1;
                const char *brace_end = brace_start + 1;
                while (*brace_end && brace_count > 0) {
                    if (*brace_end == '{') brace_count++;
                    else if (*brace_end == '}') brace_count--;
                    brace_end++;
                }
                
                if (brace_count == 0) {
                    /* Extract the code inside the fragment function */
                    size_t code_len = (brace_end - brace_start) - 2;
                    custom_fragment_code = MEM_mallocN(code_len + 1, "custom_fragment_code");
                    memcpy(custom_fragment_code, brace_start + 1, code_len);
                    custom_fragment_code[code_len] = '\0';
                    
                    /* Extract global code (everything before fragment function) */
                    size_t global_len = fragment_start - custom_shader;
                    if (global_len > 0) {
                        custom_global_frag = MEM_mallocN(global_len + 1, "custom_global_frag");
                        memcpy(custom_global_frag, custom_shader, global_len);
                        custom_global_frag[global_len] = '\0';
                    }
                }
            }
        }
    }
    
    /* Add global custom code before main */
    if (custom_global_frag) {
        BLI_dynstr_append(ds, "/* Custom Fragment Global Code */\n");
        BLI_dynstr_append(ds, custom_global_frag);
        BLI_dynstr_append(ds, "\n");
        MEM_freeN(custom_global_frag);
    }

    /* For world/sky shaders: extract and inject sky global code before main() */
    if (is_world && has_custom_fragment) {
        const char *sky_fn = strstr(custom_shader, "void sky");
        if (sky_fn && sky_fn > custom_shader) {
            size_t sky_global_len = sky_fn - custom_shader;
            char *sky_global_pre = MEM_mallocN(sky_global_len + 1, "sky_global_pre");
            memcpy(sky_global_pre, custom_shader, sky_global_len);
            sky_global_pre[sky_global_len] = '\0';
            if (strlen(sky_global_pre) > 0) {
                BLI_dynstr_append(ds, "/* Custom Sky Global Code */\n");
                BLI_dynstr_append(ds, sky_global_pre);
                BLI_dynstr_append(ds, "\n");
            }
            MEM_freeN(sky_global_pre);
        }
    }
    
    /* Declare additional uniforms needed by custom fragment/sky code that weren't already declared */
    if (custom_fragment_code || use_ubo_lighting || (is_world && has_custom_fragment)) {
        /* Only declare TIME if not already declared by builtins */
        if (!(builtins & GPU_TIME)) {
            const char *time_builtin = GPU_builtin_name(GPU_TIME);
            BLI_dynstr_append(ds, "uniform float ");
            BLI_dynstr_append(ds, time_builtin);
            BLI_dynstr_append(ds, ";\n");
        }
        
        /* Only declare matrices if not already declared */
        if (!(builtins & GPU_OBJECT_MATRIX)) {
            BLI_dynstr_append(ds, "uniform mat4 unfobmat;\n");
        }
        if (!(builtins & GPU_VIEW_MATRIX)) {
            BLI_dynstr_append(ds, "uniform mat4 unfviewmat;\n");
        }
        if (!(builtins & GPU_INVERSE_VIEW_MATRIX)) {
            BLI_dynstr_append(ds, "uniform mat4 unfinvviewmat;\n");
        }
        if (!(builtins & GPU_OBCOLOR)) {
            BLI_dynstr_append(ds, "uniform vec4 unfobcolor;\n");
        }
        BLI_dynstr_append(ds, "\n");
    }
    
    BLI_dynstr_append(ds, "\nvoid main()\n{\n");

    if (builtins & GPU_VIEW_NORMAL) {
        BLI_dynstr_append(ds, "\tvec3 facingnormal = gl_FrontFacing ? varnormal : -varnormal;\n");
    }

#ifdef WITH_OPENSUBDIV
    {
        bool has_tangent = false;
        for (node = nodes->first; node; node = node->next) {
            for (GPUInput *input = node->inputs.first; input; input = input->next) {
                if (input->source == GPU_SOURCE_ATTRIB && input->attribfirst) {
                    if (input->attribtype == CD_TANGENT) {
                        BLI_dynstr_appendf(ds, "#ifdef USE_OPENSUBDIV\n");
                        BLI_dynstr_appendf(ds, "\t%s var%d;\n",
                                           GPU_DATATYPE_STR[input->type],
                                           input->attribid);
                        if (!has_tangent) {
                            BLI_dynstr_appendf(ds, "\tvec3 Q1 = dFdx(inpt.v.position.xyz);\n");
                            BLI_dynstr_appendf(ds, "\tvec3 Q2 = dFdy(inpt.v.position.xyz);\n");
                            BLI_dynstr_appendf(ds, "\tvec2 st1 = dFdx(inpt.v.uv);\n");
                            BLI_dynstr_appendf(ds, "\tvec2 st2 = dFdy(inpt.v.uv);\n");
                            BLI_dynstr_appendf(ds, "\tvec3 T = normalize(Q1 * st2.t - Q2 * st1.t);\n");
                        }
                        BLI_dynstr_appendf(ds, "\tvar%d = vec4(T, 1.0);\n", input->attribid);
                        BLI_dynstr_appendf(ds, "#endif\n");
                    }
                }
            }
        }
    }
#endif

    if (g_useDeferred_GGG) {
		BLI_dynstr_appendf(ds, "\tgl_FragData[0] = vec4(u_MatColor, 1.0);\n");
		BLI_dynstr_appendf(ds, "\tgl_FragData[1] = vec4(u_MatColor, 1.0);\n");
		BLI_dynstr_appendf(ds, "\tgl_FragData[2] = vec4(vec3(u_MatSpec), 1.0);\n");
		BLI_dynstr_appendf(ds, "\tgl_FragData[3] = vec4(vec3(u_MatMetallic), 1.0);\n");
		BLI_dynstr_appendf(ds, "\tgl_FragData[4] = vec4(vec3(u_MatEmission), 1.0);\n");
    }
    else {
        if (use_ubo_lighting) {
            BLI_dynstr_append(ds, "\tvec3 ubo_result = vec3(0.0);\n");
        }
        if (custom_fragment_code || use_ubo_lighting) {
            /* Initialize PBR variables from tracked material property cons IDs */
            BLI_dynstr_append(ds, "\t// Initialize PBR variables from material properties\n");
            
            /* ALBEDO - from diffuse color */
            if (mat_ids.albedo_id >= 0) {
                BLI_dynstr_appendf(ds, "\tvec3 ALBEDO = cons%d;\n", mat_ids.albedo_id);
            } else {
                BLI_dynstr_append(ds, "\tvec3 ALBEDO = vec3(0.8);\n");
            }
            
            /* SPECULAR_RGB — when UBO lighting is active, prefer the dedicated
             * ubo_spec_color field (F0 colour set in the UBO Lighting panel).
             * Fall back to the standard material specular colour, then to 0.04. */
            if (use_ubo_lighting && mat_ids.ubo_speccolor_id >= 0) {
                BLI_dynstr_appendf(ds, "\tvec3 SPECULAR_RGB = cons%d;\n", mat_ids.ubo_speccolor_id);
            } else if (mat_ids.specrgb_id >= 0) {
                BLI_dynstr_appendf(ds, "\tvec3 SPECULAR_RGB = cons%d;\n", mat_ids.specrgb_id);
            } else {
                BLI_dynstr_append(ds, "\tvec3 SPECULAR_RGB = vec3(0.04);\n");
            }

            /* SPECULAR_STRENGTH — UBO specular multiplier (only exists when UBO active) */
            if (use_ubo_lighting && mat_ids.ubo_specstrength_id >= 0) {
                BLI_dynstr_appendf(ds, "\tfloat SPECULAR_STRENGTH = cons%d;\n", mat_ids.ubo_specstrength_id);
            } else {
                BLI_dynstr_append(ds, "\tfloat SPECULAR_STRENGTH = 1.0;\n");
            }
            
            /* ALPHA - from alpha value */
            if (mat_ids.alpha_id >= 0) {
                BLI_dynstr_appendf(ds, "\tfloat ALPHA = cons%d;\n", mat_ids.alpha_id);
            } else {
                BLI_dynstr_append(ds, "\tfloat ALPHA = 1.0;\n");
            }
            
            /* EMIT - from emit value */
            if (mat_ids.emit_id >= 0) {
                BLI_dynstr_appendf(ds, "\tfloat EMIT = cons%d;\n", mat_ids.emit_id);
            } else {
                BLI_dynstr_append(ds, "\tfloat EMIT = 0.0;\n");
            }
            
            /* ROUGHNESS - from roughness value */
            if (mat_ids.roughness_id >= 0) {
                BLI_dynstr_appendf(ds, "\tfloat ROUGHNESS = cons%d;\n", mat_ids.roughness_id);
            } else {
                BLI_dynstr_append(ds, "\tfloat ROUGHNESS = 0.5;\n");
            }
            
            /* METALLIC - from metallic value */
            if (mat_ids.metallic_id >= 0) {
                BLI_dynstr_appendf(ds, "\tfloat METALLIC = cons%d;\n", mat_ids.metallic_id);
            } else {
                BLI_dynstr_append(ds, "\tfloat METALLIC = 0.0;\n");
            }
            
            /* SPECULAR - from specular intensity */
            if (mat_ids.spec_id >= 0) {
                BLI_dynstr_appendf(ds, "\tfloat SPECULAR = cons%d;\n", mat_ids.spec_id);
            } else {
                BLI_dynstr_append(ds, "\tfloat SPECULAR = 0.5;\n");
            }

            /* SCATTER_RGB — UBO scatter tint colour */
            if (use_ubo_lighting && mat_ids.ubo_scattercolor_id >= 0) {
                BLI_dynstr_appendf(ds, "\tvec3 SCATTER_RGB = cons%d;\n", mat_ids.ubo_scattercolor_id);
            } else {
                BLI_dynstr_append(ds, "\tvec3 SCATTER_RGB = vec3(1.0);\n");
            }

            /* SCATTER_FAC — blend factor between Lambert and scatter */
            if (use_ubo_lighting && mat_ids.ubo_scatterfac_id >= 0) {
                BLI_dynstr_appendf(ds, "\tfloat SCATTER_FAC = cons%d;\n", mat_ids.ubo_scatterfac_id);
            } else {
                BLI_dynstr_append(ds, "\tfloat SCATTER_FAC = 0.0;\n");
            }

            /* Additional helper variables (World Space for UBO Lighting) */
            BLI_dynstr_append(ds, "\tvec3 WORLD_POSITION = (unfinvviewmat * vec4(varposition, 1.0)).xyz;\n");
            BLI_dynstr_append(ds, "\tvec3 WORLD_NORMAL = normalize((unfinvviewmat * vec4(facingnormal, 0.0)).xyz);\n");
            BLI_dynstr_append(ds, "\tvec3 WORLD_VIEW = normalize(unfinvviewmat[3].xyz - WORLD_POSITION);\n");
            
            BLI_dynstr_append(ds, "\tvec3 NORMAL = WORLD_NORMAL;\n");
            BLI_dynstr_append(ds, "\tvec3 VERTEX = WORLD_POSITION;\n");
            BLI_dynstr_append(ds, "\tvec3 VIEW = WORLD_VIEW;\n");
            BLI_dynstr_append(ds, "\tfloat TIME = unftime;\n");
            if (builtins & GPU_OBCOLOR) {
                BLI_dynstr_append(ds, "\tvec4 OBJECT_COLOR = unfobcolor;\n");
            } else {
                BLI_dynstr_append(ds, "\tvec4 OBJECT_COLOR = vec4(1.0);\n");
            }
            BLI_dynstr_append(ds, "\n");
            
            if (custom_fragment_code) {
                /* Add custom fragment code */
                BLI_dynstr_append(ds, "\t/* Custom Fragment Code */\n");
                BLI_dynstr_append(ds, custom_fragment_code);
                BLI_dynstr_append(ds, "\n");
                MEM_freeN(custom_fragment_code);
            }

            if (use_ubo_lighting) {
                BLI_dynstr_append(ds, "\t/* Inject UBO Lighting */\n");
                /* Pass SCATTER_RGB/SCATTER_FAC from material props (instead of fixed placeholders). */
                BLI_dynstr_append(ds, "\tcalcLight(VERTEX, NORMAL, VIEW, ALBEDO, SPECULAR_RGB * SPECULAR_STRENGTH, SCATTER_RGB, ROUGHNESS, METALLIC, SCATTER_FAC, ubo_result);\n");
            }

            /* Write modified PBR variables back to material property cons vars.
             * Only do this when a custom fragment shader is present: those shaders
             * may modify ALBEDO/NORMAL/etc. and need the values propagated into the
             * BGE node pipeline.  For pure UBO-lighting (no custom shader) the cons
             * were declared as 'const' so writing to them is a GLSL compile error,
             * and calcLight() already received the values directly above. */
            if (custom_fragment_code) {
                BLI_dynstr_append(ds, "\t// Apply modified PBR values back to material properties\n");

                if (mat_ids.albedo_id >= 0) {
                    BLI_dynstr_appendf(ds, "\tcons%d = ALBEDO;\n", mat_ids.albedo_id);
                }

                if (mat_ids.specrgb_id >= 0) {
                    BLI_dynstr_appendf(ds, "\tcons%d = SPECULAR_RGB;\n", mat_ids.specrgb_id);
                }

                if (mat_ids.alpha_id >= 0) {
                    BLI_dynstr_appendf(ds, "\tcons%d = ALPHA;\n", mat_ids.alpha_id);
                }

                if (mat_ids.emit_id >= 0) {
                    BLI_dynstr_appendf(ds, "\tcons%d = EMIT;\n", mat_ids.emit_id);
                }

                if (mat_ids.roughness_id >= 0) {
                    BLI_dynstr_appendf(ds, "\tcons%d = ROUGHNESS;\n", mat_ids.roughness_id);
                }

                if (mat_ids.metallic_id >= 0) {
                    BLI_dynstr_appendf(ds, "\tcons%d = METALLIC;\n", mat_ids.metallic_id);
                }

                if (mat_ids.spec_id >= 0) {
                    BLI_dynstr_appendf(ds, "\tcons%d = SPECULAR;\n", mat_ids.spec_id);
                }

                /* Writeback SCATTER_RGB and SCATTER_FAC so custom shaders can modify them
                 * before they reach calcLight. Only available when UBO lighting is active
                 * (SCATTER_RGB/SCATTER_FAC are only declared in that path). */
                if (use_ubo_lighting) {
                    if (mat_ids.ubo_scattercolor_id >= 0) {
                        BLI_dynstr_appendf(ds, "\tcons%d = SCATTER_RGB;\n", mat_ids.ubo_scattercolor_id);
                    }
                    if (mat_ids.ubo_scatterfac_id >= 0) {
                        BLI_dynstr_appendf(ds, "\tcons%d = SCATTER_FAC;\n", mat_ids.ubo_scatterfac_id);
                    }
                    BLI_dynstr_append(ds, "\tfacingnormal = NORMAL;\n");
                }
            }
            BLI_dynstr_append(ds, "\n");
        }

        /* ---- Custom Sky Shader ---- */
        if (is_world && has_custom_fragment) {
            /* Extract void sky(){} body and optional global code */
            char *sky_global_code = NULL;
            char *sky_body_code   = NULL;

            const char *sky_start = strstr(custom_shader, "void sky");
            if (sky_start) {
                const char *brace_start = strchr(sky_start, '{');
                if (brace_start) {
                    int brace_count = 1;
                    const char *brace_end = brace_start + 1;
                    while (*brace_end && brace_count > 0) {
                        if (*brace_end == '{') brace_count++;
                        else if (*brace_end == '}') brace_count--;
                        brace_end++;
                    }
                    if (brace_count == 0) {
                        size_t code_len = (brace_end - brace_start) - 2;
                        sky_body_code = MEM_mallocN(code_len + 1, "sky_body_code");
                        memcpy(sky_body_code, brace_start + 1, code_len);
                        sky_body_code[code_len] = '\0';

                        size_t global_len = sky_start - custom_shader;
                        if (global_len > 0) {
                            sky_global_code = MEM_mallocN(global_len + 1, "sky_global_code");
                            memcpy(sky_global_code, custom_shader, global_len);
                            sky_global_code[global_len] = '\0';
                        }
                    }
                }
            }

            if (sky_body_code) {
                /* --- Declare intermediate sky variables --- */
                BLI_dynstr_append(ds, "\t/* Custom Sky Shader — intermediate variables */\n");

                /* HORIZON_COLOR */
                if (mat_ids.world_horizon_id >= 0)
                    BLI_dynstr_appendf(ds, "\tvec3 HORIZON_COLOR = unf%d;\n", mat_ids.world_horizon_id);
                else
                    BLI_dynstr_append(ds, "\tvec3 HORIZON_COLOR = vec3(0.3, 0.5, 0.8);\n");

                /* ZENITH_COLOR */
                if (mat_ids.world_zenith_id >= 0)
                    BLI_dynstr_appendf(ds, "\tvec3 ZENITH_COLOR = unf%d;\n", mat_ids.world_zenith_id);
                else
                    BLI_dynstr_append(ds, "\tvec3 ZENITH_COLOR = vec3(0.1, 0.2, 0.6);\n");

                /* VIEW_DIR — direction in view/camera space (varposition is already in view space for sky dome) */
                BLI_dynstr_append(ds, "\tvec3 VIEW_DIR = normalize(varposition);\n");
                /* WORLD_VIEW_DIR — VIEW_DIR rotated into world space via the inverse view matrix */
                BLI_dynstr_append(ds, "\tvec3 WORLD_VIEW_DIR = normalize((unfinvviewmat * vec4(VIEW_DIR, 0.0)).xyz);\n");

                /* ENV_ENERGY */
                if (mat_ids.world_envlight_id >= 0)
                    BLI_dynstr_appendf(ds, "\tfloat ENV_ENERGY = unf%d;\n", mat_ids.world_envlight_id);
                else
                    BLI_dynstr_append(ds, "\tfloat ENV_ENERGY = 1.0;\n");

                /* TIME */
                BLI_dynstr_append(ds, "\tfloat TIME = unftime;\n");

                if (custom_sky_override) {
                    /* -------------------------------------------------------
                     * OVERRIDE MODE: custom defines the final sky colour.
                     * SKY_COLOR starts at vec3(0) — custom must set it.
                     * Pipeline is NOT called; we write gl_FragColor directly.
                     * ------------------------------------------------------- */
                    BLI_dynstr_append(ds, "\tvec3 SKY_COLOR = vec3(0.0);\n");
                    BLI_dynstr_append(ds, "\t/* Custom Sky Body (override) */\n");
                    BLI_dynstr_append(ds, sky_body_code);
                    BLI_dynstr_append(ds, "\n");
                    /* Write final colour — bypass pipeline completely */
                    BLI_dynstr_append(ds, "\tgl_FragColor = vec4(SKY_COLOR, 1.0);\n");
                    BLI_dynstr_append(ds, "}\n");
                    /* Early return — skip codegen_declare_tmps / codegen_call_functions */
                    MEM_freeN(sky_body_code);
                    if (sky_global_code) MEM_freeN(sky_global_code);
                    code = BLI_dynstr_get_cstring(ds);
                    BLI_dynstr_free(ds);
                    return code;
                }
                else {
                    /* -------------------------------------------------------
                     * PRE-PROCESS MODE: custom runs first, modifying the local
                     * HORIZON_COLOR / ZENITH_COLOR / ENV_ENERGY variables.
                     * We declare mutable local aliases cons{N} = unf{N} here in
                     * the main() body.  codegen_call_functions() routes those
                     * world-colour inputs through cons{N} (not unf{N}) so the
                     * pipeline sees the values the custom body wrote.
                     * ------------------------------------------------------- */
                    sky_preprocess_cons = true;

                    /* Declare mutable local aliases for world colour uniforms */
                    BLI_dynstr_append(ds, "\t/* Mutable local aliases for world uniforms (sky pre-process) */\n");
                    if (mat_ids.world_horizon_id >= 0)
                        BLI_dynstr_appendf(ds, "\tvec3 cons%d = unf%d;\n",
                            mat_ids.world_horizon_id, mat_ids.world_horizon_id);
                    if (mat_ids.world_zenith_id >= 0)
                        BLI_dynstr_appendf(ds, "\tvec3 cons%d = unf%d;\n",
                            mat_ids.world_zenith_id, mat_ids.world_zenith_id);
                    if (mat_ids.world_envlight_id >= 0)
                        BLI_dynstr_appendf(ds, "\tfloat cons%d = unf%d;\n",
                            mat_ids.world_envlight_id, mat_ids.world_envlight_id);

                    BLI_dynstr_append(ds, "\tvec3 SKY_COLOR = vec3(0.0);\n");
                    BLI_dynstr_append(ds, "\t/* Custom Sky Body (pre-process) */\n");
                    BLI_dynstr_append(ds, sky_body_code);
                    BLI_dynstr_append(ds, "\n");

                    /* Write-back: copy modified helper vars into the cons aliases */
                    BLI_dynstr_append(ds, "\t/* Write-back sky vars into pipeline aliases */\n");
                    if (mat_ids.world_horizon_id >= 0)
                        BLI_dynstr_appendf(ds, "\tcons%d = HORIZON_COLOR;\n", mat_ids.world_horizon_id);
                    if (mat_ids.world_zenith_id >= 0)
                        BLI_dynstr_appendf(ds, "\tcons%d = ZENITH_COLOR;\n", mat_ids.world_zenith_id);
                    if (mat_ids.world_envlight_id >= 0)
                        BLI_dynstr_appendf(ds, "\tcons%d = ENV_ENERGY;\n", mat_ids.world_envlight_id);
                }

                MEM_freeN(sky_body_code);
            }

            if (sky_global_code) {
                MEM_freeN(sky_global_code);
            }
        }

        /* Declare temp variables and call functions for both UBO and non-UBO */
        codegen_declare_tmps(ds, nodes);
        codegen_call_functions(ds, nodes, outputs, &mat_ids, sky_preprocess_cons);
 }

    BLI_dynstr_append(ds, "}\n");

    code = BLI_dynstr_get_cstring(ds);
    BLI_dynstr_free(ds);

    return code;
}

static char *code_generate_vertex(ListBase *nodes, const GPUMatType type, bool use_instancing, const char *custom_shader)
{
	DynStr *ds = BLI_dynstr_new();
	GPUNode *node;
	GPUInput *input;
	char *code;
	char *vertcode = NULL;

	for (node = nodes->first; node; node = node->next) {
		for (input = node->inputs.first; input; input = input->next) {
			if (input->source == GPU_SOURCE_ATTRIB && input->attribfirst) {
#ifdef WITH_OPENSUBDIV
				bool skip_opensubdiv = ELEM(input->attribtype, CD_MTFACE, CD_TANGENT);
				if (skip_opensubdiv) {
					BLI_dynstr_appendf(ds, "#ifndef USE_OPENSUBDIV\n");
				}
#endif
				BLI_dynstr_appendf(ds, "%s %s att%d;\n",
					GLEW_VERSION_3_0 ? "in" : "attribute",
					GPU_DATATYPE_STR[input->type], input->attribid);
				BLI_dynstr_appendf(ds, "uniform int att%d_info;\n",  input->attribid);
				BLI_dynstr_appendf(ds, "%s %s var%d;\n",
					GLEW_VERSION_3_0 ? "out" : "varying",
					GPU_DATATYPE_STR[input->type], input->attribid);
#ifdef WITH_OPENSUBDIV
				if (skip_opensubdiv) {
					BLI_dynstr_appendf(ds, "#endif\n");
				}
#endif
			}
		}
	}

	BLI_dynstr_append(ds, "\n");

	switch (type) {
		case GPU_MATERIAL_TYPE_MESH:
			vertcode = datatoc_gpu_shader_vertex_glsl;
			break;
		case GPU_MATERIAL_TYPE_WORLD:
			vertcode = datatoc_gpu_shader_vertex_world_glsl;
			break;
		default:
			fprintf(stderr, "invalid material type, set one after GPU_material_construct_begin\n");
			break;
	}

	/* Process custom shader to extract code for injection */
	char *custom_global_code = NULL;
	char *custom_vertex_code = NULL;
	
	if (custom_shader && strlen(custom_shader) > 0) {
		/* Find the vertex function */
		const char *vertex_start = strstr(custom_shader, "void vertex");
		if (vertex_start) {
			/* Find the opening brace */
			const char *brace_start = strchr(vertex_start, '{');
			if (brace_start) {
				/* Find the matching closing brace */
				int brace_count = 1;
				const char *brace_end = brace_start + 1;
				while (*brace_end && brace_count > 0) {
					if (*brace_end == '{') brace_count++;
					else if (*brace_end == '}') brace_count--;
					brace_end++;
				}
				
				if (brace_count == 0) {
					/* Extract the code inside the vertex function */
					size_t code_len = (brace_end - brace_start) - 2;
					custom_vertex_code = MEM_mallocN(code_len + 1, "custom_vertex_code");
					memcpy(custom_vertex_code, brace_start + 1, code_len);
					custom_vertex_code[code_len] = '\0';
					
					/* Extract global code (everything before vertex function) */
					size_t global_len = vertex_start - custom_shader;
					if (global_len > 0) {
						custom_global_code = MEM_mallocN(global_len + 1, "custom_global_code");
						memcpy(custom_global_code, custom_shader, global_len);
						custom_global_code[global_len] = '\0';
					}
				}
			}
		}
	}

	/* Add global custom code before the vertex shader template */
	if (custom_global_code) {
		BLI_dynstr_append(ds, "/* Custom Global Code */\n");
		BLI_dynstr_append(ds, custom_global_code);
		BLI_dynstr_append(ds, "\n");
		MEM_freeN(custom_global_code);
	}
	
	/* Declare unftime uniform if custom vertex code exists */
	if (custom_vertex_code) {
		const char *time_builtin = GPU_builtin_name(GPU_TIME);
		BLI_dynstr_append(ds, "uniform float ");
		BLI_dynstr_append(ds, time_builtin);
		BLI_dynstr_append(ds, ";\n\n");
	}

	/* Now add the vertex shader template */
	if (custom_vertex_code) {
		/* Replace the injection point marker with custom code */
		char *injection_point = strstr(vertcode, "// CUSTOM_VERTEX_CODE_INJECTION_POINT");
		if (injection_point) {
			/* Add code before injection point */
			size_t before_len = injection_point - vertcode;
			BLI_dynstr_nappend(ds, vertcode, before_len);
			
			/* Inject variable declarations and TIME */
			BLI_dynstr_append(ds, "vec3 VERTEX = vec3(position);\n");
			BLI_dynstr_append(ds, "\tvec3 NORMAL = normal;\n");
			BLI_dynstr_append(ds, "\tfloat TIME = unftime;\n\n");
			
			/* Add custom vertex code */
			BLI_dynstr_append(ds, "\t/* Custom Vertex Code */\n");
			BLI_dynstr_append(ds, custom_vertex_code);
			BLI_dynstr_append(ds, "\n\n");
			
			/* Replace position with VERTEX for transformation */
			BLI_dynstr_append(ds, "\t// Use modified VERTEX and NORMAL\n");
			BLI_dynstr_append(ds, "\tvec4 co = gl_ModelViewMatrix * vec4(VERTEX, 1.0);\n");
			
			/* Add code after injection point (skip the marker line and the original co= line) */
			const char *after_marker = strchr(injection_point, '\n');
			if (after_marker) {
				/* Skip to after the "vec4 co = ..." line */
				const char *co_line_end = strstr(after_marker, "gl_ModelViewMatrix * position;");
				if (co_line_end) {
					co_line_end = strchr(co_line_end, '\n');
					if (co_line_end) {
						/* Now we need to replace 'normal' with 'NORMAL' in the varnormal line */
						const char *search_start = co_line_end + 1;
						const char *varnormal_line = strstr(search_start, "varnormal = normalize(gl_NormalMatrix * normal)");
						
						if (varnormal_line) {
							/* Add everything before varnormal line */
							size_t before_varnormal_len = varnormal_line - search_start;
							BLI_dynstr_nappend(ds, search_start, before_varnormal_len);
							
							/* Add modified varnormal line using NORMAL instead of normal */
							BLI_dynstr_append(ds, "varnormal = normalize(gl_NormalMatrix * NORMAL)");
							
							/* Skip the original varnormal line and continue with the rest */
							const char *after_varnormal = strchr(varnormal_line, ';');
							if (after_varnormal) {
								BLI_dynstr_append(ds, after_varnormal);
							}
						}
						else {
							/* Fallback if we can't find varnormal line */
							BLI_dynstr_append(ds, search_start);
						}
					}
				}
				else {
					BLI_dynstr_append(ds, after_marker + 1);
				}
			}
			
			MEM_freeN(custom_vertex_code);
		}
		else {
			/* Fallback: no injection point found, just append template */
			BLI_dynstr_append(ds, vertcode);
			MEM_freeN(custom_vertex_code);
		}
	}
	else {
		/* No custom code, just use template as-is */
		BLI_dynstr_append(ds, vertcode);
	}

	for (node = nodes->first; node; node = node->next)
		for (input = node->inputs.first; input; input = input->next)
			if (input->source == GPU_SOURCE_ATTRIB && input->attribfirst) {
				if (input->attribtype == CD_TANGENT) { /* silly exception */
#ifdef WITH_OPENSUBDIV
					BLI_dynstr_appendf(ds, "#ifndef USE_OPENSUBDIV\n");
#endif
					if (use_instancing) {
						BLI_dynstr_appendf(
							ds, "\tvar%d.xyz = normalize(gl_NormalMatrix * (att%d.xyz * %s));\n",
							input->attribid, input->attribid, GPU_builtin_name(GPU_INSTANCING_MATRIX_ATTRIB));
					}
					else {
						BLI_dynstr_appendf(
							ds, "\tvar%d.xyz = normalize(gl_NormalMatrix * att%d.xyz);\n",
							input->attribid, input->attribid);
					}
					BLI_dynstr_appendf(
					        ds, "\tvar%d.w = att%d.w;\n",
					        input->attribid, input->attribid);
#ifdef WITH_OPENSUBDIV
					BLI_dynstr_appendf(ds, "#endif\n");
#endif
				}
				else {
#ifdef WITH_OPENSUBDIV
					bool is_mtface = input->attribtype == CD_MTFACE;
					if (is_mtface) {
						BLI_dynstr_appendf(ds, "#ifndef USE_OPENSUBDIV\n");
					}
#endif
					BLI_dynstr_appendf(ds, "\tset_var_from_attr(att%d, att%d_info, var%d);\n",
					                   input->attribid, input->attribid, input->attribid);
#ifdef WITH_OPENSUBDIV
					if (is_mtface) {
						BLI_dynstr_appendf(ds, "#endif\n");
					}
#endif
				}
			}
			/* unfortunately special handling is needed here because we abuse gl_Color/gl_SecondaryColor flat shading */
			else if (input->source == GPU_SOURCE_OPENGL_BUILTIN) {
				if (input->oglbuiltin == GPU_MATCAP_NORMAL) {
					/* remap to 0.0 - 1.0 range. This is done because OpenGL 2.0 clamps colors
					 * between shader stages and we want the full range of the normal */
					BLI_dynstr_appendf(ds, "\tvec3 matcapcol = vec3(0.5) * varnormal + vec3(0.5);\n");
					BLI_dynstr_appendf(ds, "\tgl_FrontSecondaryColor = vec4(matcapcol, 1.0);\n");
				}
				else if (input->oglbuiltin == GPU_COLOR) {
					BLI_dynstr_appendf(ds, "\tgl_FrontColor = gl_Color;\n");
				}
			}

	BLI_dynstr_append(ds, "}\n");

	code = BLI_dynstr_get_cstring(ds);

	BLI_dynstr_free(ds);

	return code;
}



static char *code_generate_geometry(ListBase *nodes, bool use_opensubdiv)
{
#ifdef WITH_OPENSUBDIV
	if (use_opensubdiv) {
		DynStr *ds = BLI_dynstr_new();
		GPUNode *node;
		GPUInput *input;
		char *code;

		/* Generate varying declarations. */
		for (node = nodes->first; node; node = node->next) {
			for (input = node->inputs.first; input; input = input->next) {
				if (input->source == GPU_SOURCE_ATTRIB && input->attribfirst) {
					if (input->attribtype == CD_MTFACE) {
						/* NOTE: For now we are using varying on purpose,
						 * otherwise we are not able to write to the varying.
						 */
						BLI_dynstr_appendf(ds, "%s %s var%d%s;\n",
						                   "varying",
						                   GPU_DATATYPE_STR[input->type],
						                   input->attribid,
						                   "");
						BLI_dynstr_appendf(ds, "uniform int fvar%d_offset;\n",
						                   input->attribid);
					}
				}
			}
		}

		BLI_dynstr_append(ds, datatoc_gpu_shader_geometry_glsl);

		/* Generate varying assignments. */
		for (node = nodes->first; node; node = node->next) {
			for (input = node->inputs.first; input; input = input->next) {
				if (input->source == GPU_SOURCE_ATTRIB && input->attribfirst) {
					if (input->attribtype == CD_MTFACE) {
						BLI_dynstr_appendf(
						        ds,
						        "\tINTERP_FACE_VARYING_ATT_2(var%d, "
						        "int(texelFetch(FVarDataOffsetBuffer, fvar%d_offset).r), st);\n",
						        input->attribid,
						        input->attribid);
					}
				}
			}
		}

		BLI_dynstr_append(ds, "}\n");
		code = BLI_dynstr_get_cstring(ds);
		BLI_dynstr_free(ds);

		//if (G.debug & G_DEBUG) printf("%s\n", code);

		return code;
	}
#else
	UNUSED_VARS(nodes, use_opensubdiv);
#endif
	return NULL;
}


void GPU_code_generate_glsl_lib(void)
{
    DynStr *ds;

    
    if (glsl_material_library)
        return;

    ds = BLI_dynstr_new();

    /* ==== NEW UBO LIGHTING SYSTEM ==== */
    if (USE_UBO_LIGHTING_SYSTEM) {
        /* Use the new UBO-based lighting shader instead of old lamp functions */
        BLI_dynstr_append(ds, datatoc_gpu_shader_ubo_lighting_glsl);
        printf("[GPU_codegen] Using UBO lighting system shader (Global)\n");
    }
    else {
        /* Use old lighting system (default) */
        const char *filename = "gpu_shader_material.glsl";

        FILE *f = fopen(filename, "r");
        if (f != NULL) {
           
            char line[4096]; 

            while (fgets(line, sizeof(line), f)) {
                
                size_t len = strlen(line);
                while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                    line[len - 1] = '\0';
                    len--;
                }

                
                BLI_dynstr_append(ds, line);
                BLI_dynstr_append(ds, "\n"); 
            }
            fclose(f);
        }
        else {
            
            BLI_dynstr_append(ds, datatoc_gpu_shader_material_glsl);
        }

        /* Also append UBO lighting functions for per-material usage */
        BLI_dynstr_append(ds, "\n// Per-material UBO Lighting Library\n");
        BLI_dynstr_append(ds, datatoc_gpu_shader_ubo_lighting_glsl);
    }

    /* Append custom scattering shader */
    //if (gpu_shader_atmospheric_scattering_glsl) {
    //    BLI_dynstr_append(ds, gpu_shader_atmospheric_scattering_glsl);
    //}

    BLI_dynstr_append(ds,
        "\n"
        "#ifndef CUSTOM_EMIT_RGB\n"
        "#define CUSTOM_EMIT_RGB\n"
        "vec3 _EMIT_RGB = vec3(1.0);\n"
        "#define EMIT_RGB _EMIT_RGB\n"
        "void shade_mul_emit(vec4 col1, vec4 col2, out vec4 outcol)\n"
        "{\n"
        "\toutcol = col1 * col2;\n"
        "\toutcol.rgb *= vec3(EMIT_RGB);\n"
        "}\n"
        "void shade_mul_emit_value(float fac, vec4 col, out vec4 outcol)\n"
        "{\n"
        "\toutcol = col * fac;\n"
        "\toutcol.rgb *= vec3(EMIT_RGB);\n"
        "}\n"
        "#endif\n");

    glsl_material_library = BLI_dynstr_get_cstring(ds);

    BLI_dynstr_free(ds);
}



/* GPU pass binding/unbinding */

GPUShader *GPU_pass_shader(GPUPass *pass)
{
	return pass->shader;
}

static void gpu_nodes_extract_dynamic_inputs(GPUPass *pass, ListBase *nodes)
{
	GPUShader *shader = pass->shader;
	GPUNode *node;
	GPUInput *next, *input;
	ListBase *inputs = &pass->inputs;
	int extract, z;

	memset(inputs, 0, sizeof(*inputs));

	if (!shader)
		return;

	GPU_shader_bind(shader);

	for (node = nodes->first; node; node = node->next) {
		z = 0;
		for (input = node->inputs.first; input; input = next, z++) {
			next = input->next;

			/* attributes don't need to be bound, they already have
			 * an id that the drawing functions will use */
			if (input->source == GPU_SOURCE_ATTRIB) {
#ifdef WITH_OPENSUBDIV
				/* We do need mtface attributes for later, so we can
				 * update face-varuing variables offset in the texture
				 * buffer for proper sampling from the shader.
				 *
				 * We don't do anything about attribute itself, we
				 * only use it to learn which uniform name is to be
				 * updated.
				 *
				 * TODO(sergey): We can add ad extra uniform input
				 * for the offset, which will be purely internal and
				 * which would avoid having such an exceptions.
				 */
				if (input->attribtype != CD_MTFACE) {
					continue;
				}
#else
				continue;
#endif
			}
			if (input->source == GPU_SOURCE_BUILTIN ||
			    input->source == GPU_SOURCE_OPENGL_BUILTIN)
			{
				continue;
			}

			if (input->ima || input->tex || input->prv || input->texptr) {
				BLI_snprintf(input->shadername, sizeof(input->shadername), "samp%d", input->texid);
			}
			else
				BLI_snprintf(input->shadername, sizeof(input->shadername), "unf%d", input->id);

			/* pass non-dynamic uniforms to opengl */
			extract = 0;

			if (input->ima || input->tex || input->prv || input->texptr) {
				if (input->bindtex)
					extract = 1;
			}
			else if (input->dynamicvec)
				extract = 1;

			if (extract)
				input->shaderloc = GPU_shader_get_uniform(shader, input->shadername);

#ifdef WITH_OPENSUBDIV
			if (input->source == GPU_SOURCE_ATTRIB &&
			    input->attribtype == CD_MTFACE)
			{
				extract = 1;
			}
#endif

			/* extract nodes */
			if (extract) {
				BLI_remlink(&node->inputs, input);
				BLI_addtail(inputs, input);
			}
		}
	}

	GPU_shader_unbind();
}

void GPU_pass_bind(GPUPass *pass, double time, int mipmap)
{
	GPUInput *input;
	GPUShader *shader = pass->shader;
	ListBase *inputs = &pass->inputs;

	if (!shader)
		return;

	GPU_shader_bind(shader);

	/* create the textures */
	for (input = inputs->first; input; input = input->next) {
		if (input->ima)
			input->tex = GPU_texture_from_blender(input->ima, input->iuser, input->textarget, input->image_isdata, time, mipmap);
		else if (input->prv)
			input->tex = GPU_texture_from_preview(input->prv, mipmap);
	}

	/* bind/upload the textures */
	for (input = inputs->first; input; input = input->next) {
		GPUTexture *tex = input->tex ? input->tex :
		                  (input->texptr ? *input->texptr : NULL);

		if (!tex || !input->bindtex) continue;

		if (GLEW_ARB_bindless_texture) {
			/* Bindless path: make handle resident and send 64-bit handle */
			GPU_texture_make_bindless_resident(tex);
			GLuint64 handle = GPU_texture_bindless_handle(tex);
			if (handle && input->shaderloc >= 0) {
				glProgramUniformHandleui64ARB(
					(GLuint)GPU_shader_program(shader), input->shaderloc, handle);
			}
		}
		else {
			/* Classic path: bind to texture unit and send unit index */
			GPU_texture_bind(tex, input->texid);
			GPU_shader_uniform_texture(shader, input->shaderloc, tex);
		}
	}
}

void GPU_pass_update_uniforms(GPUPass *pass)
{
	GPUInput *input;
	GPUShader *shader = pass->shader;
	ListBase *inputs = &pass->inputs;

	if (!shader)
		return;

	/* pass dynamic inputs to opengl, others were removed */
	for (input = inputs->first; input; input = input->next) {
		if (!(input->ima || input->tex || input->prv || input->texptr)) {
			if (input->type == GPU_INT) {
				GPU_shader_uniform_vector_int(shader, input->shaderloc, 1, 1, (int *)input->dynamicvec);
			}
			else {
				GPU_shader_uniform_vector(shader, input->shaderloc, input->type, 1,
					input->dynamicvec);
			}
		}
	}
}

void GPU_pass_unbind(GPUPass *pass)
{
	GPUInput *input;
	GPUShader *shader = pass->shader;
	ListBase *inputs = &pass->inputs;

	if (!shader)
		return;

	if (!GLEW_ARB_bindless_texture) {
		/* Classic path: unbind each texture from its unit */
		for (input = inputs->first; input; input = input->next) {
			if (input->tex && input->bindtex)
				GPU_texture_unbind(input->tex);
			if (input->texptr && *input->texptr && input->bindtex)
				GPU_texture_unbind(*input->texptr);
		}
	}

	/* Release tex references (both modes) */
	for (input = inputs->first; input; input = input->next) {
		if (input->ima || input->prv)
			input->tex = NULL;
	}

	GPU_shader_unbind();
}

/* Node Link Functions */

static GPUNodeLink *GPU_node_link_create(void)
{
	GPUNodeLink *link = MEM_callocN(sizeof(GPUNodeLink), "GPUNodeLink");
	link->type = GPU_NONE;
	link->users++;

	return link;
}

static void gpu_node_link_free(GPUNodeLink *link)
{
	link->users--;

	if (link->users < 0)
		fprintf(stderr, "GPU_node_link_free: negative refcount\n");

	if (link->users == 0) {
		if (link->output)
			link->output->link = NULL;
		MEM_freeN(link);
	}
}

/* Node Functions */

static GPUNode *GPU_node_begin(const char *name)
{
	GPUNode *node = MEM_callocN(sizeof(GPUNode), "GPUNode");

	node->name = name;

	return node;
}

static void gpu_node_input_link(GPUNode *node, GPUNodeLink *link, const GPUType type)
{
	GPUInput *input;
	GPUNode *outnode;
	const char *name;

	if (link->output) {
		outnode = link->output->node;
		name = outnode->name;
		input = outnode->inputs.first;

		if ((STREQ(name, "set_value") || STREQ(name, "set_rgb")) &&
		    (input->type == type))
		{
			input = MEM_dupallocN(outnode->inputs.first);
			input->type = type;
			if (input->link)
				input->link->users++;
			BLI_addtail(&node->inputs, input);
			return;
		}
	}

	input = MEM_callocN(sizeof(GPUInput), "GPUInput");
	input->node = node;

	if (link->builtin) {
		/* builtin uniform */
		input->type = type;
		input->source = GPU_SOURCE_BUILTIN;
		input->builtin = link->builtin;

		MEM_freeN(link);
	}
	else if (link->oglbuiltin) {
		/* builtin uniform */
		input->type = type;
		input->source = GPU_SOURCE_OPENGL_BUILTIN;
		input->oglbuiltin = link->oglbuiltin;

		MEM_freeN(link);
	}
	else if (link->output) {
		/* link to a node output */
		input->type = type;
		input->source = GPU_SOURCE_TEX_PIXEL;
		input->link = link;
		link->users++;
	}
	else if (link->dynamictex) {
		/* dynamic texture, GPUTexture is updated/deleted externally */
		input->type = type;
		input->source = GPU_SOURCE_TEX;

		input->tex = link->dynamictex;
		input->textarget = GL_TEXTURE_2D;
		input->textype = type;
		input->dynamictex = true;
		input->dynamicdata = link->ptr2;
		MEM_freeN(link);
	}
	else if (link->dynamictexptr) {
		/* dynamic texture, GPUTexture is updated/deleted externally */
		input->type = type;
		input->source = GPU_SOURCE_TEX;

		input->texptr = link->dynamictexptr;
		input->textarget = GL_TEXTURE_2D;
		input->textype = type;
		input->dynamictex = true;
		input->dynamicdata = link->ptr2;
		MEM_freeN(link);
	}
	else if (link->texture) {
		/* small texture created on the fly, like for colorbands */
		input->type = GPU_VEC4;
		input->source = GPU_SOURCE_TEX;
		input->textype = type;

#if 0
		input->tex = GPU_texture_create_2D(link->texturesize, link->texturesize, link->ptr2, NULL);
#endif
		input->tex = GPU_texture_create_2D(link->texturesize, 1, link->ptr1, GPU_HDR_NONE, NULL);
		input->textarget = GL_TEXTURE_2D;

		MEM_freeN(link->ptr1);
		MEM_freeN(link);
	}
	else if (link->image) {
		/* blender image */
		input->type = GPU_VEC4;
		input->source = GPU_SOURCE_TEX;

		if (link->image == GPU_NODE_LINK_IMAGE_PREVIEW) {
			input->prv = link->ptr1;
			input->textarget = GL_TEXTURE_2D;
			input->textype = GPU_TEX2D;
		}
		else if (link->image == GPU_NODE_LINK_IMAGE_BLENDER) {
			input->ima = link->ptr1;
			input->iuser = link->ptr2;
			input->image_isdata = link->image_isdata;
			input->textarget = GL_TEXTURE_2D;
			input->textype = GPU_TEX2D;
		}
		else if (link->image == GPU_NODE_LINK_IMAGE_CUBE_MAP) {
			input->ima = link->ptr1;
			input->iuser = link->ptr2;
			input->image_isdata = link->image_isdata;
			input->textarget = GL_TEXTURE_CUBE_MAP;
			input->textype = GPU_TEXCUBE;
		}
		MEM_freeN(link);
	}
	else if (link->attribtype) {
		/* vertex attribute */
		input->type = type;
		input->source = GPU_SOURCE_ATTRIB;

		input->attribtype = link->attribtype;
		BLI_strncpy(input->attribname, link->attribname, sizeof(input->attribname));
		MEM_freeN(link);
	}
	else {
		/* uniform vector */
		input->type = type;
		input->source = GPU_SOURCE_VEC_UNIFORM;

		memcpy(input->vec, link->ptr1, GPU_DATATYPE_SIZE[type] * sizeof(float));
		if (link->dynamic) {
			input->dynamicvec = link->ptr1;
			input->dynamictype = link->dynamictype;
			input->dynamicdata = link->ptr2;
		}
		else {
			/* Even for non-dynamic (const) uniforms, preserve the dynamictype for tracking */
			input->dynamictype = link->dynamictype;
		}
		MEM_freeN(link);
	}

	BLI_addtail(&node->inputs, input);
}

static void gpu_node_input_socket(GPUNode *node, GPUNodeStack *sock)
{
	GPUNodeLink *link;

	if (sock->link) {
		gpu_node_input_link(node, sock->link, sock->type);
	}
	else {
		link = GPU_node_link_create();
		link->ptr1 = sock->vec;
		gpu_node_input_link(node, link, sock->type);
	}
}

static void gpu_node_output(GPUNode *node, const GPUType type, GPUNodeLink **link)
{
	GPUOutput *output = MEM_callocN(sizeof(GPUOutput), "GPUOutput");

	output->type = type;
	output->node = node;

	if (link) {
		*link = output->link = GPU_node_link_create();
		output->link->type = type;
		output->link->output = output;

		/* note: the caller owns the reference to the link, GPUOutput
		 * merely points to it, and if the node is destroyed it will
		 * set that pointer to NULL */
	}

	BLI_addtail(&node->outputs, output);
}

static void gpu_inputs_free(ListBase *inputs)
{
	GPUInput *input;

	for (input = inputs->first; input; input = input->next) {
		if (input->link)
			gpu_node_link_free(input->link);
		else if (input->tex && !input->dynamictex)
			GPU_texture_free(input->tex);
	}

	BLI_freelistN(inputs);
}

static void gpu_node_free(GPUNode *node)
{
	GPUOutput *output;

	gpu_inputs_free(&node->inputs);

	for (output = node->outputs.first; output; output = output->next)
		if (output->link) {
			output->link->output = NULL;
			gpu_node_link_free(output->link);
		}

	BLI_freelistN(&node->outputs);
	MEM_freeN(node);
}

static void gpu_nodes_free(ListBase *nodes)
{
	GPUNode *node;

	while ((node = BLI_pophead(nodes))) {
		gpu_node_free(node);
	}
}

/* vertex attributes */

static void gpu_nodes_get_vertex_attributes(ListBase *nodes, GPUVertexAttribs *attribs)
{
	GPUNode *node;
	GPUInput *input;
	int a;

	/* convert attributes requested by node inputs to an array of layers,
	 * checking for duplicates and assigning id's starting from zero. */

	memset(attribs, 0, sizeof(*attribs));

	for (node = nodes->first; node; node = node->next) {
		for (input = node->inputs.first; input; input = input->next) {
			if (input->source == GPU_SOURCE_ATTRIB) {
				for (a = 0; a < attribs->totlayer; a++) {
					if (attribs->layer[a].type == input->attribtype &&
					    STREQ(attribs->layer[a].name, input->attribname))
					{
						break;
					}
				}

				if (a < GPU_MAX_ATTRIB) {
					if (a == attribs->totlayer) {
						input->attribid = attribs->totlayer++;
						input->attribfirst = 1;

						attribs->layer[a].type = input->attribtype;
						attribs->layer[a].attribid = input->attribid;
						BLI_strncpy(attribs->layer[a].name, input->attribname,
						            sizeof(attribs->layer[a].name));
					}
					else {
						input->attribid = attribs->layer[a].attribid;
					}
				}
			}
		}
	}
}

static void gpu_nodes_get_builtin_flag(ListBase *nodes, int *builtin)
{
	GPUNode *node;
	GPUInput *input;

	*builtin = 0;

	for (node = nodes->first; node; node = node->next)
		for (input = node->inputs.first; input; input = input->next)
			if (input->source == GPU_SOURCE_BUILTIN)
				*builtin |= input->builtin;
}

/* varargs linking  */

GPUNodeLink *GPU_attribute(const CustomDataType type, const char *name)
{
	GPUNodeLink *link = GPU_node_link_create();

	link->attribtype = type;
	link->attribname = name;

	return link;
}

GPUNodeLink *GPU_uniform(float *num)
{
	GPUNodeLink *link = GPU_node_link_create();

	link->ptr1 = num;
	link->ptr2 = NULL;
	link->dynamic = false;
	link->dynamictype = 0;  /* No specific type for generic uniforms */

	return link;
}

/* Extended version that accepts dynamictype for tracking material properties */
GPUNodeLink *GPU_uniform_tracked(float *num, GPUDynamicType dynamictype)
{
	GPUNodeLink *link = GPU_node_link_create();

	link->ptr1 = num;
	link->ptr2 = NULL;
	link->dynamic = false;  /* Not a runtime dynamic uniform, but we track the type */
	link->dynamictype = dynamictype;

	return link;
}

GPUNodeLink *GPU_dynamic_uniform(void *num, GPUDynamicType dynamictype, void *data)
{
	GPUNodeLink *link = GPU_node_link_create();

	link->ptr1 = num;
	link->ptr2 = data;
	link->dynamic = true;
	link->dynamictype = dynamictype;


	return link;
}

GPUNodeLink *GPU_select_uniform(float *num, GPUDynamicType dynamictype, void *data, Material *material)
{
	/* Skip lamp uniforms if using new UBO lighting system */
	if (USE_UBO_LIGHTING_SYSTEM && GPU_DYNAMIC_GROUP_FROM_TYPE(dynamictype) == GPU_DYNAMIC_GROUP_LAMP) {
		/* Return a dummy uniform to avoid breaking the node graph */
		return GPU_uniform(num);
	}
	
	bool dynamic = false;
	if (GPU_DYNAMIC_GROUP_FROM_TYPE(dynamictype) == GPU_DYNAMIC_GROUP_MAT) {
		dynamic = !(material->constflag & MA_CONSTANT_MATERIAL);
	}
	else if (GPU_DYNAMIC_GROUP_FROM_TYPE(dynamictype) == GPU_DYNAMIC_GROUP_LAMP) {
		dynamic = !(material->constflag & MA_CONSTANT_LAMP);
	}
	else if (GPU_DYNAMIC_GROUP_FROM_TYPE(dynamictype) == GPU_DYNAMIC_GROUP_TEX) {
		dynamic = !(material->constflag & MA_CONSTANT_TEXTURE);
	}
	else if (GPU_DYNAMIC_GROUP_FROM_TYPE(dynamictype) == GPU_DYNAMIC_GROUP_TEX_UV) {
		dynamic = !(material->constflag & MA_CONSTANT_TEXTURE_UV);
	}
	else if (GPU_DYNAMIC_GROUP_FROM_TYPE(dynamictype) == GPU_DYNAMIC_GROUP_WORLD) {
		dynamic = !(material->constflag & MA_CONSTANT_WORLD);
	}
	else if (GPU_DYNAMIC_GROUP_FROM_TYPE(dynamictype) == GPU_DYNAMIC_GROUP_MIST) {
		dynamic = !(material->constflag & MA_CONSTANT_MIST);
	}

	if (dynamic) {
		return GPU_dynamic_uniform(num, dynamictype, data);
	}
	else {
		/* Use tracked uniform to preserve dynamictype even for constants */
		return GPU_uniform_tracked(num, dynamictype);
	}
}

GPUNodeLink *GPU_image(Image *ima, ImageUser *iuser, bool is_data)
{
	GPUNodeLink *link = GPU_node_link_create();

	link->image = GPU_NODE_LINK_IMAGE_BLENDER;
	link->ptr1 = ima;
	link->ptr2 = iuser;
	link->image_isdata = is_data;

	return link;
}

GPUNodeLink *GPU_cube_map(Image *ima, ImageUser *iuser, bool is_data)
{
	GPUNodeLink *link = GPU_node_link_create();

	link->image = GPU_NODE_LINK_IMAGE_CUBE_MAP;
	link->ptr1 = ima;
	link->ptr2 = iuser;
	link->image_isdata = is_data;

	return link;
}

GPUNodeLink *GPU_image_preview(PreviewImage *prv)
{
	GPUNodeLink *link = GPU_node_link_create();

	link->image = GPU_NODE_LINK_IMAGE_PREVIEW;
	link->ptr1 = prv;

	return link;
}


GPUNodeLink *GPU_texture(int size, float *pixels)
{
	GPUNodeLink *link = GPU_node_link_create();

	link->texture = true;
	link->texturesize = size;
	link->ptr1 = pixels;

	return link;
}

GPUNodeLink *GPU_dynamic_texture(GPUTexture *tex, GPUDynamicType dynamictype, void *data)
{
	GPUNodeLink *link = GPU_node_link_create();

	link->dynamic = true;
	link->dynamictex = tex;
	link->dynamictype = dynamictype;
	link->ptr2 = data;

	return link;
}

GPUNodeLink *GPU_dynamic_texture_ptr(GPUTexture **tex, GPUDynamicType dynamictype, void *data)
{
	GPUNodeLink *link = GPU_node_link_create();

	link->dynamic = true;
	link->dynamictexptr = tex;
	link->dynamictype = dynamictype;
	link->ptr2 = data;

	return link;
}

GPUNodeLink *GPU_builtin(GPUBuiltin builtin)
{
	GPUNodeLink *link = GPU_node_link_create();

	link->builtin = builtin;

	return link;
}

GPUNodeLink *GPU_opengl_builtin(GPUOpenGLBuiltin builtin)
{
	GPUNodeLink *link = GPU_node_link_create();

	link->oglbuiltin = builtin;

	return link;
}

bool GPU_link(GPUMaterial *mat, const char *name, ...)
{
	GPUNode *node;
	GPUFunction *function;
	GPUNodeLink *link, **linkptr;
	va_list params;
	int i;

	function = gpu_lookup_function(name);
	if (!function) {
		fprintf(stderr, "GPU failed to find function %s\n", name);
		return false;
	}

	node = GPU_node_begin(name);

	va_start(params, name);
	for (i = 0; i < function->totparam; i++) {
		if (function->paramqual[i] != FUNCTION_QUAL_IN) {
			linkptr = va_arg(params, GPUNodeLink **);
			gpu_node_output(node, function->paramtype[i], linkptr);
		}
		else {
			link = va_arg(params, GPUNodeLink *);
			gpu_node_input_link(node, link, function->paramtype[i]);
		}
	}
	va_end(params);

	gpu_material_add_node(mat, node);

	return true;
}

bool GPU_stack_link(GPUMaterial *mat, const char *name, GPUNodeStack *in, GPUNodeStack *out, ...)
{
	GPUNode *node;
	GPUFunction *function;
	GPUNodeLink *link, **linkptr;
	va_list params;
	int i, totin, totout;

	function = gpu_lookup_function(name);
	if (!function) {
		fprintf(stderr, "GPU failed to find function %s\n", name);
		return false;
	}

	node = GPU_node_begin(name);
	totin = 0;
	totout = 0;

	if (in) {
		for (i = 0; in[i].type != GPU_NONE; i++) {
			gpu_node_input_socket(node, &in[i]);
			totin++;
		}
	}

	if (out) {
		for (i = 0; out[i].type != GPU_NONE; i++) {
			gpu_node_output(node, out[i].type, &out[i].link);
			totout++;
		}
	}

	va_start(params, out);
	for (i = 0; i < function->totparam; i++) {
		if (function->paramqual[i] != FUNCTION_QUAL_IN) {
			if (totout == 0) {
				linkptr = va_arg(params, GPUNodeLink **);
				gpu_node_output(node, function->paramtype[i], linkptr);
			}
			else
				totout--;
		}
		else {
			if (totin == 0) {
				link = va_arg(params, GPUNodeLink *);
				if (link->socket)
					gpu_node_input_socket(node, link->socket);
				else
					gpu_node_input_link(node, link, function->paramtype[i]);
			}
			else
				totin--;
		}
	}
	va_end(params);

	gpu_material_add_node(mat, node);

	return true;
}

int GPU_link_changed(GPUNodeLink *link)
{
	GPUNode *node;
	GPUInput *input;
	const char *name;

	if (link->output) {
		node = link->output->node;
		name = node->name;

		if (STREQ(name, "set_value") || STREQ(name, "set_rgb")) {
			input = node->inputs.first;
			return (input->link != NULL);
		}

		return 1;
	}
	else
		return 0;
}

/* Pass create/free */

static void gpu_nodes_tag(GPUNodeLink *link)
{
	GPUNode *node;
	GPUInput *input;

	if (!link->output)
		return;

	node = link->output->node;
	if (node->tag)
		return;

	node->tag = true;
	for (input = node->inputs.first; input; input = input->next)
		if (input->link)
			gpu_nodes_tag(input->link);
}

static void gpu_nodes_prune(ListBase *nodes, GPUNodeLink *outlinks[8])
{
	GPUNode *node, *next;

	for (node = nodes->first; node; node = node->next)
		node->tag = false;

	for (unsigned short i = 0; i < 8; ++i) {
		if (outlinks[i]) {
			gpu_nodes_tag(outlinks[i]);
		}
	}

	for (node = nodes->first; node; node = next) {
		next = node->next;

		if (!node->tag) {
			BLI_remlink(nodes, node);
			gpu_node_free(node);
		}
	}
}

GPUPass *GPU_generate_pass(
        ListBase *nodes, GPUNodeLink *outlinks[8],
        GPUVertexAttribs *attribs, int *builtins,
        const GPUMatType type, const char *UNUSED(name),
        const bool use_opensubdiv,
        const bool use_instancing,
        const bool use_new_shading,
        const char *custom_shader,
        const char *custom_fragment_shader,
        const bool use_ubo_lighting,
        const bool custom_sky_override)
{
    GPUShader *shader;
    GPUPass *pass;
    char *vertexcode = NULL, *geometrycode = NULL, *fragmentcode = NULL, *computecode = NULL;

    /* prune unused nodes */
    gpu_nodes_prune(nodes, outlinks);

    gpu_nodes_get_vertex_attributes(nodes, attribs);
    gpu_nodes_get_builtin_flag(nodes, builtins);
    
    if (use_ubo_lighting) {
        *builtins |= GPU_INVERSE_VIEW_MATRIX;
        *builtins |= GPU_VIEW_POSITION;
        *builtins |= GPU_VIEW_NORMAL;
        *builtins |= GPU_TIME;
    }

    const bool is_world = (type == GPU_MATERIAL_TYPE_WORLD);

    /* Sky custom shader needs TIME and inverse-view-matrix builtins.
     * GPU_INVERSE_VIEW_MATRIX must be in the bitmask so that gpu_material.c
     * registers the uniform location and uploads the matrix every frame.
     * Without this the 'unfinvviewmat' uniform stays zero → WORLD_VIEW_DIR is black. */
    if (is_world && custom_fragment_shader && custom_fragment_shader[0] != '\0') {
        *builtins |= GPU_TIME;
        *builtins |= GPU_INVERSE_VIEW_MATRIX;
    }

    /* generate code and compile with OpenGL */
    fragmentcode = code_generate_fragment(nodes, outlinks, custom_fragment_shader, use_ubo_lighting, is_world, custom_sky_override);
    vertexcode   = code_generate_vertex(nodes, type, use_instancing, custom_shader);
    geometrycode = code_generate_geometry(nodes, use_opensubdiv);
    
    /* Save world/sky shader to file if it's a world material */
    if (type == GPU_MATERIAL_TYPE_WORLD && fragmentcode) {
        char filepath[1024];
        #ifdef WIN32
            char exepath[512];
            GetModuleFileNameA(NULL, exepath, sizeof(exepath));
            char *lastslash = strrchr(exepath, '\\');
            if (lastslash) *lastslash = '\0';
            BLI_snprintf(filepath, sizeof(filepath), "%s\\sky_shader.glsl", exepath);
        #else
            BLI_snprintf(filepath, sizeof(filepath), "./sky_shader.glsl");
        #endif
        
        FILE *f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "// Sky/World Fragment Shader - Generated at runtime\n\n");
            fprintf(f, "%s", fragmentcode);
            fclose(f);
            printf("Sky shader saved to: %s\n", filepath);
        }
        else {
            printf("Warning: Could not save sky shader to: %s\n", filepath);
        }
    }

    int flags = GPU_SHADER_FLAGS_NONE;
    if (use_opensubdiv) {
        flags |= GPU_SHADER_FLAGS_SPECIAL_OPENSUBDIV;
    }
    if (use_new_shading) {
        flags |= GPU_SHADER_FLAGS_NEW_SHADING;
    }
    if (use_instancing) {
        flags |= GPU_SHADER_FLAGS_SPECIAL_INSTANCING;
    }

    shader = GPU_shader_create_ex(
        vertexcode,
        fragmentcode,
        geometrycode,
        glsl_material_library,
        NULL,
        0,
        0,
        0,
        flags);

    /* failed? */
    if (!shader) {
        if (fragmentcode)
            MEM_freeN(fragmentcode);
        if (vertexcode)
            MEM_freeN(vertexcode);
        if (computecode)
            MEM_freeN(computecode);
        memset(attribs, 0, sizeof(*attribs));
        memset(builtins, 0, sizeof(*builtins));
        gpu_nodes_free(nodes);
        return NULL;
    }

    /* create pass */
    pass = MEM_callocN(sizeof(GPUPass), "GPUPass");

    pass->shader       = shader;
    pass->fragmentcode = fragmentcode;
    pass->geometrycode = geometrycode;
    pass->vertexcode   = vertexcode;
    pass->libcode      = glsl_material_library;

    /* extract dynamic inputs and throw away nodes */
    gpu_nodes_extract_dynamic_inputs(pass, nodes);
    gpu_nodes_free(nodes);

    return pass;
}


void GPU_pass_free(GPUPass *pass)
{
	GPU_shader_free(pass->shader);
	gpu_inputs_free(&pass->inputs);
	if (pass->fragmentcode)
		MEM_freeN(pass->fragmentcode);
	if (pass->geometrycode)
		MEM_freeN(pass->geometrycode);
	if (pass->vertexcode)
		MEM_freeN(pass->vertexcode);
}


void GPU_pass_free_nodes(ListBase *nodes)
{
	gpu_nodes_free(nodes);
}
