// Sky/World Fragment Shader - Generated at runtime

in vec3 varposition;
uniform vec3 unf5;
uniform vec3 unf7;

uniform vec3 u_MatColor;
uniform float u_MatAlpha;
uniform float u_MatSpec;
uniform float u_MatRoughness;
uniform float u_MatMetallic;
uniform float u_MatEmission;
/* Custom Sky Global Code */

const float density              = 0.78;
const float multiScatterPhase    = 0.5;
const float anisotropicIntensity = 0.22;

#define pi 3.14159265359
#define invPi 1.0 / pi

#define smooth(x) x * x * (3.0 - 2.0 * x)
#define zenithDensity(x) density / pow(max(x, 0.35e-2), 0.75)

vec3 getSkyAbsorption(vec3 x, float y) {
    vec3 absorption = -y * x;
    absorption = exp2(absorption) * 2.0;

    return absorption;
}

float getSunPoint(vec3 p, vec3 lp) {
    return smoothstep(0.03, 0.026, distance(p, lp)) * 50.0;
}

float getRayleigMultiplier(vec3 p, vec3 lp) {
    return 1.0 + pow(1.0 - clamp(distance(p, lp), 0.0, 1.0), 2.0) * pi * 0.5;
}

float getMie(vec3 p, vec3 lp) {
    float disk = clamp(1.0 - pow(distance(p, lp), 0.1), 0.0, 1.0);

    return disk * disk * (3.0 - 2.0 * disk) * 2.0 * pi;
}

vec3 getAtmosphericScattering(vec3 p, vec3 lp, vec3 skyColor) {
    float zenith = zenithDensity(p.z);
    float sunPointDistMult = clamp(length(max(lp.z + multiScatterPhase, 0.0)), 0.0, 1.0);

    float rayleighMult = getRayleigMultiplier(p, lp);

    vec3 absorption = getSkyAbsorption(skyColor, zenith);
    vec3 sunAbsorption = getSkyAbsorption(skyColor, zenithDensity(lp.z + multiScatterPhase));

    vec3 sky = skyColor * zenith * rayleighMult;
    vec3 sun = getSunPoint(p, lp) * absorption;
    vec3 mie = getMie(p, lp) * sunAbsorption;

    vec3 totalSky = mix(sky * absorption, sky / (sky + 0.5), sunPointDistMult);
    totalSky += sun + mie;
    totalSky *= sunAbsorption * 0.5 + 0.5 * length(sunAbsorption);

    return totalSky / 10.0;
}


uniform float unftime;
uniform mat4 unfobmat;
uniform mat4 unfviewmat;
uniform mat4 unfinvviewmat;
uniform vec4 unfobcolor;


void main()
{
	vec3 tmp2;
	float tmp4;
	vec3 tmp6;
	vec3 tmp8;
	vec4 tmp12;
	vec3 tmp14;
	vec4 tmp16;

	/* Custom Sky Shader — intermediate variables */
	vec3 HORIZON_COLOR = unf5;
	vec3 ZENITH_COLOR = unf7;
	vec3 VIEW_DIR = normalize(varposition);
	vec3 WORLD_VIEW_DIR;
	{
		vec4 _wvd_v = (gl_ProjectionMatrix[3][3] == 0.0) ? vec4(varposition, 1.0) : vec4(0.0, 0.0, 1.0, 1.0);
		vec4 _wvd_h = gl_ProjectionMatrixInverse * _wvd_v;
		WORLD_VIEW_DIR = normalize((gl_ModelViewMatrixInverse * vec4(_wvd_h.xyz / _wvd_h.w, 0.0)).xyz);
	}
	float ENV_ENERGY = 1.0;
	float TIME = unftime;
	vec3 SKY_COLOR = vec3(0.0);
	/* Custom Sky Body (override) */

    vec3 p = normalize(WORLD_VIEW_DIR);

    vec3 ldir = vec3(0.0, 1.0, 0.0);

    for (int i = 0; i < 32; ++i) {
        if (sceneLights[i].type_mode.x == 1.0) {
            ldir = -(sceneLights[i].spotDirection.xyz);
            break;
        }
    }

    const vec3 skyColor = ZENITH_COLOR * (1.0 + anisotropicIntensity);
    vec3 col = getAtmosphericScattering(p, ldir, skyColor) * pi;

    SKY_COLOR = col;

	gl_FragColor = vec4(SKY_COLOR, 1.0);
}
