#include "claudi_ui_layout.h"

claudi_layout_t claudi_layout_compute(int16_t w, int16_t h, claudi_layout_shape_t shape)
{
    claudi_layout_t r;
    r.cx = (int16_t)(w / 2);
    r.cy = (int16_t)(h / 2);
    r.short_side = w < h ? w : h;
    r.safe_radius = (int16_t)(r.short_side / 2);
    (void)shape;  // round and rect share these anchors; shape steers app layout choice
    return r;
}

uint16_t claudi_layout_image_zoom(int16_t short_side, int16_t src_px, float frac)
{
    if (src_px <= 0) return 256;
    return (uint16_t)(((float)short_side * frac / (float)src_px) * 256.0f);
}
