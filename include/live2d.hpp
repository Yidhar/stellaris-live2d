#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace l2d {

struct Registry;

// Log file next to stellaris.exe (stellaris_live2d.log).
void Log(const char* fmt, ...);

struct Settings {
    // M1 proof of concept: paint a test pattern into the render target of every visible portrait, after the engine has
    // rendered it.
    bool test_pattern = false;
    int only_width = 0;   // 0 = every portrait size, otherwise only render targets of exactly this size
    int only_height = 0;

    // Live2D: draw models into the render targets of the visible portraits (of the size above, if one is set). Which model a
    // portrait gets comes from the enabled mods' portrait registrations (portrait_registry.hpp); when no mod registers any,
    // `models` is used instead and its models are handed out to the portraits in turn, the first time each is seen.
    bool live2d = false;
    std::string core_dll;  // path of Live2DCubismCore.dll (Live2D's, or a compatible one)
    struct ModelEntry {
        std::string path;       // path of the model's model3.json
        bool auto_view = false; // work the view out from the model's geometry instead of using view_x/y/h
        float auto_body = 0.46f; // with auto_view: how much of the figure's height the portrait shows, from the head down
        float view_x = 0.44f;   // the part of the model canvas shown: centre from the left, centre from the top,
        float view_y = 0.19f;   // and height, all as fractions of the canvas
        float view_h = 0.26f;
        bool operator==(const ModelEntry&) const = default;
    };
    std::vector<ModelEntry> models;
    std::vector<std::string> extra_mod_dirs;  // mod root folders read as if they were enabled (development)
    bool audio = true;     // play the voice lines of motions started by interactions
    float volume = 0.8f;   // master volume of those, 0..1
    bool interactions = true;  // the mouse follow (and later click, drag, zoom) that mods declare; off = models just play
    int fps = 30;          // how often a model is advanced and redrawn
    bool physics = true;   // secondary motion from the models' physics3.json

    bool operator==(const Settings&) const = default;
};

// Checks the exe against the SDK and hooks the portrait renderer (the hook passes through while nothing is enabled).
bool Install(uintptr_t base);
// Returns false when something of the plugin could not be taken out of the game (a hung window): the DLL must then stay loaded.
bool Uninstall();
void Apply(const Settings& s, const Registry& registry);
std::string StatsLine();

} // namespace l2d
