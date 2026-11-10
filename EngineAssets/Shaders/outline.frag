#version 450 core

// Selection outline (editor), over the final image: a soft line around the selection mask
// (select_mask.frag). Bright where the selected object is visible, dimmer where it is hidden behind
// something, plus a faint fill over its hidden parts so it can be found behind walls.

in vec2 v_UV;
out vec4 FragColor;

uniform sampler2D u_Mask;      // RG8: R visible, G hidden
uniform isampler2D u_IDs;      // the scene's entity IDs (same size)
uniform vec3  u_Color;         // display colour (sRGB)
uniform float u_Width;         // pixels

void main() {
    ivec2 size = textureSize(u_Mask, 0);
    ivec2 p = ivec2(v_UV * vec2(size));
    vec2 here = texelFetch(u_Mask, p, 0).rg;

    // Distance to the nearest visible / hidden mask pixel within the outline width
    int r = int(ceil(u_Width)) + 1;
    float dVis = 1e9, dHid = 1e9;
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            ivec2 q = clamp(p + ivec2(x, y), ivec2(0), size - 1);
            vec2 m = texelFetch(u_Mask, q, 0).rg;
            float d = length(vec2(x, y));
            if (m.r > 0.5) dVis = min(dVis, d);
            if (m.g > 0.5) dHid = min(dHid, d);
        }
    }

    float a = 0.0;
    if (here.r < 0.5 && here.g < 0.5) {
        // outside the selection: the outline
        float vis = clamp(u_Width + 0.5 - dVis, 0.0, 1.0);
        float hid = clamp(u_Width + 0.5 - dHid, 0.0, 1.0) * 0.45;
        a = max(vis, hid);
    }
    else if (here.r > 0.5) {
        // inside a visible selected part: a thin line where it meets another selected object (several selected
        // objects that touch would otherwise merge into one outline). One side only, so the line is 1 px.
        int id = texelFetch(u_IDs, p, 0).r;
        ivec2 qr = min(p + ivec2(1, 0), size - 1), qu = min(p + ivec2(0, 1), size - 1);
        bool edge = (texelFetch(u_Mask, qr, 0).r > 0.5 && texelFetch(u_IDs, qr, 0).r != id)
                 || (texelFetch(u_Mask, qu, 0).r > 0.5 && texelFetch(u_IDs, qu, 0).r != id);
        if (edge) a = 0.8;
    }
    else if (here.r < 0.5) {
        a = 0.18;                // a hidden part: a faint fill
    }
    if (a <= 0.0) discard;
    FragColor = vec4(u_Color, a);
}
