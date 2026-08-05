// Scene shader source, shared by the renderer and tools/check_shaders.sh.
//
// Included rather than compiled on its own so the strings stay next to the code
// that binds their uniforms. Requires ENGINE_GLSL_VERSION to be defined as 100
// (GLES2, the Raspberry Pi console build) or 330 (desktop).
#if !defined(ENGINE_GLSL_VERSION)
    #error "define ENGINE_GLSL_VERSION before including scene_shader.inc"
#endif

// ---------------------------------------------------------------------------
// Shaders. Embedded rather than loaded from disk so a level can be rendered
// without any files beyond the models themselves.
//
// The GLSL 330 and GLSL 100 variants share one body; only the qualifiers and
// the output differ. Keeping them as one string stops the desktop and GLES2
// paths from drifting apart.
// ---------------------------------------------------------------------------

#if ENGINE_GLSL_VERSION == 100
    #define GLSL_VERT_HEADER "#version 100\n"
    #define GLSL_FRAG_HEADER "#version 100\nprecision mediump float;\n"
    #define GLSL_ATTRIBUTE   "attribute"
    #define GLSL_VARY_OUT    "varying"
    #define GLSL_VARY_IN     "varying"
    #define GLSL_FRAG_DECL   ""
    #define GLSL_FRAG_COLOR  "gl_FragColor"
#else
    #define GLSL_VERT_HEADER "#version 330\n"
    #define GLSL_FRAG_HEADER "#version 330\n"
    #define GLSL_ATTRIBUTE   "in"
    #define GLSL_VARY_OUT    "out"
    #define GLSL_VARY_IN     "in"
    #define GLSL_FRAG_DECL   "out vec4 finalColor;\n"
    #define GLSL_FRAG_COLOR  "finalColor"
#endif

#define STR(x) #x
#define XSTR(x) STR(x)

static const char *kVertexShader =
    GLSL_VERT_HEADER
    GLSL_ATTRIBUTE " vec3 vertexPosition;\n"
    GLSL_ATTRIBUTE " vec3 vertexNormal;\n"
    GLSL_ATTRIBUTE " vec4 vertexColor;\n"
    "uniform mat4 mvp;\n"
    "uniform mat4 matModel;\n"
    "uniform mat4 matNormal;\n"
    GLSL_VARY_OUT " vec4 fragColor;\n"
    GLSL_VARY_OUT " vec3 fragNormal;\n"
    GLSL_VARY_OUT " vec3 fragWorld;\n"
    "void main() {\n"
    "    fragColor = vertexColor;\n"
    "    fragNormal = normalize(vec3(matNormal*vec4(vertexNormal, 0.0)));\n"
    "    fragWorld = vec3(matModel*vec4(vertexPosition, 1.0));\n"
    "    gl_Position = mvp*vec4(vertexPosition, 1.0);\n"
    "}\n";

static const char *kFragmentShader =
    GLSL_FRAG_HEADER
    "#define MAX_LIGHTS " XSTR(LIGHTS_PER_DRAW) "\n"
    GLSL_VARY_IN " vec4 fragColor;\n"
    GLSL_VARY_IN " vec3 fragNormal;\n"
    GLSL_VARY_IN " vec3 fragWorld;\n"
    "uniform vec4 colDiffuse;\n"
    "uniform vec3 sunDir;\n"
    "uniform vec4 sunColor;\n"        // rgb tint, a = intensity
    "uniform vec4 ambient;\n"
    "uniform vec4 fogColor;\n"
    "uniform float fogDensity;\n"
    "uniform vec3 cameraPos;\n"
    "uniform int lightCount;\n"
    // xyz position, w range
    "uniform vec4 lightPosRange[MAX_LIGHTS];\n"
    // rgb colour premultiplied by intensity, w cos(outer cone) or -2 for a point
    "uniform vec4 lightColor[MAX_LIGHTS];\n"
    // xyz spot direction, w cos(inner cone)
    "uniform vec4 lightDir[MAX_LIGHTS];\n"
    GLSL_FRAG_DECL
    "void main() {\n"
    "    vec3 n = normalize(fragNormal);\n"
    "    float ndl = max(dot(n, -normalize(sunDir)), 0.0);\n"
    // Ambient, key light, and a weak sky term so upward faces lift a little.
    "    vec3 lighting = ambient.rgb + sunColor.rgb*sunColor.a*ndl\n"
    "                  + ambient.rgb*0.35*max(n.y, 0.0);\n"
    "    for (int i = 0; i < MAX_LIGHTS; i++) {\n"
    "        if (i >= lightCount) break;\n"
    "        vec3 toLight = lightPosRange[i].xyz - fragWorld;\n"
    "        float dist = length(toLight);\n"
    "        float range = lightPosRange[i].w;\n"
    "        if (dist >= range) continue;\n"
    "        vec3 L = toLight/max(dist, 0.0001);\n"
    // Squared linear falloff: cheap, and reaches exactly zero at the range.
    "        float atten = 1.0 - dist/range;\n"
    "        atten *= atten;\n"
    "        float cosOuter = lightColor[i].w;\n"
    "        if (cosOuter > -1.5) {\n"
    "            float cd = dot(-L, normalize(lightDir[i].xyz));\n"
    "            float cosInner = lightDir[i].w;\n"
    "            atten *= clamp((cd - cosOuter)/max(cosInner - cosOuter, 0.001), 0.0, 1.0);\n"
    "        }\n"
    "        lighting += lightColor[i].rgb*max(dot(n, L), 0.0)*atten;\n"
    "    }\n"
    "    vec4 base = fragColor*colDiffuse;\n"
    "    vec3 lit = base.rgb*lighting;\n"
    "    float d = length(cameraPos - fragWorld)*fogDensity;\n"
    "    float f = clamp(1.0 - exp(-d*d), 0.0, 1.0);\n"
    "    " GLSL_FRAG_COLOR " = vec4(mix(lit, fogColor.rgb, f), base.a);\n"
    "}\n";

