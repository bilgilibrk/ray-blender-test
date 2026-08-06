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
    // The shadow compare comes out of a perspective divide near 1.0 and is
    // tested against a bias of about 0.0016, which is finer than mediump can
    // resolve there. Ask for highp where the hardware has it.
    #define GLSL_FRAG_HEADER "#version 100\n" \
                             "#ifdef GL_FRAGMENT_PRECISION_HIGH\n" \
                             "precision highp float;\n" \
                             "#else\n" \
                             "precision mediump float;\n" \
                             "#endif\n"
    #define GLSL_ATTRIBUTE   "attribute"
    #define GLSL_VARY_OUT    "varying"
    #define GLSL_VARY_IN     "varying"
    #define GLSL_FRAG_DECL   ""
    #define GLSL_FRAG_COLOR  "gl_FragColor"
    #define GLSL_TEXTURE     "texture2D"
#else
    #define GLSL_VERT_HEADER "#version 330\n"
    #define GLSL_FRAG_HEADER "#version 330\n"
    #define GLSL_ATTRIBUTE   "in"
    #define GLSL_VARY_OUT    "out"
    #define GLSL_VARY_IN     "in"
    #define GLSL_FRAG_DECL   "out vec4 finalColor;\n"
    #define GLSL_FRAG_COLOR  "finalColor"
    #define GLSL_TEXTURE     "texture"
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
    // World space to the sun's clip space, for the shadow lookup.
    "uniform mat4 lightVP;\n"
    "uniform sampler2D shadowMap;\n"
    "uniform float shadowTexel;\n"       // 1/resolution, for the PCF taps
    "uniform float shadowStrength;\n"    // 0 skips the lookup entirely
    GLSL_FRAG_DECL
    // How much of the sun reaches this fragment: 1 fully lit, 0 fully shadowed.
    // Only the key light is occluded — ambient, the sky term and the placed
    // lamps all still reach into the shadow, which is what keeps it readable
    // rather than a black hole.
    "float SunReach(vec3 n, vec3 l) {\n"
    "    if (shadowStrength <= 0.0) return 1.0;\n"
    "    vec4 lp = lightVP*vec4(fragWorld, 1.0);\n"
    "    vec3 proj = lp.xyz/lp.w*0.5 + 0.5;\n"
    // Outside the map the sun is unoccluded: the box only covers the play area
    // around the camera, and anything beyond it must not be shadowed by chance.
    "    if (proj.z > 1.0 || proj.x < 0.0 || proj.x > 1.0 ||\n"
    "        proj.y < 0.0 || proj.y > 1.0) return 1.0;\n"
    // Slope-scaled bias: a surface edge-on to the sun crosses many depth texels
    // within one of its own, and needs far more slack than one facing it. The
    // floor is set above the 0.06-unit lip where the ground meets the tarmac,
    // which is otherwise close enough to the bias to strobe in and out of
    // shadow along every kerb in the level.
    "    float bias = max(0.0035*(1.0 - dot(n, l)), 0.0016);\n"
    "    float lit = 0.0;\n"
    // 3x3 PCF, which softens the edge enough to hide the texel grid.
    "    for (int x = -1; x <= 1; x++) {\n"
    "        for (int y = -1; y <= 1; y++) {\n"
    "            vec2 at = proj.xy + vec2(float(x), float(y))*shadowTexel;\n"
    "            float d = " GLSL_TEXTURE "(shadowMap, at).r;\n"
    "            lit += (proj.z - bias > d) ? 0.0 : 1.0;\n"
    "        }\n"
    "    }\n"
    "    return mix(1.0, lit/9.0, shadowStrength);\n"
    "}\n"
    "void main() {\n"
    "    vec3 n = normalize(fragNormal);\n"
    "    vec3 sunL = -normalize(sunDir);\n"
    "    float ndl = max(dot(n, sunL), 0.0);\n"
    // Ambient, key light, and a weak sky term so upward faces lift a little.
    "    vec3 lighting = ambient.rgb + sunColor.rgb*sunColor.a*ndl*SunReach(n, sunL)\n"
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

// ---------------------------------------------------------------------------
// Depth-only pass, used to fill the shadow map. There is no colour attachment
// to write to, so the fragment shader exists purely to be a legal one.
// ---------------------------------------------------------------------------

static const char *kDepthVertexShader =
    GLSL_VERT_HEADER
    GLSL_ATTRIBUTE " vec3 vertexPosition;\n"
    "uniform mat4 mvp;\n"
    "void main() {\n"
    "    gl_Position = mvp*vec4(vertexPosition, 1.0);\n"
    "}\n";

static const char *kDepthFragmentShader =
    GLSL_FRAG_HEADER
    GLSL_FRAG_DECL
    "void main() {\n"
    "    " GLSL_FRAG_COLOR " = vec4(1.0);\n"
    "}\n";

