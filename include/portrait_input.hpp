#pragma once
// The inputs of the interactions a mod can declare: where the mouse pointer is and where a portrait is drawn on the screen.
namespace l2d {

struct ScreenRect {
    float x = 0, y = 0, w = 0, h = 0;  // GUI units from the top-left corner of the GUI, y down (the pointer is in the same units)
    bool mirrored = false;             // the GUI draws the portrait flipped left to right
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

} // namespace l2d
