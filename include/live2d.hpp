#pragma once

#include <cstdint>
#include <string>

namespace l2d {

// Log file next to stellaris.exe (stellaris_live2d.log).
void Log(const char* fmt, ...);

struct Settings {
    // M1 proof of concept: paint a test pattern into the render target of every visible portrait, after the engine has
    // rendered it.
    bool test_pattern = false;
    int only_width = 0;   // 0 = every portrait size, otherwise only render targets of exactly this size
    int only_height = 0;

    // Live2D: draw a model into the render target of every visible portrait (of the size above, if one is set).
    bool live2d = false;
    std::string core_dll;  // path of Live2DCubismCore.dll (Live2D's, or a compatible one)
    std::string model;     // path of the model's model3.json
    float view_x = 0.44f;  // the part of the model canvas shown: centre from the left, centre from the top,
    float view_y = 0.19f;  // and height, all as fractions of the canvas
    float view_h = 0.26f;
    int fps = 30;          // how often the model is advanced and redrawn
    bool physics = true;   // secondary motion from the model's physics3.json

    bool operator==(const Settings&) const = default;
};

// Checks the exe against the SDK and hooks the portrait renderer (the hook passes through while nothing is enabled).
bool Install(uintptr_t base);
void Uninstall();
void Apply(const Settings& s);
std::string StatsLine();

} // namespace l2d
