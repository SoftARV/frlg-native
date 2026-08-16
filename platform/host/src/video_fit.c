#include "host_video_fit.h"

struct host_video_fit host_video_fit_rect(int win_w, int win_h,
                                          int src_w, int src_h)
{
    struct host_video_fit out = {0.0f, 0.0f, 0.0f, 0.0f};
    float fit_w, fit_h, fit;

    if (win_w <= 0 || win_h <= 0 || src_w <= 0 || src_h <= 0)
        return out;

    fit_w = (float)win_w / (float)src_w;
    fit_h = (float)win_h / (float)src_h;
    fit = fit_w < fit_h ? fit_w : fit_h;

    out.w = (float)src_w * fit;
    out.h = (float)src_h * fit;
    out.x = ((float)win_w - out.w) / 2.0f;
    out.y = ((float)win_h - out.h) / 2.0f;
    return out;
}
