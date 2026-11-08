#version 450 core

in vec2 v_UV;
out float FragAO;

uniform sampler2D u_AO;

void main() {
    vec2 texel = 1.0 / vec2(textureSize(u_AO, 0));
    float sum = 0.0;
    for (int x = -2; x <= 2; x++)
        for (int y = -2; y <= 2; y++)
            sum += texture(u_AO, v_UV + vec2(x, y) * texel).r;
    FragAO = sum / 25.0;
}