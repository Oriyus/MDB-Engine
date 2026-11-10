#version 450 core

// Selection mask (editor outline), linked with lit.vert: the selected parts are drawn into an RG8 target.
// R = visible here (in front of the scene depth), G = hidden behind something. Blended with MAX.

in vec2 v_TexCoord;
in vec4 v_Color;

layout(location = 0) out vec4 Mask;

uniform sampler2D u_Texture;      // unit 0
uniform sampler2D u_SceneDepth;   // unit 8: a copy of the scene depth
uniform int   u_UseTexture;
uniform int   u_AlphaMode;        // 0 opaque, 1 cutout, 2 blend
uniform float u_AlphaCutoff;

void main() {
    if (u_AlphaMode != 0) {
        float a = v_Color.a * (u_UseTexture == 1 ? texture(u_Texture, v_TexCoord).a : 1.0);
        if (a < (u_AlphaMode == 1 ? u_AlphaCutoff : 0.05)) discard;
    }
    // The mask is drawn without the TAA jitter (a steady outline) but the scene depth has it: up to half a pixel
    // apart. So the farthest depth in the 3x3 neighbourhood decides; without TAA the centre alone would do.
    ivec2 p = ivec2(gl_FragCoord.xy), size = textureSize(u_SceneDepth, 0);
    float scene = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
            scene = max(scene, texelFetch(u_SceneDepth, clamp(p + ivec2(x, y), ivec2(0), size - 1), 0).r);
    bool visible = gl_FragCoord.z <= scene + 2e-6;
    Mask = visible ? vec4(1.0, 0.0, 0.0, 1.0) : vec4(0.0, 1.0, 0.0, 1.0);
}
