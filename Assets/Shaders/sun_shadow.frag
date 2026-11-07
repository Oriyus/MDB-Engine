#version 450 core

in vec2 v_TexCoord;

// See-through surfaces cast shadows only where alpha >= the cutoff
uniform sampler2D u_Texture;    // unit 0: albedo
uniform int   u_UseTexture;
uniform int   u_AlphaTest;
uniform float u_Alpha;
uniform float u_AlphaCutoff;

void main() {
    if (u_AlphaTest == 1) {
        float a = u_Alpha * (u_UseTexture == 1 ? texture(u_Texture, v_TexCoord).a : 1.0);
        if (a < u_AlphaCutoff) discard;
    }
}
