#pragma once
// The inputs of the interactions a mod can declare: where the mouse pointer is, where a portrait is drawn on the screen, and the
// mouse events that land on a portrait's picture.
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

// Called by the portrait hook before the portrait is painted, on the render thread (which is the game window's thread: the first
// call also puts a subclass on the window, which sees the mouse messages as the window gets them).
void SetPortraitFrame(const PortraitFrame& frame);

// Takes the window subclass off again. From the worker thread at unload: the window's own thread is asked to do it. False when that
// did not happen (the game's window thread is hung): the DLL must then stay loaded.
bool ReleaseInput();

// Where a model should look so that it looks at the mouse pointer, as seen from the portrait: each axis -1..1 (x to the right
// on the screen, y up; x is already flipped for a mirrored portrait). Without a known rectangle the portrait is taken to be in
// the middle of the game window. False (and 0, 0) while the game window is not the foreground window.
bool ComputeLookTarget(const void* portrait, float* x, float* y);

// Whether the portrait is drawn mirrored by the GUI (as of its last draw).
bool PortraitMirrored(const void* portrait);

// A mouse event that landed on a portrait's picture: the left button went down on it, or the pointer came onto it. Where portraits
// overlap, the one whose centre is nearest gets it. Only the part the GUI shows counts (inside its clip area). Nothing is swallowed:
// the GUI still gets every message, so a button drawn over the portrait is pressed as well.
struct PortraitEvent {
    enum class Type { Click, Hover, Greeting } type = Type::Click;
    float u = 0, v = 0;  // where in the portrait's rectangle as drawn: 0..1 from its left and from its top
};

// Adds an event for a portrait (the game's greeting sound is one: not a mouse event, but the same way to the painter).
void QueuePortraitEvent(const void* portrait, PortraitEvent::Type type);

// Whether the game window is the foreground window.
bool GameWindowInFront();

// The size of the game window's client area in pixels (false while it is minimized), and the pointer in it (false unless the game is in front
// and the pointer is inside): for the plugin's self-test.
bool GameClientSize(int* width, int* height);
bool ClientPointer(float* x, float* y, float* width, float* height);

// The oldest event waiting for this portrait (events older than half a second are dropped); false when there is none.
bool PopPortraitEvent(const void* portrait, PortraitEvent* out);

} // namespace l2d
