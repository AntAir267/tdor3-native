layout(push_constant) uniform Push {
    layout(row_major) mat4 m;            // clip = m * pos (SurRender's projection matrix)
    uint shader;                         // srShader (draw.frag)
    int tex0, smp0, tex1, smp1;
    uint fog_color;                      // 0x00RRGGBB
    uint flags;                          // 1: stage 1 has a texture; 2: 2D
    float d3d_shift_x, d3d_shift_y;      // (draw.frag) where Direct3D sampled: this far up-left, pixels
} pc;
