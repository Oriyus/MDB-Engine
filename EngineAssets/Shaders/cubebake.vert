#version 450 core

uniform int u_Face;      // 0..5 = +X -X +Y -Y +Z -Z
out vec3 v_Dir;

void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);   // (0,0) (2,0) (0,2)
    vec2 st = p * 2.0 - 1.0;
    float sc = st.x, tc = st.y;

    vec3 d;
    switch (u_Face) {
        case 0:  d = vec3( 1.0, -tc, -sc); break;
        case 1:  d = vec3(-1.0, -tc,  sc); break;
        case 2:  d = vec3( sc,  1.0,  tc); break;
        case 3:  d = vec3( sc, -1.0, -tc); break;
        case 4:  d = vec3( sc, -tc,  1.0); break;
        default: d = vec3(-sc, -tc, -1.0); break;
    }
    v_Dir = d;
    gl_Position = vec4(st, 0.0, 1.0);
}