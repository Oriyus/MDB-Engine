#version 450 core

in vec3 v_Dir;
out vec4 FragColor;

uniform samplerCube u_Env;
const float PI = 3.14159265359;

void main() {
    vec3 N = normalize(v_Dir);
    vec3 up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(up, N));
    up = cross(N, right);

    vec3 sum = vec3(0.0);
    float count = 0.0;
    const float delta = 0.04;
    for (float phi = 0.0; phi < 2.0 * PI; phi += delta) {
        for (float theta = 0.0; theta < 0.5 * PI; theta += delta) {
            vec3 t = vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            vec3 dir = t.x * right + t.y * up + t.z * N;
            // A blurred mip keeps a tiny, very bright sun from causing sparkles
            sum += textureLod(u_Env, dir, 3.0).rgb * cos(theta) * sin(theta);
            count += 1.0;
        }
    }
    FragColor = vec4(PI * sum / count, 1.0);
}