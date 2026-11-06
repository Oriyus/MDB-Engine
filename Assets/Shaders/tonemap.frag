#version 450 core

in vec2 v_UV;
out vec4 FragColor;

uniform sampler2D u_Scene;
uniform float u_Exposure;
uniform int   u_ToneMap;     // 0 = clamp, 1 = Reinhard, 2 = ACES
uniform int   u_Gamma;       // 1 = encode for the monitor

// Narkowicz's fit of the ACES filmic curve
vec3 ACES(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 hdr = texture(u_Scene, v_UV).rgb * u_Exposure;

    vec3 mapped;
    if      (u_ToneMap == 1) mapped = hdr / (1.0 + hdr);
    else if (u_ToneMap == 2) mapped = ACES(hdr);
    else                     mapped = clamp(hdr, 0.0, 1.0);

    if (u_Gamma == 1) mapped = pow(mapped, vec3(1.0 / 2.2));

    FragColor = vec4(mapped, 1.0);
}