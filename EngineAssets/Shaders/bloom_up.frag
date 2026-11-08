#version 450 core

in vec2 v_UV;
out vec4 FragColor;

uniform sampler2D u_Src;
uniform float u_Radius;

void main() {
    vec2 d = u_Radius / vec2(textureSize(u_Src, 0));

    vec3 a = texture(u_Src, v_UV + vec2(-d.x,  d.y)).rgb;
    vec3 b = texture(u_Src, v_UV + vec2( 0.0,  d.y)).rgb;
    vec3 c = texture(u_Src, v_UV + vec2( d.x,  d.y)).rgb;
    vec3 e = texture(u_Src, v_UV + vec2(-d.x,  0.0)).rgb;
    vec3 f = texture(u_Src, v_UV).rgb;
    vec3 g = texture(u_Src, v_UV + vec2( d.x,  0.0)).rgb;
    vec3 h = texture(u_Src, v_UV + vec2(-d.x, -d.y)).rgb;
    vec3 i = texture(u_Src, v_UV + vec2( 0.0, -d.y)).rgb;
    vec3 j = texture(u_Src, v_UV + vec2( d.x, -d.y)).rgb;

    FragColor = vec4((a + c + h + j + 2.0 * (b + e + g + i) + 4.0 * f) / 16.0, 1.0);
}