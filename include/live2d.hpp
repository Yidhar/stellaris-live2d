#pragma once

#include <cstdint>
#include <string>

namespace l2d {

// Log file next to stellaris.exe (stellaris_live2d.log).
void Log(const char* fmt, ...);

// M1 proof of concept: paint a test pattern into the render target of every visible portrait, after the engine has
// rendered it. If the pattern shows up inside the portrait frames, with the game's own masks and shader still applied,
// the hook point and the texture write work, and a Live2D frame can take the pattern's place.
struct Settings {
    bool test_pattern = false;
    int only_width = 0;   // 0 = every portrait size, otherwise only render targets of exactly this size
    int only_height = 0;
    bool operator==(const Settings&) const = default;
};

// Checks the exe against the SDK and hooks the portrait renderer (the hook passes through while nothing is enabled).
bool Install(uintptr_t base);
void Uninstall();
void Apply(const Settings& s);
std::string StatsLine();

} // namespace l2d
