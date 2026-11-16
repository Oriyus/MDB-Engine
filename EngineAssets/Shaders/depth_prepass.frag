#version 450 core

// Depth pre-pass (SceneRenderer): the opaque scene and the grass are drawn once with this, depth only, so the lit
// pass that follows shades each pixel once (depth test LEQUAL, no depth writes) instead of once per overlapping
// leaf card or grass blade. Linked with lit.vert and with foliage.vert; colour writes are masked off.
// A shader graph material that writes alpha is drawn with its own copy of this (MDB_GRAPH), cut by the graph's alpha.

in vec2 v_TexCoord;
in vec4 v_Color;

uniform sampler2D u_Texture;   // unit 0
uniform int   u_UseTexture;
uniform int   u_AlphaMode;     // 1 = cutout: the cut-away parts must not hide what is behind them
uniform float u_AlphaCutoff;

#ifdef MDB_GRAPH
in vec3 v_WorldPos;
in vec3 v_Normal;
in vec4 v_CurClip;
uniform vec3 u_ViewPos;
uniform float u_SceneWindTime;
#endif
// @GRAPH_CODE

void main() {
    if (u_AlphaMode == 1) {
#ifdef MDB_GRAPH
        SGIn gIn;
        gIn.UV = v_TexCoord; gIn.WorldPos = v_WorldPos; gIn.WorldNormal = normalize(v_Normal);
        gIn.ViewDir = normalize(u_ViewPos - v_WorldPos); gIn.Time = u_SceneWindTime;
        gIn.ScreenUV = v_CurClip.xy / v_CurClip.w * 0.5 + 0.5;
        float a = v_Color.a * GraphSurface(gIn).Alpha;
#else
        float a = v_Color.a * (u_UseTexture == 1 ? texture(u_Texture, v_TexCoord).a : 1.0);
#endif
        if (a < u_AlphaCutoff) discard;
    }
}
