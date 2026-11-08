#version 450 core

in vec3 v_Dir;
out vec4 FragColor;

uniform sampler2D u_Equirect;
uniform int u_Procedural;

const float PI = 3.14159265359;

vec3 ProceduralSky(vec3 d) {
    vec3 zenith  = vec3(0.16, 0.34, 0.80);
    vec3 horizon = vec3(0.70, 0.78, 0.90);
    vec3 ground  = vec3(0.10, 0.09, 0.08);

    vec3 c = d.y > 0.0 ? mix(horizon, zenith, sqrt(d.y))
                       : mix(horizon * 0.5, ground, pow(-d.y, 0.4));

    // A bright sun disc with a soft halo, so metals have something to reflect
    vec3 sunDir = normalize(vec3(0.5, 0.6, 0.4));
    float s = max(dot(d, sunDir), 0.0);
    c += vec3(1.0, 0.9, 0.7) * (pow(s, 600.0) * 20.0 + pow(s, 8.0) * 0.3);
    return c;
}

void main() {
    vec3 d = normalize(v_Dir);
    vec3 c;
    if (u_Procedural == 1) {
        c = ProceduralSky(d);
    } else {
        vec2 uv = vec2(atan(d.z, d.x) / (2.0 * PI) + 0.5, acos(clamp(d.y, -1.0, 1.0)) / PI);
        c = textureLod(u_Equirect, uv, 0.0).rgb;
    }
    FragColor = vec4(min(c, vec3(60000.0)), 1.0);   // stay inside half-float range
}