#version 450
#extension GL_GOOGLE_include_directive : require
// SurRender's vertices: homogeneous positions times its projection matrix, clipped as OpenGL
// clips (NDC y up, z -1..1 onto the viewport's depth range).
#include "push.glsl"
layout(location = 0) in vec4 in_pos;
layout(location = 1) in vec4 in_col;
layout(location = 2) in vec4 in_spec;
layout(location = 3) in vec2 in_uv0;
layout(location = 4) in vec2 in_uv1;
layout(location = 0) out vec4 v_col;
layout(location = 1) out vec4 v_spec;
layout(location = 2) out vec2 v_uv0;
layout(location = 3) out vec2 v_uv1;
void main()
{
    vec4 p = pc.m * in_pos;
    gl_Position = vec4(p.x, -p.y, (p.z + p.w) * 0.5, p.w);
    gl_PointSize = 1.0;
    v_col = in_col;
    v_spec = in_spec;
    v_uv0 = in_uv0;
    v_uv1 = in_uv1;
}
