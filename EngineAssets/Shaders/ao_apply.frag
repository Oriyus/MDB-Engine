#version 450 core
// Ambient occlusion onto the opaque scene (SceneRenderer, right after the opaque passes): multiplied into the HDR
// colour with blending (DST_COLOR, ZERO). Done before the sky, glass, volumetric fog and particles, so those are not
// darkened, and before the TAA resolve, which averages away AO's noise.

in vec2 v_UV;
layout(location = 0) out vec4 FragColor;

uniform sampler2D u_AO;          // 1 = open, 0 = fully occluded (half resolution, blurred)
uniform float u_AOStrength;

void main() {
    const float ao = mix(1.0, texture(u_AO, v_UV).r, u_AOStrength);
    FragColor = vec4(vec3(ao), 1.0);
}
