#pragma once
// The inputs of the interactions a mod can declare: where the mouse pointer is and where a portrait is drawn on the screen.
namespace l2d {

struct ScreenRect {
    float x = 0, y = 0, w = 0, h = 0;  // GUI units from the top-left corner of the GUI, y down (the pointer is in the same units)
    bool mirrored = false;             // the GUI draws the portrait flipped left to right
    bool has_clip = false;             // the part of the screen the GUI lets the portrait show (a scroll area, a window), in
    int clip_x0 = 0, clip_y0 = 0, clip_x1 = 0, clip_y1 = 0;  // framebuffer pixels, top-left origin
};

// What the portrait hook read about one portrait object as the engine called UpdatePortrait for it.
struct PortraitFrame {
    const void* portrait = nullptr;
    bool has_rect = false;   // the GUI drew the portrait last frame and its rectangle looks sane
    ScreenRect rect;
    bool has_mouse = false;  // the pointer in GUI units, from the engine's graphics object
    float mouse_x = 0, mouse_y = 0;
    float gui_w = 0, gui_h = 0;  // the size of the GUI in GUI units (1920x1080 at UI scale 1 in a 1080p window)
};

// Called by the portrait hook before the portrait is painted, on the render thread.
void SetPortraitFrame(const PortraitFrame& frame);

// Where a model should look so that it looks at the mouse pointer, as seen from the portrait: each axis -1..1 (x to the right
// on the screen, y up; x is already flipped for a mirrored portrait). Without a known rectangle the portrait is taken to be in
// the middle of the game window. False (and 0, 0) while the game window is not the foreground window.
bool ComputeLookTarget(const void* portrait, float* x, float* y);

// Whether the portrait is drawn mirrored by the GUI (as of its last draw).
bool PortraitMirrored(const void* portrait);

// True once for each left mouse button press that landed on this portrait (inside the rectangle the GUI last drew it in and inside
// its clip area), for a short while after the press. Only while the game window is in front. The press is not swallowed: the GUI
// still sees it, so a button drawn over the portrait is pressed as well.
bool ConsumeClick(const void* portrait);

} // namespace l2d
