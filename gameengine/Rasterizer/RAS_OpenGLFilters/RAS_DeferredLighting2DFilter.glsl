
uniform sampler2D bgl_DepthTexture;

// albedo, spec, metalic, emit
uniform sampler2D bgl_DataTextures[7];

float lightType(int i) {return gl_LightSource[i].position.w;}
float lightCut(int i) {return gl_LightSource[i].spotCutoff;}
float lightCos(int i) {return gl_LightSource[i].spotCosCutoff;}
float lightExp(int i) {return gl_LightSource[i].spotExponent;}

vec3 lightPos(int i) {return gl_LightSource[i].position.xyz;}
vec3 lightDir(int i) {return gl_LightSource[i].spotDirection.xyz;}
vec3 lightCol(int i) {return gl_LightSource[i].diffuse.xyz;}

vec3 getViewPos(vec2 coord) {
	float depth = texture(bgl_DepthTexture, coord).x;

	vec3 ndc = vec3(coord, depth) * 2.0 - 1.0;

	vec4 view = inverse(gl_ProjectionMatrix) * vec4(ndc, 1.0);
	view.xyz /= view.w;

	return view.xyz;
}

float inverseSquare(vec3 L, float D) {
	float dist = length(L) / D;

	return 1.0 + dist * dist;
}

vec3 getLighting(vec3 P, vec3 N, vec3 A) {
	vec3 V = -normalize(P);

	vec3 result = vec3(0.0);
	vec3 lpos = vec3(0.0);
	float att = 1.0;

	for (int i = 0; i < gl_MaxLights; i++) {
		float type = lightType(i);
	
		if (0.0 == type) {
			lpos = normalize(lightPos(i));
			att = 1.0;

		} else if (0.0 != type && lightCut(i) > 90.0) {
			lpos = lightPos(i) - P;

			att /= inverseSquare(lpos, 25.0);

		} else if (0.0 != type && lightCut(i) <= 90.0) {
			lpos = lightPos(i) - P;

			float cosTheta = dot(-normalize(lpos), lightDir(i));
			float cutoffCos = cos(lightCut(i));
			float expBlend = lightExp(i) / 180.0;

			if (cosTheta > cutoffCos){
				cosTheta *= pow(cosTheta - cutoffCos, expBlend);

				att *= cosTheta;
			} else {
				att = 0.0;
			}

			att /= inverseSquare(lpos, 25.0);
		}

		vec3 L = normalize(lpos);

		vec3 H = normalize(N + V);

		float dotNL = max(0.0, dot(L, N));
		float dotHN = max(0.0, dot(H, N));

		vec3 diffuse  = lightCol(i) * A * sqrt(dotNL);
		vec3 specular = lightCol(i) * pow(dotHN, 50.0) / (1.0 + dotNL);

		result += (diffuse + specular) * att;
	}

	return result;
}

void main()
{
	vec2 uv = gl_TexCoord[0].st;

	vec4 albe  = texture2D(bgl_DataTextures[0], uv);
	//vec4 spec  = texture2D(bgl_DataTextures[1], uv);
	//vec4 metal = texture2D(bgl_DataTextures[2], uv);

	vec3 pos  = getViewPos(uv);
	vec3 norm = normalize(cross(dFdx(pos), dFdy(pos)));

	vec3 final = getLighting(pos, norm, albe.rgb);

	gl_FragColor = vec4(final, 1.0);
}
