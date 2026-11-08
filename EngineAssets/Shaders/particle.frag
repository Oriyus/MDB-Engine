#version 450 core

in vec2 v_UV;
in vec4 v_Color;

layout(location = 0) out vec4 FragColor;

uniform sampler2D u_Texture;   // unit 0, sRGB (decoded to linear by the GPU)
uniform int u_UseTexture;      // 0: a soft round dot

void main() {
    vec3 rgb = v_Color.rgb;
    float a;
    if (u_UseTexture == 1) {
        vec4 t = texture(u_Texture, v_UV);
        rgb *= t.rgb;
        a = t.a;
    } else {
        float d = length(v_UV * 2.0 - 1.0);
        a = 1.0 - smoothstep(0.0, 1.0, d);
        a *= a;                    // a soft falloff to the edge
    }
    a *= v_Color.a;
    if (a < 0.002) discard;
    // Premultiplied: additive blending (ONE, ONE) ignores alpha; "over" (ONE, 1 - alpha) uses it
    FragColor = vec4(rgb * a, a);
}
