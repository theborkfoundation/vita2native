#pragma once

/* Window coordinates have their origin at the top left. */
typedef struct {
    int x, y, width, height;
} HostPresentRect;

/* Keep the whole frame visible, horizontally centred and aligned to the top. */
static inline HostPresentRect host_present_layout(int window_width, int window_height,
                                                  int width, int height)
{
    double scale_x = (double)window_width / width;
    double scale_y = (double)window_height / height;
    double scale = scale_x < scale_y ? scale_x : scale_y;
    HostPresentRect rect;
    rect.width = (int)(width * scale + 0.5);
    rect.height = (int)(height * scale + 0.5);
    rect.x = (window_width - rect.width) / 2;
    rect.y = 0;
    return rect;
}
