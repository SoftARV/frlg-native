#ifndef GUARD_HOST_VIDEO_FIT_H
#define GUARD_HOST_VIDEO_FIT_H

// Where the picture goes inside a window that is not its shape.
struct host_video_fit
{
    float x;
    float y;
    float w;
    float h;
};

// The largest the source fits in the window without its shape changing,
// centred, so whichever axis has room to spare is the one that gets the bars.
// A window or source with no area returns a rectangle with none either.
struct host_video_fit host_video_fit_rect(int win_w, int win_h,
                                          int src_w, int src_h);

#endif // GUARD_HOST_VIDEO_FIT_H
