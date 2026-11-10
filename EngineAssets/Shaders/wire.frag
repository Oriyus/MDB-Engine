#version 450 core

// Editor wireframe view (SceneRenderer::ViewMode::Wireframe), linked with lit.vert.
// Pass 0 fills the triangles to write depth and entity IDs (colour masked off), so clicks pick the
// surface under the mouse. Pass 1 draws the triangles as lines (glPolygonMode GL_LINE) in u_WireColor.

in vec2 v_TexCoord;
in vec4 v_Color;
flat in int v_EntityID;

layout(location = 0) out vec4 FragColor;
layout(location = 1) out int  EntityID;

uniform sampler2D u_Texture;     // unit 0
uniform int   u_UseTexture;
uniform int   u_AlphaMode;       // 1 = cutout
uniform float u_AlphaCutoff;
uniform int   u_WirePass;        // 0 fill (depth + IDs), 1 lines
uniform vec3  u_WireColor;       // linear

void main() {
    EntityID = v_EntityID;
    if (u_WirePass == 0 && u_AlphaMode == 1) {      // leaf cards etc.: click through the cut-away parts
        float a = v_Color.a * (u_UseTexture == 1 ? texture(u_Texture, v_TexCoord).a : 1.0);
        if (a < u_AlphaCutoff) discard;
    }
    FragColor = vec4(u_WireColor, 1.0);
}
