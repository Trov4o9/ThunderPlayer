
#define SPOT 0
#define SUN 1
#define POINT 2
//vai loco krl
#define inverse_linear

#define PI 3.14159

layout(std140, binding=1) uniform SceneLightBlock {
	vec4 sceneLightInfo; 

	struct {
		vec4 type_mode; 
		vec4 color_energy;
		vec4 position; 
		vec4 spotDirection;
		vec4 attenuation;
	} sceneLights[32];
};

float falloff_type(vec3 p, float d)
{
	float dist = length(p) / max(d, 0.0001);

	#if inverse_linear
		return 1.0 + dist;
	#elif inverse_square
		return 1.0 + dist * dist;
	#else
		return 1.0;
	#endif
}

float G1V(float NdotV, float k)
{
	return 1.0 / (NdotV * (1.0 - k) + k);
}

vec3 specular_ggx(
	float NdotL,
	float NdotV,
	float NdotH,
	float LdotH,
	float roughness,
	vec3 F0)
{
	float alpha = roughness * roughness;
	float alphaSqr = alpha * alpha;

	float denom = NdotH * NdotH *(alphaSqr - 1.0) + 1.0;
	float D = alphaSqr / (PI * denom * denom);

	float LdotH5 = pow(1.0 - LdotH,5);
	vec3 F = F0 + (1.0 - F0) * (LdotH5);

	float k = alpha / 2.0;
	float G = G1V(NdotL, k) * G1V(NdotV, k);

	return D * F * G;
}

void calcLight(
	vec3 pos,
	vec3 norm,
	vec3 view,

	vec3 albedo,
	vec3 specular_rgb,
	vec3 scatter_rgb,

	float roughness,
	float metallic,
	float scatter_fac,

	inout vec3 result)
{
	vec3 acc = vec3(0.0);

	for (int i = 0; i < 32; i++) {
		vec3 L = vec3(0.0);
		float att = 1.0;

		float type = sceneLights[i].type_mode.x;

		if (type == SUN) {
			L = -sceneLights[i].spotDirection.xyz;
			att = 1.0;

		} else {
			vec3 delta = sceneLights[i].position.xyz - pos;
			float dist = sceneLights[i].attenuation.x;

			L = normalize(delta);
			att = 1.0 / falloff_type(delta, dist);

			if (type == SPOT) {

				float theta = max(0.0, dot(-L, sceneLights[i].spotDirection.xyz));

				/* cone temporariamente removido
				att *= smoothstep(lightOuter, lightInner, theta);
				*/
			}
		}

		vec3 N = normalize(norm);
		vec3 V = normalize(view);
		vec3 H = normalize(V + L);

		float NdotL = max(dot(N, L), 0.0);
		float NdotV = max(dot(N, V), 0.0);
		float NdotH = max(dot(N, H), 0.0);
		float VdotH = max(dot(V, H), 0.0);
		float LdotH = max(dot(L, H), 0.0);

		if (NdotL <= 0.0 || NdotV <= 0.0)
			continue;

		vec3 F0 = mix(specular_rgb, albedo, metallic);

		vec3 F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
		vec3 kd = (1.0 - F) * (1.0 - metallic);

		float FL = pow(1.0 - NdotL, 5.0);
		float FV = pow(1.0 - NdotV, 5.0);

		float Fss90 = VdotH * VdotH * roughness;
		float Fss = mix(1.0, Fss90, FL) * mix(1.0, Fss90, FV);
		float ss = 1.25 * (Fss * (1.0 / max(NdotL + NdotV, 0.001) - 0.5) + 0.5);

		vec3 scatter = scatter_rgb * ss / PI;

		vec3 diffuse  = albedo / PI;
		diffuse = kd * mix(diffuse, scatter, scatter_fac);

		vec3 specular = specular_ggx(NdotL, NdotV, NdotH, LdotH, roughness, F0);

		acc += sceneLights[i].color_energy.rgb * (diffuse + specular) * NdotL * att;
	}

	result += acc;
}
