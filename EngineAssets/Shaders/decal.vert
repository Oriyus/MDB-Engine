#version 450 core
// A decal's box (a unit cube placed by its Model matrix), corners from gl_VertexID: no vertex buffer.
// Only the back faces are drawn (front faces culled), so it still works with the camera inside the box.

struct DecalData {
    mat4 Model;        // unit cube -> world
    mat4 InvModel;     // world -> unit cube
    vec4 Color;        // sRGB colour, alpha
    vec4 Params;       // shape (0 circle, 1 square, 2 texture), thickness, fill, softness
    vec4 Extra;        // emission
};
layout(std430, binding = 3) readonly buffer Decals { DecalData u_Decals[]; };

uniform mat4 u_ViewProjection;
uniform int u_Base;                // first decal of this draw (one draw per texture)

flat out int v_Index;

const vec3 kCorners[8] = vec3[8](
    vec3(-0.5, -0.5, -0.5), vec3(0.5, -0.5, -0.5), vec3(0.5, 0.5, -0.5), vec3(-0.5, 0.5, -0.5),
    vec3(-0.5, -0.5,  0.5), vec3(0.5, -0.5,  0.5), vec3(0.5, 0.5,  0.5), vec3(-0.5, 0.5,  0.5));
// counter-clockwise seen from outside
const int kIndices[36] = int[36](0, 2, 1, 0, 3, 2,   4, 5, 6, 4, 6, 7,   0, 4, 7, 0, 7, 3,
                                 1, 2, 6, 1, 6, 5,   0, 1, 5, 0, 5, 4,   3, 7, 6, 3, 6, 2);

void main() {
    v_Index = u_Base + gl_InstanceID;
    gl_Position = u_ViewProjection * u_Decals[v_Index].Model * vec4(kCorners[kIndices[gl_VertexID]], 1.0);
}
