#include "portrait_registry.hpp"

#include "pdx_script.hpp"

#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace l2d {

namespace {

std::string DocumentsDir() {
    PWSTR w = nullptr;
    fs::path p;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &w))) p = w;
    CoTaskMemFree(w);
    return (p / "Paradox Interactive" / "Stellaris").string();
}

uint64_t Mix(uint64_t h, const std::string& s) {
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

// signature of a file: its path, size and modification time
uint64_t Sign(uint64_t h, const fs::path& p) {
    std::error_code ec;
    h = Mix(h, p.string());
    const auto size = fs::file_size(p, ec);
    h = Mix(h, std::to_string(ec ? 0 : size));
    const auto t = fs::last_write_time(p, ec);
    h = Mix(h, std::to_string(ec ? 0 : (long long)t.time_since_epoch().count()));
    return h;
}

bool ReadText(const fs::path& p, std::string* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out->assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

// One event's settings: `click = { motion_group = "touch*" voices = { ... } sounds = { ... } volume = 1 expression = "smile" }`; the
// block form is enabled unless it says `enabled = no`, the bare form is `click = yes`.
void ParseAction(const pdx::Node& n, const fs::path& root, EventAction* a) {
    a->enabled = n.block ? n.Bool("enabled", true) : (n.value == "yes" || n.value == "true");
    if (!n.block) return;
    auto absolute = [&](const std::string& s) { return (root / fs::u8path(s)).lexically_normal().string(); };
    a->motion_groups = n.List("motion_groups");
    if (!n.Str("motion_group").empty()) a->motion_groups.push_back(n.Str("motion_group"));
    a->motion_index = (int)n.Num("motion_index", -1);
    if (const pdx::Node* v = n.Find("voices"); v && v->block) {
        for (const pdx::Node& b : v->children) {
            if (b.key.empty()) continue;
            VoiceBinding vb;
            vb.pattern = b.key;
            if (b.block) {
                for (const pdx::Node& l : b.children)
                    if (l.key.empty() && !l.block) vb.lines.push_back(absolute(l.value));
            } else {
                vb.lines.push_back(absolute(b.value));
            }
            if (!vb.lines.empty()) a->voices.push_back(std::move(vb));
        }
    }
    std::vector<std::string> sounds = n.List("sounds");
    if (!n.Str("sound").empty()) sounds.push_back(n.Str("sound"));
    for (const std::string& s : sounds) a->sounds.push_back(absolute(s));
    a->volume = (float)n.Num("volume", 1.0);
    a->expression = n.Str("expression");
    a->expression_hold = (float)n.Num("expression_hold", 3.0);
    a->replace_engine_sound = n.Bool("replace_engine_sound", false);
    if (const pdx::Node* iv = n.Find("interval"); iv && iv->block) {
        std::vector<double> v;
        for (const pdx::Node& c : iv->children)
            if (c.key.empty() && !c.block) v.push_back(std::strtod(c.value.c_str(), nullptr));
        if (!v.empty()) {
            a->interval_min = (float)v.front();
            a->interval_max = (float)(v.size() > 1 ? v[1] : v.front());
        }
    }
    if (a->interval_min < 1.0f) a->interval_min = 1.0f;
    if (a->interval_max < a->interval_min) a->interval_max = a->interval_min;
}

void ReadEntry(const pdx::Node& e, const fs::path& root, const std::string& file, Registry* reg) {
    PortraitEntry p;
    p.key = e.key;
    p.live2d = e.Bool("live2d");
    p.spine = e.Bool("spine");
    p.unmirror = e.Bool("live2d_unmirror", true);
    p.view.scale = std::clamp((float)e.Num("live2d_scale", 1.0), 0.1f, 10.0f);
    p.source = file + ":" + std::to_string(e.line);
    if (!p.live2d && !p.spine) return;
    const std::string model = e.Str(p.live2d ? "live2d_model" : "spine_model");
    if (model.empty()) {
        reg->messages.push_back(p.source + ": portrait " + p.key + " is " + (p.live2d ? "live2d" : "spine") + " but has no model path; skipped");
        return;
    }
    p.model = (root / fs::u8path(model)).lexically_normal().string();
    auto read_view = [&](const pdx::Node& v, ViewSpec* spec) {
        spec->auto_view = v.Bool("auto", !v.Find("x"));
        spec->body = (float)v.Num("body", spec->body);
        spec->x = (float)v.Num("x", spec->x);
        spec->y = (float)v.Num("y", spec->y);
        spec->h = (float)v.Num("height", spec->h);
        spec->scale = std::clamp((float)v.Num("scale", spec->scale), 0.1f, 10.0f);
    };
    if (const pdx::Node* v = e.Find("live2d_view"); v && v->block) read_view(*v, &p.view);
    for (const pdx::Node& c : e.children) {
        if (!c.block || c.key.rfind("live2d_view_", 0) != 0) continue;
        ViewSpec spec = p.view;  // what a variant does not say is the default's
        spec.selector = c.key.substr(12);
        static const char* const kinds[] = { "character", "character_large", "room", "empty_room", "character_without_room" };
        for (int k = 0; k < 5; ++k)
            if (spec.selector == kinds[k]) spec.kind = k;
        int w = 0, h = 0;
        if (spec.kind < 0 && sscanf(spec.selector.c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            spec.width = w;
            spec.height = h;
        } else if (spec.kind < 0) {
            reg->messages.push_back(p.source + ": portrait " + p.key + ": `" + c.key + "` names no portrait kind or size (character, character_large, room, empty_room, character_without_room or WxH); ignored");
            continue;
        }
        read_view(c, &spec);
        p.views.push_back(std::move(spec));
    }
    if (const pdx::Node* a = e.Find("live2d_actions"); a && a->block) {
        for (const pdx::Node& c : a->children) {
            if (c.key == "mouse_follow") {
                p.mouse_follow.enabled = c.block ? c.Bool("enabled", true) : c.value == "yes";
                p.mouse_follow.strength = (float)c.Num("strength", 1.0);
            } else if (c.key == "click") {
                ParseAction(c, root, &p.click);
            } else if (c.key.rfind("click_", 0) == 0 && c.key.size() > 6) {
                EventAction area;
                ParseAction(c, root, &area);
                p.click_areas.emplace_back(c.key.substr(6), std::move(area));
            } else if (c.key == "hover") {
                ParseAction(c, root, &p.hover);
            } else if (c.key == "appear") {
                ParseAction(c, root, &p.appear);
            } else if (c.key == "idle") {
                ParseAction(c, root, &p.idle);
            } else if (c.key == "greeting") {
                ParseAction(c, root, &p.greeting);
            } else if (!c.key.empty()) {
                reg->messages.push_back(p.source + ": portrait " + p.key + ": live2d_actions has no action `" + c.key + "`; ignored");
            }
        }
    }
    // the last definition of a key wins
    for (PortraitEntry& old : reg->entries) {
        if (old.key == p.key) {
            reg->messages.push_back(p.source + ": portrait " + p.key + " replaces the definition at " + old.source);
            old = std::move(p);
            return;
        }
    }
    reg->entries.push_back(std::move(p));
}

void ScanFile(const fs::path& file, const fs::path& root, bool sidecar, Registry* reg) {
    std::string text;
    if (!ReadText(file, &text)) return;
    // the engine's own portrait files have no plugin keys; do not parse them unless they mention them
    if (!sidecar && text.find("live2d") == std::string::npos && text.find("spine") == std::string::npos) return;
    pdx::Node doc;
    std::string err;
    if (!pdx::Parse(text, &doc, &err)) {
        reg->messages.push_back(file.string() + ": " + err);
        if (doc.children.empty()) return;
    }
    for (const pdx::Node& top : doc.children) {
        if (!top.block || (top.key != "portraits" && top.key != "live2d_portraits")) continue;
        for (const pdx::Node& e : top.children)
            if (e.block && !e.key.empty()) ReadEntry(e, root, file.string(), reg);
    }
}

void ScanMod(const fs::path& root, const std::string& name, bool parse, Registry* reg) {
    std::error_code ec;
    int files = 0;
    const size_t before = reg->entries.size();
    for (const char* dir : { "gfx/portraits/portraits", "gfx/portraits/live2d" }) {
        const bool sidecar = std::string(dir) == "gfx/portraits/live2d";
        const fs::path d = root / dir;
        if (!fs::is_directory(d, ec)) continue;
        std::vector<fs::path> list;
        for (const auto& e : fs::directory_iterator(d, ec))
            if (e.is_regular_file() && e.path().extension() == ".txt") list.push_back(e.path());
        std::sort(list.begin(), list.end());  // the engine loads files in name order
        for (const fs::path& f : list) {
            reg->signature = Sign(reg->signature, f);
            if (parse) ScanFile(f, root, sidecar, reg);
            ++files;
        }
    }
    if (parse && (reg->entries.size() != before || files))
        reg->messages.push_back("mod " + name + " (" + root.string() + "): " + std::to_string(reg->entries.size() - before) + " new portrait key(s) in " + std::to_string(files) + " file(s)");
}

} // namespace

Registry ScanRegistry(const std::vector<std::string>& extra_mod_dirs, bool parse, bool use_playset) {
    Registry reg;
    reg.signature = 14695981039346656037ull;
    const fs::path docs = DocumentsDir();
    const fs::path playset = docs / "dlc_load.json";
    std::error_code ec;
    if (use_playset && fs::exists(playset, ec)) {
        reg.signature = Sign(reg.signature, playset);
        std::string text;
        if (ReadText(playset, &text)) {
            try {
                const auto j = nlohmann::json::parse(text);
                if (j.contains("enabled_mods")) {
                    for (const auto& m : j["enabled_mods"]) {
                        const fs::path descriptor = docs / fs::u8path(m.get<std::string>());
                        reg.signature = Sign(reg.signature, descriptor);
                        std::string dtext;
                        if (!ReadText(descriptor, &dtext)) continue;
                        pdx::Node d;
                        std::string err;
                        pdx::Parse(dtext, &d, &err);
                        const std::string path = d.Str("path");
                        if (path.empty()) {
                            if (!d.Str("archive").empty()) reg.messages.push_back(m.get<std::string>() + ": archive mods are not scanned");
                            continue;
                        }
                        ScanMod(fs::u8path(path), d.Str("name", m.get<std::string>()), parse, &reg);
                    }
                }
            } catch (const std::exception& e) {
                reg.messages.push_back(std::string("dlc_load.json: ") + e.what());
            }
        }
    }
    for (const std::string& dir : extra_mod_dirs) {
        if (dir.empty()) continue;
        reg.signature = Mix(reg.signature, dir);
        ScanMod(fs::u8path(dir), "(extra) " + dir, parse, &reg);
    }
    return reg;
}

} // namespace l2d
