#version 450 core

in vec2 v_UV;
in vec4 v_Color;

layout(location = 0) out vec4 FragColor;

uniform sampler2D u_Texture;   // unit 0, sRGB (decoded to linear by the GPU)
uniform int u_UseTexture;      // 0: a soft round dot

// Soft particles: a copy of the opaque scene's depth (unit 1) and the camera projection to compare distances
uniform sampler2D u_Depth;
uniform mat4 u_Projection;
uniform float u_Softness;      // metres over which a particle fades as it nears the surface behind it (0 = off)

// A depth-buffer value (0..1) back to the distance in front of the camera (perspective or orthographic)
float ViewDistance(float depth) {
    float ndc = depth * 2.0 - 1.0;
    if (u_Projection[3][3] > 0.5) return -(ndc - u_Projection[3][2]) / u_Projection[2][2];   // orthographic
    return u_Projection[3][2] / (ndc + u_Projection[2][2]);
}

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
    if (u_Softness > 0.0) {
        float behind = ViewDistance(texelFetch(u_Depth, ivec2(gl_FragCoord.xy), 0).r);
        float k = clamp((behind - ViewDistance(gl_FragCoord.z)) / u_Softness, 0.0, 1.0);
        a *= k * k * (3.0 - 2.0 * k);
    }
    if (a < 0.002) discard;
    // Premultiplied: additive blending (ONE, ONE) ignores alpha; "over" (ONE, 1 - alpha) uses it
    FragColor = vec4(rgb * a, a);
}
