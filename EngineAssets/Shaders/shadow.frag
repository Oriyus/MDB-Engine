#version 450 core

in vec3 v_WorldPos;
in vec2 v_TexCoord;
flat in float v_Alpha;      // per instance

uniform vec3  u_LightPos;
uniform float u_FarPlane;

// See-through surfaces cast shadows only where alpha >= the cutoff
uniform sampler2D u_Texture;    // unit 0: albedo
uniform int   u_UseTexture;
uniform int   u_AlphaTest;

uniform float u_AlphaCutoff;

// @GRAPH_CODE
#ifdef MDB_GRAPH
uniform float u_SceneWindTime;
float GraphAlpha(vec3 worldPos) {   // a shader graph material's alpha (no normal or view here: they read "up")
    SGIn gIn;
    gIn.UV = v_TexCoord; gIn.WorldPos = worldPos; gIn.WorldNormal = vec3(0.0, 1.0, 0.0);
    gIn.ViewDir = vec3(0.0, 1.0, 0.0); gIn.Time = u_SceneWindTime; gIn.ScreenUV = vec2(0.5);
    return GraphSurface(gIn).Alpha;
}
#endif

void main() {
    if (u_AlphaTest == 1) {
#ifdef MDB_GRAPH
        float a = v_Alpha * GraphAlpha(v_WorldPos);
#else
        float a = v_Alpha * (u_UseTexture == 1 ? texture(u_Texture, v_TexCoord).a : 1.0);
#endif
        if (a < u_AlphaCutoff) discard;
    }
    gl_FragDepth = length(v_WorldPos - u_LightPos) / u_FarPlane;
}
