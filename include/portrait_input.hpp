#pragma once
// Where the mouse pointer is and where a portrait is drawn on the screen: the inputs of the interactions a mod can declare.
namespace l2d {

struct ScreenRect {
    float x = 0, y = 0, w = 0, h = 0;  // client pixels of the game window, y down
    bool mirrored = false;             // the GUI draws the portrait flipped left to right
};

// Where the GUI drew `portrait` last frame. False when that is not known (not drawn, or the draw hook is not installed).
bool PortraitScreenRect(const void* portrait, ScreenRect* out);

// The mouse pointer in client pixels of the game window and the size of the client area. False when the game window is not
// the foreground window (the pointer is then somewhere else).
bool MouseInGameWindow(float* x, float* y, float* width, float* height);

} // namespace l2d
