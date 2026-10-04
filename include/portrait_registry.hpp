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
//     live2d_view_character_large = { ... }    another framing for one kind of portrait (character, character_large, room, empty_room,
//     live2d_view_800x400 = { ... }            character_without_room) or for one size of picture; a size beats a kind beats the default.
//                                              Each takes auto, body, x, y, height and scale (scale defaults to live2d_scale)
//     live2d_actions = {
//         mouse_follow = { enabled = yes  strength = 0.6 }
//         click  = { motion_group = "touch*" ... }     what happens on each event, see EventAction; events: click (anywhere on the picture),
//         click_Head = { ... }                          click_<Area> (a hit area of the model, the Name in its model3.json HitAreas),
//         hover  = { ... }                              the pointer comes onto the picture, appear (the portrait shows up), idle (now and
//         appear = { ... }   idle = { interval = { 20 40 } ... }   then, every `interval` seconds while it is shown)
//     }
// The motion groups and expressions are the model's own, named in its model3.json; the mod only picks which one an event plays.
//
// `live2d = yes` entries are found in the portraits blocks of the mod's files and in blocks named `live2d_portraits`.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace l2d {

struct MouseFollow { bool enabled = false; float strength = 1.0f; };
// the lines a motion group says: `voices = { touch_1 = "sound/a.wav"  touch_2 = { "sound/b.wav" "sound/c.wav" }  wait* = ... }`; the key is a
// group name or a prefix ending in *, several lines are taken in turn
struct VoiceBinding {
    std::string pattern;
    std::vector<std::string> lines;  // absolute paths
};

// What one event does: a motion, an expression and a line to say, any of them.
struct EventAction {
    bool enabled = false;
    // motion groups to pick from at random: `motion_group = "name"` or `motion_groups = { a b }`; a name ending in * matches every
    // group that starts with the rest ("touch*": touch_1, touch_2, ...). The Idle group is never chosen by a pattern.
    std::vector<std::string> motion_groups;
    int motion_index = -1;      // -1: a random motion of the chosen group
    // voice lines per motion group (checked first), see VoiceBinding
    std::vector<VoiceBinding> voices;
    // voice lines (absolute paths) said by any motion that has no entry in `voices`, in turn, instead of the motion's own `Sound`;
    // `sound = "file"` or `sounds = { a b }`
    std::vector<std::string> sounds;
    float volume = 1.0f;        // of those lines and of the motion's, relative to the master volume
    std::string expression;     // an expression of the model: its Name in the Expressions of model3.json
    float expression_hold = 3.0f;  // seconds before it fades back to the neutral face; 0 = until another one is set
    float interval_min = 20.0f, interval_max = 40.0f;  // idle only: seconds between two
};

// The settings as text, to tell whether a reload changed anything.
inline std::string Describe(const EventAction& a) {
    std::string s = a.enabled ? "on:" : "off:";
    for (const std::string& g : a.motion_groups) s += g + ",";
    s += "|" + std::to_string(a.motion_index) + "|";
    for (const VoiceBinding& v : a.voices) {
        s += v.pattern + "=";
        for (const std::string& l : v.lines) s += l + ",";
        s += ";";
    }
    s += "|";
    for (const std::string& l : a.sounds) s += l + ",";
    s += "|" + std::to_string(a.volume) + "|" + a.expression + "|" + std::to_string(a.expression_hold) + "|" +
         std::to_string(a.interval_min) + "|" + std::to_string(a.interval_max);
    return s;
}

// How a portrait is framed: the part of the model canvas it shows. One per kind of portrait or picture size, plus the default.
struct ViewSpec {
    std::string selector;   // empty: the default; else a kind name (character_large) or a size (800x400)
    int kind = -1;          // the kind selector as a number (see Paint), -1 when it is not a kind
    int width = 0, height = 0;  // the size selector
    bool auto_view = true;
    float body = 0.46f;
    float x = 0.44f, y = 0.19f, h = 0.26f;
    float scale = 1.0f;
};

struct PortraitEntry {
    std::string key;            // the portrait key (`human_female_01`), what the engine reports for a portrait object
    bool live2d = false, spine = false;
    bool unmirror = true;       // draw the picture flipped where the GUI mirrors the portrait, so it comes out the right way round
    std::string model;          // absolute path of the model3.json
    ViewSpec view;              // the default framing (with live2d_scale in `scale`)
    std::vector<ViewSpec> views;  // the framings for one kind or size of portrait
    MouseFollow mouse_follow;
    EventAction click, hover, appear, idle;
    std::vector<std::pair<std::string, EventAction>> click_areas;  // by hit area name
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
