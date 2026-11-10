#version 450 core

// Depth pre-pass (SceneRenderer): the opaque scene and the grass are drawn once with this, depth only, so the lit
// pass that follows shades each pixel once (depth test LEQUAL, no depth writes) instead of once per overlapping
// leaf card or grass blade. Linked with lit.vert and with foliage.vert; colour writes are masked off.

in vec2 v_TexCoord;
in vec4 v_Color;

uniform sampler2D u_Texture;   // unit 0
uniform int   u_UseTexture;
uniform int   u_AlphaMode;     // 1 = cutout: the cut-away parts must not hide what is behind them
uniform float u_AlphaCutoff;

void main() {
    if (u_AlphaMode == 1) {
        float a = v_Color.a * (u_UseTexture == 1 ? texture(u_Texture, v_TexCoord).a : 1.0);
        if (a < u_AlphaCutoff) discard;
    }
}
