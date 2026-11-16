#version 450 core

// A shader graph node's thumbnail in the Shader Graph Editor (Renderer/ShaderGraph.h, SGNodePreviews): the node's
// first output, drawn with tonemap.vert's fullscreen triangle into a small texture. Flat (UV across the square) unless
// the node depends on where or which way the surface is (u_Sphere: a unit sphere seen from the front, lit softly so
// its shape reads). Values are shown as colour: a float as grey, a vec2 as red / green; gamma-encoded for display.

in vec2 v_UV;
out vec4 FragColor;

uniform float u_SceneWindTime;   // the Time node
uniform int u_Sphere;

// @GRAPH_CODE

void main() {
    vec2 uv = v_UV;
    SGIn gIn;
    gIn.Time = u_SceneWindTime;
    gIn.ScreenUV = uv;
    gIn.ViewDir = vec3(0.0, 0.0, 1.0);
    if (u_Sphere == 1) {
        vec2 p = uv * 2.0 - 1.0;
        float r2 = dot(p, p);
        if (r2 > 1.0) {                                   // behind the sphere: a dim checker
            float c = mod(floor(uv.x * 8.0) + floor(uv.y * 8.0), 2.0);
            FragColor = vec4(vec3(0.10 + 0.04 * c), 1.0);
            return;
        }
        vec3 n = vec3(p, sqrt(1.0 - r2));
        gIn.WorldNormal = n;
        gIn.WorldPos = n * 0.5;
        gIn.UV = vec2(atan(n.x, n.z) / 6.28318 + 0.5, n.y * 0.5 + 0.5);
    }
    else {
        gIn.UV = uv;
        gIn.WorldPos = vec3(uv.x * 2.0 - 1.0, uv.y * 2.0 - 1.0, 0.0);
        gIn.WorldNormal = vec3(0.0, 0.0, 1.0);
    }
    vec3 v = GraphPreview(gIn).rgb;
    if (any(isnan(v))) v = vec3(1.0, 0.0, 1.0);
    FragColor = vec4(pow(clamp(v, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0);
}
