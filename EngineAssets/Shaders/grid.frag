#version 450 core

// Editor ground grid (Viewport > View > Grid), over the final image: the world's y = 0 plane with 1 m, 10 m and
// 100 m lines (each level fades in when its cells are big enough on screen, so there is no moire), the X axis in
// red and the Z axis in blue. Hidden behind the scene by its depth; fades out with distance.

in vec2 v_UV;
out vec4 FragColor;

uniform sampler2D u_Depth;        // the scene's depth (window depth, 0..1)
uniform mat4  u_InvViewProj;      // unjittered
uniform mat4  u_ViewProj;
uniform vec3  u_CamPos;
uniform float u_Near, u_Far;
uniform float u_Opacity;

float LinearDepth(float d) { return u_Near * u_Far / (u_Far - d * (u_Far - u_Near)); }

// 1 on a line of spacing `s` (about `px` pixels wide), 0 between lines
float Lines(vec2 p, float s, float px) {
    vec2 c = p / s;
    vec2 w = max(fwidth(c), vec2(1e-6));
    vec2 g = abs(fract(c - 0.5) - 0.5) / w;          // distance to the nearest line, in pixels
    return 1.0 - min(min(g.x, g.y) / px, 1.0);
}

void main() {
    vec2 ndc = v_UV * 2.0 - 1.0;
    vec4 a = u_InvViewProj * vec4(ndc, -1.0, 1.0);
    vec4 b = u_InvViewProj * vec4(ndc,  1.0, 1.0);
    vec3 nearP = a.xyz / a.w, farP = b.xyz / b.w;
    vec3 dir = farP - nearP;
    float t = -nearP.y / (abs(dir.y) < 1e-6 ? 1e-6 : dir.y);
    bool hit = t > 0.0;                              // false: the plane is behind the camera
    vec3 p = nearP + dir * t;

    // (Every derivative is taken before any discard: derivatives next to discarded pixels are undefined.)
    // Each level shows once its cells are at least a few pixels across
    vec2 xz = p.xz;
    float metresPerPixel = max(length(vec2(dFdx(xz.x), dFdy(xz.x))), length(vec2(dFdx(xz.y), dFdy(xz.y))));
    float alpha = 0.0;
    const float spacing[3] = float[](1.0, 10.0, 100.0);
    const float strength[3] = float[](0.3, 0.45, 0.55);
    for (int i = 0; i < 3; i++) {
        float cellPx = spacing[i] / max(metresPerPixel, 1e-6);
        float show = smoothstep(6.0, 24.0, cellPx);
        alpha = max(alpha, Lines(xz, spacing[i], 1.0) * strength[i] * show);
    }
    vec3 color = vec3(0.62);

    // The axes: X (z = 0) red, Z (x = 0) blue, a little wider
    vec2 axisPx = abs(xz) / max(fwidth(xz), vec2(1e-6));
    float onX = 1.0 - min(axisPx.y / 1.5, 1.0);
    float onZ = 1.0 - min(axisPx.x / 1.5, 1.0);
    if (onX > 0.0 && onX >= onZ) { color = mix(color, vec3(0.92, 0.30, 0.32), onX); alpha = max(alpha, 0.8 * onX); }
    else if (onZ > 0.0) { color = mix(color, vec3(0.32, 0.52, 0.95), onZ); alpha = max(alpha, 0.8 * onZ); }

    if (!hit) discard;
    // Behind something? Tested by height, not depth: the surface seen here is hidden behind only if it is nearer
    // and stands clearly above the plane on the camera's side. So anything lying at y = 0 (a floor, a flat terrain)
    // always shows the grid on top: comparing depths there z-fought, made worse by the scene being drawn with TAA's
    // jitter and the grid without it (the grid flickered).
    float dist = length(p - u_CamPos);
    vec4 clip = u_ViewProj * vec4(p, 1.0);
    float gridLinear = LinearDepth(clip.z / clip.w * 0.5 + 0.5);
    float sceneDepth = texelFetch(u_Depth, ivec2(gl_FragCoord.xy), 0).r;
    if (sceneDepth < 1.0) {
        float sceneLinear = LinearDepth(sceneDepth);
        vec3 q = u_CamPos + (p - u_CamPos) * (sceneLinear / gridLinear);   // the surface's point on the same ray
        float above = q.y * sign(u_CamPos.y);                             // its height on the camera's side
        if (sceneLinear < gridLinear && above > 0.02 + 0.002 * dist) discard;
    }

    // Fade with distance (further when the camera is higher) and at grazing angles
    float reach = clamp(abs(u_CamPos.y) * 60.0, 60.0, u_Far * 0.9);
    alpha *= 1.0 - smoothstep(reach * 0.4, reach, dist);
    alpha *= smoothstep(0.0, 0.08, abs(normalize(dir).y));

    alpha *= u_Opacity;
    if (alpha < 0.002) discard;
    FragColor = vec4(color, alpha);
}
