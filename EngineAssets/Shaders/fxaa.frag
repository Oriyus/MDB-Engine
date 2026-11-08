#version 450 core

in vec2 v_UV;
out vec4 FragColor;

uniform sampler2D u_Src;

const float kEdgeMin       = 1.0 / 24.0;   // ignore edges in very dark areas
const float kEdgeThreshold = 1.0 / 8.0;    // minimum contrast (relative) to count as an edge
const float kReduceMin     = 1.0 / 128.0;
const float kReduceMul     = 1.0 / 8.0;
const float kSpanMax       = 8.0;          // longest blur search along an edge, in pixels

float Luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main() {
    vec2 texel = 1.0 / vec2(textureSize(u_Src, 0));

    vec3 rgbM  = texture(u_Src, v_UV).rgb;
    vec3 rgbNW = texture(u_Src, v_UV + vec2(-1.0, -1.0) * texel).rgb;
    vec3 rgbNE = texture(u_Src, v_UV + vec2( 1.0, -1.0) * texel).rgb;
    vec3 rgbSW = texture(u_Src, v_UV + vec2(-1.0,  1.0) * texel).rgb;
    vec3 rgbSE = texture(u_Src, v_UV + vec2( 1.0,  1.0) * texel).rgb;

    float lM = Luma(rgbM), lNW = Luma(rgbNW), lNE = Luma(rgbNE), lSW = Luma(rgbSW), lSE = Luma(rgbSE);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));

    // Flat area: nothing to smooth
    if (lMax - lMin < max(kEdgeMin, lMax * kEdgeThreshold)) {
        FragColor = vec4(rgbM, 1.0);
        return;
    }

    // Edge direction from the brightness gradient
    vec2 dir;
    dir.x = -((lNW + lNE) - (lSW + lSE));
    dir.y =  ((lNW + lSW) - (lNE + lSE));

    float reduce = max((lNW + lNE + lSW + lSE) * 0.25 * kReduceMul, kReduceMin);
    float rcp = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcp, vec2(-kSpanMax), vec2(kSpanMax)) * texel;

    // Blur along the edge, with a wider and a narrower tap pattern
    vec3 rgbA = 0.5 * (texture(u_Src, v_UV + dir * (1.0 / 3.0 - 0.5)).rgb +
                       texture(u_Src, v_UV + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (texture(u_Src, v_UV + dir * -0.5).rgb +
                                     texture(u_Src, v_UV + dir *  0.5).rgb);

    // If the wide blur reaches outside the local brightness range, it grabbed the wrong
    // side of the edge, so fall back to the narrow one
    float lB = Luma(rgbB);
    FragColor = vec4((lB < lMin || lB > lMax) ? rgbA : rgbB, 1.0);
}