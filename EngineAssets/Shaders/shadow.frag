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

void main() {
    if (u_AlphaTest == 1) {
        float a = v_Alpha * (u_UseTexture == 1 ? texture(u_Texture, v_TexCoord).a : 1.0);
        if (a < u_AlphaCutoff) discard;
    }
    gl_FragDepth = length(v_WorldPos - u_LightPos) / u_FarPlane;
}
