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
//     live2d_unmirror = yes             (default) the GUI mirrors some portraits (the council does); the plugin draws them flipped
//     live2d_view = { auto = yes  body = 0.46 }      or { x = 0.44  y = 0.19  height = 0.26 }, fractions of the model canvas
//     live2d_scale = 1.0                magnifies the framed part around its centre: 1.25 shows a quarter less of the model
//     live2d_actions = {
//         mouse_follow = { enabled = yes  strength = 0.6 }
//         click        = { enabled = yes  motion_group = "touch*"  motion_index = -1 }   // or motion_groups = { a b c }
//                         add  sounds = { "sound/a.wav" "sound/b.ogg" }  volume = 1.0   to say a line instead of the motion's Sound
//         drag         = { enabled = yes  strength = 1.0 }
//     }
//
// `live2d = yes` entries are found in the portraits blocks of the mod's files and in blocks named `live2d_portraits`.
#include <cstdint>
#include <string>
#include <vector>

namespace l2d {

struct MouseFollow { bool enabled = false; float strength = 1.0f; };
struct ClickAction {
    bool enabled = false;
    // motion groups to pick from at random: `motion_group = "name"` or `motion_groups = { a b }`; a name ending in * matches every
    // group that starts with the rest ("touch*": touch_1, touch_2, ...). The Idle group is never chosen by a pattern.
    std::vector<std::string> motion_groups;
    int motion_index = -1;      // -1: a random motion of the chosen group
    // voice lines (absolute paths) to pick from at random instead of the motion's own `Sound`; `sound = "file"` or `sounds = { a b }`
    std::vector<std::string> sounds;
    float volume = 1.0f;        // of those lines and of the motion's, relative to the master volume
    std::string expression;     // read, not implemented yet
};
struct DragAction { bool enabled = false; float strength = 1.0f; };

struct PortraitEntry {
    std::string key;            // the portrait key (`human_female_01`), what the engine reports for a portrait object
    bool live2d = false, spine = false;
    bool unmirror = true;       // draw the picture flipped where the GUI mirrors the portrait, so it comes out the right way round
    std::string model;          // absolute path of the model3.json
    bool auto_view = true;
    float auto_body = 0.46f;
    float view_x = 0.44f, view_y = 0.19f, view_h = 0.26f;
    float scale = 1.0f;         // live2d_scale: >1 shows a smaller part of the model, bigger
    MouseFollow mouse_follow;
    ClickAction click;
    DragAction drag;
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
