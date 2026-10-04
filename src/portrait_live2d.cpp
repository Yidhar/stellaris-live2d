#include "portrait_live2d.hpp"

#include "live2d_character.hpp"
#include "live2d_renderer.hpp"
#include "portrait_input.hpp"
#include "voice.hpp"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <random>
#include <mutex>
#include <set>
#include <unordered_map>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace l2d {

namespace {

uint64_t Ticks() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}

double TickFrequency() {
    static const double f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return (double)q.QuadPart; }();
    return f;
}

// A point of a portrait's rectangle (u from the left, v from the top, 0..1) as a point of the model, in the model's own units with y up,
// for the view the portrait is drawn with: the inverse of what the renderer does.
void RectPointToModel(const View& view, const Model& m, UINT width, UINT height, float u, float v, float* x, float* y) {
    const float cw = m.canvas_size.x, ch = m.canvas_size.y;
    const float vh = view.height, vw = vh * (ch / cw) * ((float)width / (float)height);
    const float fx = view.center_x + (u - 0.5f) * vw, fy = view.center_y + (v - 0.5f) * vh;  // canvas fractions from the left and the top
    *x = (fx * cw - m.canvas_origin.x) / m.pixels_per_unit;
    *y = ((1.0f - fy) * ch - m.canvas_origin.y) / m.pixels_per_unit;
}

bool SameName(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower((unsigned char)x) == std::tolower((unsigned char)y); });
}

struct Frame {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    uint64_t serial = 0;  // the step this frame was drawn for
};

// One loaded model, once, however many portrait keys use it: the Core model with its textures, motions and physics, and its
// animation state. Shared between the worker and the render thread; after it is published only the render thread touches the
// character.
struct Slot {
    std::string path, name;
    std::shared_ptr<Character> character;
    Model::PortraitBounds bounds;  // measured at load, while nobody else used the model
};

// How a portrait key shows a model: the framing, and what it does when the mouse is near or it is clicked. Cheap: the model, its
// textures and its animation are the slot's; this holds only settings, and the frames drawn for it.
struct ActionState {
    std::vector<uint32_t> voice_next;  // render thread: per `voices` entry of the action, which of its lines comes next
    uint32_t sound_next = 0;           // the same for its `sounds`
};
enum { kStateClick = 0, kStateHover = 1, kStateAppear = 2, kStateIdle = 3, kStateAreas = 4 };

struct Presentation {
    int slot = -1;  // index into the slots
    View view;
    MouseFollow follow;
    EventAction click, hover, appear, idle;
    std::vector<std::pair<std::string, EventAction>> click_areas;
    std::vector<ActionState> states;    // kStateClick.., then one per click area
    bool unmirror = true;
    std::string identity;               // the settings as text, to see what a reload changed
    uint64_t next_idle = 0;             // render thread: when the idle action fires next (Ticks; 0 = not scheduled yet)
};

} // namespace

struct Live2DPainter::Impl {
    // --- shared between the worker and the render thread, under `mutex`
    std::mutex mutex;
    std::shared_ptr<core::Api> api;
    std::vector<std::shared_ptr<Slot>> slots;
    std::vector<std::shared_ptr<Presentation>> presentations;
    uint64_t generation = 0;  // changes whenever the slots or presentations do
    int fps = 30;
    bool physics = true;
    bool interactions = true;
    bool audio = true;
    std::string loaded_core;
    std::string loaded_signature;                // what the slots were made from, to see when that changes
    std::unordered_map<std::string, int> by_key; // portrait key -> presentation, when the mods register portraits
    bool registry_mode = false;
    bool voice_failed = false;  // worker thread: opening the playback device failed, do not retry every two seconds

    // --- render thread only
    struct GpuSlot {
        std::shared_ptr<Slot> slot;
        Renderer::GpuPtr gpu;
        std::map<std::pair<int, uint64_t>, Frame> frames;  // by presentation, then output size, format and flip
        uint64_t stepped = 0;                              // the last step this model was advanced for
    };
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> deferred;
    std::unique_ptr<Renderer> renderer;
    std::vector<GpuSlot> gpu_slots;
    std::vector<std::shared_ptr<Presentation>> gpu_presentations;
    uint64_t gpu_generation = 0;
    std::unordered_map<const void*, int> assignment;  // fallback mode: portrait object -> presentation
    std::unordered_map<std::string, int> gpu_by_key;  // registry mode: copy of by_key for this generation
    bool gpu_registry_mode = false;
    std::set<std::string> seen;                       // portrait keys already logged
    int next_presentation = 0;
    uint64_t last_call = 0, serial = 0;
    double pending = 0.0;  // seconds since the last step that have not been stepped yet
    bool reported_failure = false;
    struct Painted {
        uint64_t tick = 0;
        std::string key;
    };
    std::unordered_map<const void*, Painted> last_painted;  // when each portrait object was last painted, for the appear event
    std::mt19937 rng{ std::random_device{}() };

    // --- statistics
    std::atomic<uint64_t> ticks{ 0 }, draws{ 0 }, copies{ 0 }, failures{ 0 }, assigned{ 0 }, slot_count{ 0 }, skipped{ 0 };
    std::atomic<uint64_t> tick_ticks{ 0 }, draw_ticks{ 0 }, copy_ticks{ 0 };

    void ResetGpu() {
        gpu_slots.clear();
        gpu_presentations.clear();
        assignment.clear();
        gpu_by_key.clear();
        seen.clear();
        next_presentation = 0;
        renderer.reset();
        deferred.Reset();
        device.Reset();
    }
};

Live2DPainter::Live2DPainter() : impl_(new Impl) {}
Live2DPainter::~Live2DPainter() = default;

Live2DPainter& Painter() {
    static Live2DPainter painter;
    return painter;
}

void Live2DPainter::Configure(const Settings& s, const Registry& registry) {
    Impl& d = *impl_;

    // the voice device: open while Live2D and audio are both on (opening takes a moment, so it is done here, not on the render thread)
    if (s.live2d && s.audio) {
        if (!d.voice_failed && !Voice::Get().Start(s.volume)) d.voice_failed = true;
        else if (!d.voice_failed) Voice::Get().SetVolume(s.volume);
    } else {
        d.voice_failed = false;
        Voice::Get().Shutdown();
    }

    // What to load: every model once (the mods' portrait keys can share one), and for every portrait key how it presents its model.
    struct PresWant {
        int model = -1;  // index into `paths`
        bool auto_view = false;
        float body = 0.46f, x = 0, y = 0, h = 0, scale = 1.0f;
        bool unmirror = true;
        MouseFollow follow;
        EventAction click, hover, appear, idle;
        std::vector<std::pair<std::string, EventAction>> click_areas;
        std::string identity;
    };
    std::vector<std::string> paths;
    std::vector<PresWant> wants;
    std::unordered_map<std::string, int> by_key;
    auto model_index = [&](const std::string& path) {
        for (size_t i = 0; i < paths.size(); ++i)
            if (paths[i] == path) return (int)i;
        paths.push_back(path);
        return (int)paths.size() - 1;
    };
    for (const PortraitEntry& e : registry.entries) {
        if (!e.live2d) continue;  // spine entries are read but not drawn yet
        PresWant w;
        w.model = model_index(e.model);
        w.auto_view = e.auto_view;
        w.body = e.auto_body;
        w.x = e.view_x;
        w.y = e.view_y;
        w.h = e.view_h;
        w.scale = e.scale;
        w.unmirror = e.unmirror;
        w.follow = e.mouse_follow;
        w.click = e.click;
        w.hover = e.hover;
        w.appear = e.appear;
        w.idle = e.idle;
        w.click_areas = e.click_areas;
        char id[256];
        snprintf(id, sizeof id, "%d|%d %.3f %.3f %.3f %.3f z%.3f|m%d f%.2f u%d|", w.model, (int)w.auto_view, w.body, w.x, w.y, w.h, w.scale,
                 (int)e.mouse_follow.enabled, e.mouse_follow.strength, (int)e.unmirror);
        w.identity = std::string(id) + "click " + Describe(e.click) + "|hover " + Describe(e.hover) + "|appear " + Describe(e.appear) + "|idle " +
                     Describe(e.idle);
        for (const auto& [name, action] : e.click_areas) w.identity += "|click_" + name + " " + Describe(action);
        int index = -1;
        for (size_t i = 0; i < wants.size(); ++i)
            if (wants[i].identity == w.identity) { index = (int)i; break; }
        if (index < 0) { wants.push_back(w); index = (int)wants.size() - 1; }
        by_key[e.key] = index;
    }
    const bool registry_mode = !wants.empty();
    if (!registry_mode) {
        // the ini's list: one presentation per entry, handed out to the portrait objects in turn
        for (const Settings::ModelEntry& e : s.models) {
            PresWant w;
            w.model = model_index(e.path);
            w.auto_view = e.auto_view;
            w.body = e.auto_body;
            w.x = e.view_x;
            w.y = e.view_y;
            w.h = e.view_h;
            w.unmirror = false;
            char id[160];
            snprintf(id, sizeof id, "list %zu|%d|%d %.3f %.3f %.3f %.3f", wants.size(), w.model, (int)w.auto_view, w.body, w.x, w.y, w.h);
            w.identity = id;
            wants.push_back(w);
        }
    }
    std::string signature = s.core_dll + (registry_mode ? "|registry" : "|list");
    for (const std::string& p : paths) signature += "\nmodel " + p;
    for (const PresWant& w : wants) signature += "\npresentation " + w.identity;
    {
        std::vector<std::pair<std::string, int>> keys(by_key.begin(), by_key.end());
        std::sort(keys.begin(), keys.end());
        for (const auto& k : keys) signature += "\nkey " + k.first + "=" + std::to_string(k.second);
    }

    if (!s.live2d || s.core_dll.empty() || wants.empty()) {
        std::lock_guard<std::mutex> lock(d.mutex);
        if (!d.slots.empty()) {
            Log("live2d: models unloaded");
            d.slots.clear();
            d.presentations.clear();
            d.by_key.clear();
            d.loaded_signature.clear();
            ++d.generation;
        }
        return;
    }
    std::unordered_map<std::string, std::shared_ptr<Slot>> loaded;  // models already in memory, to keep when they are still wanted
    std::shared_ptr<core::Api> api;
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        d.fps = s.fps < 1 ? 1 : s.fps > 120 ? 120 : s.fps;
        d.interactions = s.interactions;
        d.audio = s.audio;
        if (d.physics != s.physics) {
            d.physics = s.physics;
            for (auto& slot : d.slots) slot->character->set_physics_enabled(s.physics);
        }
        if (!d.slots.empty() && d.loaded_signature == signature) return;
        if (d.api && d.loaded_core == s.core_dll) {
            api = d.api;
            for (const auto& slot : d.slots) loaded[slot->path] = slot;
        }
    }

    // load outside the lock: the render thread keeps drawing whatever is current
    std::string err;
    if (!api) {
        api = std::make_shared<core::Api>();
        if (!api->Load(std::wstring(s.core_dll.begin(), s.core_dll.end()), &err)) {
            Log("live2d: %s: %s", s.core_dll.c_str(), err.c_str());
            return;
        }
        Log("live2d: Cubism Core loaded from %s (version 0x%08X)", s.core_dll.c_str(), api->GetVersion());
    }
    std::vector<std::shared_ptr<Slot>> slots;
    std::vector<int> slot_of_path(paths.size(), -1);
    for (size_t pi = 0; pi < paths.size(); ++pi) {
        auto kept = loaded.find(paths[pi]);
        if (kept != loaded.end()) {  // a model that stays is neither loaded nor uploaded again, and keeps its animation
            slot_of_path[pi] = (int)slots.size();
            slots.push_back(kept->second);
            continue;
        }
        const uint64_t t0 = Ticks();
        auto character = std::make_shared<Character>();
        if (!character->Load(api.get(), paths[pi], &err)) {
            Log("live2d: cannot load %s: %s", paths[pi].c_str(), err.c_str());
            continue;
        }
        character->set_physics_enabled(s.physics);
        if (!character->physics_error().empty()) Log("live2d: %s: physics not loaded: %s", paths[pi].c_str(), character->physics_error().c_str());
        auto slot = std::make_shared<Slot>();
        slot->path = paths[pi];
        slot->character = character;
        slot->name = std::filesystem::path(paths[pi]).parent_path().filename().string();
        slot->bounds = character->model().MeasurePortrait();
        const Model& m = character->model();
        Log("live2d: loaded %s in %.0f ms: canvas %.0fx%.0f, %d parameters, %d drawables, %zu textures (%.1f MB)", slot->name.c_str(),
            (Ticks() - t0) * 1000.0 / TickFrequency(), m.canvas_size.x, m.canvas_size.y, m.parameter_count, m.drawable_count, m.textures.size(),
            [&] { size_t b = 0; for (const Image& i : m.textures) b += i.Bytes(); return b / 1048576.0; }());
        slot_of_path[pi] = (int)slots.size();
        slots.push_back(std::move(slot));
    }
    // the presentations: a view worked out from the model's measured bounds or given, magnified by the scale
    std::vector<std::shared_ptr<Presentation>> presentations;
    std::vector<int> presentation_of_want(wants.size(), -1);
    for (size_t wi = 0; wi < wants.size(); ++wi) {
        const PresWant& w = wants[wi];
        if (slot_of_path[w.model] < 0) continue;
        auto p = std::make_shared<Presentation>();
        p->slot = slot_of_path[w.model];
        p->view = { w.x, w.y, w.h };
        if (w.auto_view) Model::ViewFromBounds(slots[p->slot]->bounds, w.body, &p->view.center_x, &p->view.center_y, &p->view.height);
        p->view.height /= w.scale;  // live2d_scale: magnify around the middle of the framed part
        p->follow = w.follow;
        p->click = w.click;
        p->hover = w.hover;
        p->appear = w.appear;
        p->idle = w.idle;
        p->click_areas = w.click_areas;
        p->states.resize(kStateAreas + w.click_areas.size());
        p->states[kStateClick].voice_next.assign(w.click.voices.size(), 0);
        p->states[kStateHover].voice_next.assign(w.hover.voices.size(), 0);
        p->states[kStateAppear].voice_next.assign(w.appear.voices.size(), 0);
        p->states[kStateIdle].voice_next.assign(w.idle.voices.size(), 0);
        for (size_t i = 0; i < w.click_areas.size(); ++i) p->states[kStateAreas + i].voice_next.assign(w.click_areas[i].second.voices.size(), 0);
        p->unmirror = w.unmirror;
        p->identity = w.identity;
        presentation_of_want[wi] = (int)presentations.size();
        presentations.push_back(std::move(p));
    }
    std::unordered_map<std::string, int> keys;
    for (const auto& [key, wi] : by_key)
        if (presentation_of_want[wi] >= 0) keys[key] = presentation_of_want[wi];
    std::lock_guard<std::mutex> lock(d.mutex);
    d.slots = std::move(slots);  // models that went away live on until the render thread drops its copies
    d.presentations = std::move(presentations);
    d.by_key = std::move(keys);
    d.registry_mode = registry_mode;
    ++d.generation;
    d.api = api;
    d.loaded_core = s.core_dll;
    d.loaded_signature = signature;
    d.slot_count = d.slots.size();
    if (registry_mode) Log("live2d: %zu portrait key(s) use %zu presentation(s) of %zu model(s)", d.by_key.size(), d.presentations.size(), d.slots.size());
}

bool Live2DPainter::Ready() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return !impl_->slots.empty();
}

PaintResult Live2DPainter::Paint(const void* portrait, const char* key, ID3D11Texture2D* target, const D3D11_TEXTURE2D_DESC& desc) {
    Impl& d = *impl_;
    int fps;
    bool interactions, audio;
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        fps = d.fps;
        interactions = d.interactions;
        audio = d.audio;
        if (d.slots.empty() || d.presentations.empty()) return PaintResult::Skipped;
        if (d.generation != d.gpu_generation) {
            // the set of models or presentations changed: a model that stayed keeps its GPU resources, new ones get theirs when first used
            std::vector<Impl::GpuSlot> fresh;
            for (const auto& slot : d.slots) {
                Impl::GpuSlot g;
                for (auto& old : d.gpu_slots)
                    if (old.slot == slot) { g = std::move(old); break; }
                g.slot = slot;
                g.frames.clear();  // keyed by presentation index, which may have changed
                fresh.push_back(std::move(g));
            }
            d.gpu_slots = std::move(fresh);
            d.gpu_presentations = d.presentations;
            d.assignment.clear();
            d.next_presentation = 0;
            d.gpu_by_key = d.by_key;
            d.gpu_registry_mode = d.registry_mode;
            d.seen.clear();
            d.gpu_generation = d.generation;
        }
    }

    // which presentation this portrait gets: the one registered for its key, or in the fallback mode the next one in turn
    int pres_index = -1;
    if (d.gpu_registry_mode) {
        if (!key[0]) return PaintResult::Skipped;  // planets and other objects that show no portrait have no key
        const std::string k = key;
        const auto it = d.gpu_by_key.find(k);
        char id[96];
        snprintf(id, sizeof id, "%s %ux%u", key, desc.Width, desc.Height);
        if (d.seen.insert(id).second)
            Log("live2d: portrait key %s -> %s", id,
                it == d.gpu_by_key.end() ? "not registered, the game draws it" : d.gpu_slots[d.gpu_presentations[it->second]->slot].slot->name.c_str());
        if (it == d.gpu_by_key.end()) {
            ++d.skipped;
            return PaintResult::Skipped;
        }
        pres_index = it->second;
    }

    ID3D11Device* dev = nullptr;
    target->GetDevice(&dev);
    if (!dev) return PaintResult::Failed;
    ComPtr<ID3D11Device> device_guard;
    device_guard.Attach(dev);
    if (dev != d.device.Get()) {
        // a different device (the first use, or the game recreated it): everything GPU-side starts over
        for (auto& g : d.gpu_slots) { g.gpu.reset(); g.frames.clear(); g.stepped = 0; }
        d.assignment.clear();
        d.renderer.reset();
        d.deferred.Reset();
        d.device = dev;
        d.renderer.reset(new Renderer);
        std::string err;
        if (!d.renderer->Init(dev, &err)) {
            if (!d.reported_failure) Log("live2d: renderer: %s", err.c_str());
            d.reported_failure = true;
            d.renderer.reset();
            d.device.Reset();
            ++d.failures;
            return PaintResult::Failed;
        }
        if (FAILED(dev->CreateDeferredContext(0, &d.deferred))) {
            Log("live2d: the game's device cannot make a deferred context");
            d.renderer.reset();
            d.device.Reset();
            ++d.failures;
            return PaintResult::Failed;
        }
    }

    if (!d.gpu_registry_mode) {  // fallback: handed out in turn when the portrait object is first seen
        auto found = d.assignment.find(portrait);
        if (found == d.assignment.end()) {
            pres_index = d.next_presentation++ % (int)d.gpu_presentations.size();
            d.assignment[portrait] = pres_index;
            ++d.assigned;
        } else {
            pres_index = found->second;
        }
    }
    Presentation& pr = *d.gpu_presentations[pres_index];
    Impl::GpuSlot& gs = d.gpu_slots[pr.slot];
    Character& character = *gs.slot->character;
    if (!gs.gpu) {
        std::string err;
        gs.gpu = d.renderer->CreateModel(character.model(), &err);
        if (!gs.gpu) {
            if (!d.reported_failure) Log("live2d: GPU resources of %s: %s", gs.slot->name.c_str(), err.c_str());
            d.reported_failure = true;
            ++d.failures;
            return PaintResult::Failed;
        }
        Log("live2d: GPU resources created for %s", gs.slot->name.c_str());
    }

    // Time moves in fixed steps of 1/fps. Time since the last call piles up in `pending` and a step is taken when a whole
    // one has piled up, so the rate is fps on average even when the game's frames do not divide evenly. At most one step
    // per call, and the pile is capped so a pause does not turn into a burst. A model is advanced for a step only when one
    // of its portraits is asked for, and every size redraws once per step.
    const uint64_t now = Ticks();
    const double step = 1.0 / fps;
    if (d.last_call) d.pending += (now - d.last_call) / TickFrequency();
    d.last_call = now;
    if (d.serial == 0 || d.pending >= step) {
        d.pending = d.serial == 0 ? 0.0 : std::min(d.pending - step, step * 2);
        ++d.serial;
    }
    // where the GUI mirrors the portrait the picture is drawn flipped, so that it comes out the right way round
    const bool flip = pr.unmirror && PortraitMirrored(portrait);
    if (flip) {
        char id[96];
        snprintf(id, sizeof id, "%s flipped", key);
        if (d.seen.insert(id).second) Log("live2d: portrait %s is mirrored by the GUI; drawing %s flipped so it comes out the right way round", key, gs.slot->name.c_str());
    }

    // Events. What each does is the mod's: a motion picked from the model's own groups, an expression, a line to say.
    auto fire = [&](const EventAction& act, ActionState& state, const char* what) {
        const bool motion = !act.motion_groups.empty() && character.PlayMotionFrom(act.motion_groups, act.motion_index);
        const bool expression = !act.expression.empty() && character.SetExpression(act.expression, act.expression_hold);
        // the line to say: the mod's entry for the motion's group (exact name first, then prefix patterns), else one of its `sounds`, else the
        // Sound that model3.json gives the motion. Several lines are taken in turn, so one is not repeated at once.
        std::filesystem::path line;
        int bound = -1;
        if (motion) {
            const std::string& group = character.last_motion_group();
            for (size_t i = 0; i < act.voices.size() && bound < 0; ++i)
                if (act.voices[i].pattern == group) bound = (int)i;
            for (size_t i = 0; i < act.voices.size() && bound < 0; ++i) {
                const std::string& pat = act.voices[i].pattern;
                if (!pat.empty() && pat.back() == '*' && group.rfind(pat.substr(0, pat.size() - 1), 0) == 0) bound = (int)i;
            }
        }
        if (bound >= 0) line = act.voices[bound].lines[state.voice_next[bound]++ % act.voices[bound].lines.size()];
        else if (!act.sounds.empty()) line = act.sounds[state.sound_next++ % act.sounds.size()];
        else if (motion) line = character.last_motion_sound();
        bool said = false;
        if (audio && !line.empty()) said = Voice::Get().Play(gs.slot.get(), line, act.volume);
        std::string note = motion ? character.last_motion_group() : (act.motion_groups.empty() ? "no motion" : "no matching motion");
        if (expression) note += ", expression " + act.expression;
        else if (!act.expression.empty()) note += ", no expression " + act.expression;
        if (!line.empty()) note += std::string(said ? ", saying " : ", could not play ") + line.filename().string();
        Log("live2d: %s on portrait %s (%s) -> %s", what, key, gs.slot->name.c_str(), note.c_str());
    };
    if (interactions) {
        PortraitEvent ev;
        while (PopPortraitEvent(portrait, &ev)) {
            if (ev.type == PortraitEvent::Type::Hover) {
                if (pr.hover.enabled) fire(pr.hover, pr.states[kStateHover], "hover");
                continue;
            }
            const EventAction* action = &pr.click;
            ActionState* state = &pr.states[kStateClick];
            if (!pr.click_areas.empty()) {  // which part of the model was clicked
                const float u = PortraitMirrored(portrait) && !flip ? 1.0f - ev.u : ev.u;
                float mx, my;
                RectPointToModel(pr.view, character.model(), desc.Width, desc.Height, u, ev.v, &mx, &my);
                const std::string area = character.HitTest(mx, my);
                for (size_t i = 0; i < pr.click_areas.size() && !area.empty(); ++i) {
                    if (SameName(pr.click_areas[i].first, area) && pr.click_areas[i].second.enabled) {
                        action = &pr.click_areas[i].second;
                        state = &pr.states[kStateAreas + i];
                        break;
                    }
                }
            }
            if (action->enabled) fire(*action, *state, action == &pr.click ? "click" : "click on a hit area");
        }
        // the portrait shows up: the first time, a different portrait in the object, or back after a pause
        Impl::Painted& painted = d.last_painted[portrait];
        const bool appeared = painted.tick == 0 || painted.key != key || now - painted.tick > (uint64_t)(1.5 * TickFrequency());
        painted.tick = now;
        painted.key = key;
        if (appeared && pr.appear.enabled) fire(pr.appear, pr.states[kStateAppear], "appear");
        // now and then while it is shown, not over a motion an event started
        if (pr.idle.enabled) {
            auto schedule = [&]() {
                const float seconds = pr.idle.interval_min + (pr.idle.interval_max - pr.idle.interval_min) * (float)(d.rng() % 1000) / 1000.0f;
                pr.next_idle = now + (uint64_t)(seconds * TickFrequency());
            };
            if (pr.next_idle == 0) schedule();
            else if (now >= pr.next_idle) {
                if (!character.PlayingAction()) fire(pr.idle, pr.states[kStateIdle], "idle");
                schedule();
            }
        }
    }

    if (gs.stepped != d.serial) {
        // the look target: where the mouse pointer is, seen from this portrait (or from the middle of the window when the
        // portrait's place on the screen is not known); back to the middle while the game is not the foreground window
        float lx = 0.0f, ly = 0.0f;
        const bool follow = interactions && pr.follow.enabled;
        if (follow) {
            ComputeLookTarget(portrait, &lx, &ly);
            if (flip) lx = -lx;  // the picture is flipped back: no allowance for the GUI's mirror
        }
        character.SetLookTarget(lx, ly, follow ? pr.follow.strength : 0.0f);
        const uint64_t t0 = Ticks();
        character.Tick((float)step);
        d.tick_ticks += Ticks() - t0;
        gs.stepped = d.serial;
        ++d.ticks;
    }

    // frames are kept per presentation (its view differs) and per flip, because a model can be shown mirrored and not mirrored at once
    const uint64_t frame_key = ((uint64_t)desc.Width << 40) | ((uint64_t)desc.Height << 16) | ((uint64_t)(flip ? 1 : 0) << 15) | (uint64_t)desc.Format;
    const std::pair<int, uint64_t> frame_id(pres_index, frame_key);
    Frame& f = gs.frames[frame_id];
    if (!f.texture) {
        D3D11_TEXTURE2D_DESC td = desc;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.SampleDesc.Count = 1;
        td.SampleDesc.Quality = 0;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = 0;
        td.MiscFlags = 0;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &f.texture)) || FAILED(dev->CreateRenderTargetView(f.texture.Get(), nullptr, &f.rtv))) {
            gs.frames.erase(frame_id);
            ++d.failures;
            return PaintResult::Failed;
        }
    }

    ComPtr<ID3D11DeviceContext> immediate;
    dev->GetImmediateContext(&immediate);
    if (f.serial != d.serial) {
        const uint64_t t0 = Ticks();
        View view = pr.view;
        view.flip_x = flip;
        d.renderer->Draw(d.deferred.Get(), *gs.gpu, character.model(), f.rtv.Get(), desc.Width, desc.Height, view);
        ComPtr<ID3D11CommandList> list;
        if (FAILED(d.deferred->FinishCommandList(FALSE, &list))) {
            ++d.failures;
            return PaintResult::Failed;
        }
        // RestoreContextState: the engine's own pipeline state is put back after our commands ran
        immediate->ExecuteCommandList(list.Get(), TRUE);
        f.serial = d.serial;
        d.draw_ticks += Ticks() - t0;
        ++d.draws;
    }
    const uint64_t t1 = Ticks();
    immediate->CopyResource(target, f.texture.Get());
    d.copy_ticks += Ticks() - t1;
    ++d.copies;
    return PaintResult::Painted;
}

void Live2DPainter::Shutdown() {
    Impl& d = *impl_;
    Voice::Get().Shutdown();
    d.ResetGpu();
    std::lock_guard<std::mutex> lock(d.mutex);
    d.slots.clear();  // models before the library that made them
    d.presentations.clear();
    d.by_key.clear();
    d.api.reset();
    d.loaded_core.clear();
    d.loaded_signature.clear();
    d.gpu_generation = 0;
}

std::string Live2DPainter::Stats() const {
    const Impl& d = *impl_;
    const double ms = 1000.0 / TickFrequency();
    char buf[400];
    const double ticks = (double)d.ticks.load(), draws = (double)d.draws.load(), copies = (double)d.copies.load();
    snprintf(buf, sizeof buf,
             "live2d: %llu models, %llu portraits assigned, %llu not registered; advanced %llu (avg %.3f ms), drawn %llu (avg %.3f ms on the render thread), copied %llu (avg %.3f ms), failures %llu",
             (unsigned long long)d.slot_count.load(), (unsigned long long)d.assigned.load(), (unsigned long long)d.skipped.load(), (unsigned long long)d.ticks.load(),
             ticks ? d.tick_ticks.load() * ms / ticks : 0.0, (unsigned long long)d.draws.load(), draws ? d.draw_ticks.load() * ms / draws : 0.0,
             (unsigned long long)d.copies.load(), copies ? d.copy_ticks.load() * ms / copies : 0.0, (unsigned long long)d.failures.load());
    return buf;
}

} // namespace l2d
