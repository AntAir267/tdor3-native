#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
// The srShader word (work/analysis/SRDD_SPEC.md section 4):
//  0-2 depth compare, 3 depth write, 4 colour write, 5-7 dst blend (fixed function),
//  8-9 fog: 0 off, 1 to the fog colour, 2 to black, 3 as 1
//  10-11 stage 0 combine: 0 texture, 1/3 texture x diffuse, 2 texture + diffuse
//  12 specular add, 13-14 src blend, 15 texturing,
//  23 alpha test (alpha != 0; != 1 when src blend is 1-SRC_ALPHA)
//  25-28 stage 1 colour op, 29-31 stage 1 alpha op
#include "push.glsl"
layout(set = 0, binding = 0) uniform texture2D texs[];
layout(set = 0, binding = 1) uniform sampler smps[32];
layout(location = 0) in vec4 v_col;
layout(location = 1) in vec4 v_spec;
layout(location = 2) in vec2 v_uv0;
layout(location = 3) in vec2 v_uv1;
layout(location = 0) out vec4 o_color;

void main()
{
    /* Direct3D 6 sampled a pixel at its top-left corner, Vulkan at its centre. The game's 2D
       (menus, HUD, text) is laid out for that: glyph cells end half a pixel short, so a letter's
       top row only showed through Direct3D's sampling. 2D draws sample half a game pixel up-left
       (d3d_shift, in target pixels; 0 in 3D scenes). */
    vec2 uv0 = v_uv0 - dFdx(v_uv0) * pc.d3d_shift_x - dFdy(v_uv0) * pc.d3d_shift_y;
    vec2 uv1 = v_uv1 - dFdx(v_uv1) * pc.d3d_shift_x - dFdy(v_uv1) * pc.d3d_shift_y;
    uint sh = pc.shader;
    vec4 c = v_col;
    bool texturing = (sh & (1u << 15)) != 0u;
    if (texturing && pc.tex0 >= 0) {
        /* 2D pieces that show a whole image (the gauges, panels, menu tiles): kept within it at
           its edges, where drawn larger than at 640x480 the filter would reach round to the
           image's far side (a dark line along a piece's edge) */
        if ((pc.flags & 2u) != 0u) {
            vec2 h = 0.5 / vec2(textureSize(sampler2D(texs[pc.tex0], smps[pc.smp0]), 0));
            vec2 fw = fwidth(v_uv0);         /* (one texel spread over a shape, the gauge needles: as was) */
            bvec2 in01 = bvec2(fw.x > 0.0 && v_uv0.x >= 0.0 && v_uv0.x <= 1.0,
                               fw.y > 0.0 && v_uv0.y >= 0.0 && v_uv0.y <= 1.0);
            vec2 cl = clamp(uv0, h, 1.0 - h);
            uv0 = vec2(in01.x ? cl.x : uv0.x, in01.y ? cl.y : uv0.y);
        }
        vec4 t = texture(sampler2D(texs[pc.tex0], smps[pc.smp0]), uv0);
        uint g = (sh >> 10) & 3u;
        if (g == 0u) c = t;
        else if (g == 2u) c = vec4(min(t.rgb + v_col.rgb, vec3(1.0)), t.a);
        else c = t * v_col;
    }
    if (texturing && (pc.flags & 1u) != 0u) {
        vec4 t1 = texture(sampler2D(texs[pc.tex1], smps[pc.smp1]), uv1);
        uint cop = (sh >> 25) & 15u, aop = (sh >> 29) & 7u;
        vec3 rgb = c.rgb;
        if (cop == 1u) rgb = t1.rgb;
        else if (cop == 2u) rgb = t1.rgb * c.rgb;
        else if (cop == 3u) rgb = t1.rgb + c.rgb - t1.rgb * c.rgb;
        else if (cop == 4u) rgb = t1.rgb + c.rgb;
        else if (cop == 5u) rgb = t1.rgb - c.rgb;
        else if (cop == 6u) rgb = c.rgb - t1.rgb;
        else if (cop == 7u) rgb = mix(c.rgb, t1.rgb, t1.a);
        else if (cop == 8u) rgb = mix(c.rgb, t1.rgb, c.a);
        float a = c.a;
        if (aop == 1u) a = t1.a;
        else if (aop == 2u) a = t1.a * c.a;
        else if (aop == 3u) a = t1.a + c.a - t1.a * c.a;
        c = vec4(clamp(rgb, 0.0, 1.0), clamp(a, 0.0, 1.0));
    }
    if ((sh & (1u << 12)) != 0u)
        c.rgb = min(c.rgb + v_spec.rgb, vec3(1.0));
    if ((sh & (1u << 23)) != 0u) {
        float a8 = floor(c.a * 255.0 + 0.5);
        float ref = ((sh >> 13) & 3u) == 3u ? 255.0 : 0.0;
        if (a8 == ref) discard;
    }
    uint fog = (sh >> 8) & 3u;
    if (fog != 0u) {
        vec3 fc = fog == 2u ? vec3(0.0)
                            : vec3((pc.fog_color >> 16) & 255u, (pc.fog_color >> 8) & 255u, pc.fog_color & 255u) / 255.0;
        c.rgb = mix(c.rgb, fc, clamp(v_spec.w, 0.0, 1.0));
    }
    o_color = c;
}
