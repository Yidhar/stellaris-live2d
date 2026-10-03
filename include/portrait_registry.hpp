#pragma once
// The portraits that mods say should be drawn by the plugin, read from the enabled mods' script files.
//
// A mod registers a portrait the usual way (`portraits = { key = { entity = ... } }` in gfx/portraits/portraits/*.txt) and
// adds the plugin's keys to that entry, or puts an entry with the same key and the plugin's keys in a file of its own under
// gfx/portraits/live2d/ (the engine never reads that folder, so it logs nothing about the extra keys). Keys:
//
//     live2d = yes                      draw the portrait as a Live2D model
//     spine = yes                       ... or as a Spine skeleton (not implemented yet; the portrait stays as the game draws it)
//     live2d_model = "gfx/live2d/x/x.model3.json"   path inside the mod
//     live2d_view = { auto = yes  body = 0.46 }      or { x = 0.44  y = 0.19  height = 0.26 }, fractions of the model canvas
//     live2d_actions = {
//         mouse_follow = { enabled = yes  strength = 0.6 }
//         click        = { enabled = yes  motion_group = "TapBody"  motion_index = -1  expression = "" }
//         drag         = { enabled = yes  strength = 1.0 }
//         scale        = { enabled = yes  min = 0.8  max = 1.6 }
//     }
//
// `live2d = yes` entries are found in the portraits blocks of the mod's files and in blocks named `live2d_portraits`.
#include <cstdint>
#include <string>
#include <vector>

namespace l2d {

struct MouseFollow { bool enabled = false; float strength = 1.0f; };
struct ClickAction { bool enabled = false; std::string motion_group; int motion_index = -1; std::string expression; };
struct DragAction { bool enabled = false; float strength = 1.0f; };
struct ScaleAction { bool enabled = false; float min = 0.5f, max = 2.0f; };

struct PortraitEntry {
    std::string key;            // the portrait key (`human_female_01`), what the engine reports for a portrait object
    bool live2d = false, spine = false;
    std::string model;          // absolute path of the model3.json
    bool auto_view = true;
    float auto_body = 0.46f;
    float view_x = 0.44f, view_y = 0.19f, view_h = 0.26f;
    MouseFollow mouse_follow;
    ClickAction click;
    DragAction drag;
    ScaleAction scale;
    std::string source;         // file and line, for the log
};

struct Registry {
    std::vector<PortraitEntry> entries;  // one per key, the last enabled mod wins
    std::vector<std::string> messages;   // what the scan found and skipped, for the log
    uint64_t signature = 0;              // changes when a scanned file or the playset changes
};

// Scans the enabled mods (Documents\Paradox Interactive\Stellaris\dlc_load.json) and `extra_mod_dirs` (mod root folders to
// read as if enabled, for development).
// With parse = false only the signature is worked out (file sizes and times), cheap enough to do every few seconds.
Registry ScanRegistry(const std::vector<std::string>& extra_mod_dirs, bool parse = true);

} // namespace l2d
