#version 450 core

in vec3 v_Dir;
out vec4 FragColor;

uniform samplerCube u_Env;
uniform float u_Roughness;
uniform float u_Resolution;    // face size of the environment cubemap

const float PI = 3.14159265359;
const uint SAMPLES = 1024u;

vec2 Hammersley(uint i, uint n) {
    return vec2(float(i) / float(n), float(bitfieldReverse(i)) * 2.3283064365386963e-10);
}

vec3 ImportanceSampleGGX(vec2 Xi, vec3 N, float rough) {
    float a = rough * rough;
    float phi = 2.0 * PI * Xi.x;
    float cosT = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinT = sqrt(1.0 - cosT * cosT);
    vec3 H = vec3(cos(phi) * sinT, sin(phi) * sinT, cosT);

    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 T = normalize(cross(up, N));
    vec3 B = cross(N, T);
    return normalize(T * H.x + B * H.y + N * H.z);
}

float DistributionGGX(float NdotH, float rough) {
    float a2 = rough * rough * rough * rough;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

void main() {
    vec3 N = normalize(v_Dir);
    if (u_Roughness < 0.001) {                  // mirror: just copy the sky
        FragColor = vec4(textureLod(u_Env, N, 0.0).rgb, 1.0);
        return;
    }

    vec3 V = N;
    vec3 sum = vec3(0.0);
    float weight = 0.0;
    for (uint i = 0u; i < SAMPLES; i++) {
        vec3 H = ImportanceSampleGGX(Hammersley(i, SAMPLES), N, u_Roughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);
        float NdotL = dot(N, L);
        if (NdotL > 0.0) {
            // Sample a blurrier mip when the sample is statistically rare (removes noise)
            float NdotH = max(dot(N, H), 0.0);
            float HdotV = max(dot(H, V), 0.0);
            float pdf = DistributionGGX(NdotH, u_Roughness) * NdotH / (4.0 * HdotV) + 0.0001;
            float saTexel  = 4.0 * PI / (6.0 * u_Resolution * u_Resolution);
            float saSample = 1.0 / (float(SAMPLES) * pdf + 0.0001);
            float mip = 0.5 * log2(saSample / saTexel);

            sum += textureLod(u_Env, L, mip).rgb * NdotL;
            weight += NdotL;
        }
    }
    FragColor = vec4(sum / max(weight, 1e-4), 1.0);
}