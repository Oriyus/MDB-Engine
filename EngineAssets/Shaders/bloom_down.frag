#version 450 core

in vec2 v_UV;
out vec4 FragColor;

uniform sampler2D u_Src;
uniform int   u_FirstPass;
uniform float u_Threshold;
uniform float u_Knee;
uniform float u_MaxBrightness;

float Luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// Soft threshold with a hard ceiling, so one extremely bright pixel can't dominate a frame
vec3 Prefilter(vec3 c) {
    c = min(c, vec3(u_MaxBrightness));
    float br = max(c.r, max(c.g, c.b));
    float soft = clamp(br - u_Threshold + u_Knee, 0.0, 2.0 * u_Knee);
    soft = soft * soft / (4.0 * u_Knee + 1e-4);
    float contrib = max(soft, br - u_Threshold) / max(br, 1e-4);
    return c * contrib;
}

void main() {
    vec2 texel = 1.0 / vec2(textureSize(u_Src, 0));

    #define TAP(ox, oy) (u_FirstPass == 1 ? Prefilter(texture(u_Src, v_UV + vec2(ox, oy) * texel).rgb) \
                                          : texture(u_Src, v_UV + vec2(ox, oy) * texel).rgb)
    vec3 a = TAP(-2, -2), b = TAP(0, -2), c = TAP(2, -2);
    vec3 d = TAP(-2,  0), e = TAP(0,  0), f = TAP(2,  0);
    vec3 g = TAP(-2,  2), h = TAP(0,  2), i = TAP(2,  2);
    vec3 j = TAP(-1, -1), k = TAP(1, -1), l = TAP(-1, 1), m = TAP(1, 1);

    if (u_FirstPass == 1) {
        vec3 grp[5];
        grp[0] = (a + b + d + e) * 0.25;
        grp[1] = (b + c + e + f) * 0.25;
        grp[2] = (d + e + g + h) * 0.25;
        grp[3] = (e + f + h + i) * 0.25;
        grp[4] = (j + k + l + m) * 0.25;
        const float w[5] = float[](0.125, 0.125, 0.125, 0.125, 0.5);

        vec3 sum = vec3(0.0);
        float wsum = 0.0;
        for (int n = 0; n < 5; n++) {
            float kw = w[n] / (1.0 + Luma(grp[n]));     // bright groups count for less
            sum += grp[n] * kw;
            wsum += kw;
        }
        FragColor = vec4(sum / wsum, 1.0);
    } else {
        vec3 o = e * 0.125
               + (a + c + g + i) * 0.03125
               + (b + d + f + h) * 0.0625
               + (j + k + l + m) * 0.125;
        FragColor = vec4(o, 1.0);
    }
}