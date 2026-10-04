#include "portrait_live2d.hpp"

#include "live2d_character.hpp"
#include "live2d_renderer.hpp"
#include "portrait_input.hpp"
#include "voice.hpp"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <thread>
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

std::string DescribeView(const ViewSpec& v) {
    char buf[160];
    snprintf(buf, sizeof buf, "%d %.3f %.3f %.3f %.3f z%.3f", (int)v.auto_view, v.body, v.x, v.y, v.h, v.scale);
    return buf;
}

bool SameName(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower((unsigned char)x) == std::tolower((unsigned char)y); });
}

struct Frame {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    uint64_t serial = 0;  // the step this frame was drawn for
};

// A model in memory: the Core model with its motions and physics and its animation state, and the GPU resources made from its
// textures. Once published only the render thread touches the character.
struct Loaded {
    std::shared_ptr<core::Api> api;  // keeps the Core library alive as long as the model
    std::shared_ptr<Character> character;
    Model::PortraitBounds bounds;    // measured at load, while nobody else used the model
    uint64_t serial = 0;             // tells one loading of a model from another
    std::mutex gpu_mutex;
    Renderer::GpuPtr gpu;
    ID3D11Device* gpu_device = nullptr;  // the device the resources are for (not a reference)
};

// One model of the mods, however many portrait keys use it. A slot exists as soon as a mod names the model; the model itself is loaded in
// the background (in advance, or when a portrait first needs it) and may be dropped again to make room.
struct Slot {
    enum class State { Idle, Queued, Loading, Failed };
    std::string path, name;
    size_t estimate = 0;              // bytes it takes once loaded
    std::shared_ptr<Loaded> loaded;   // under Impl::mutex
    State state = State::Idle;        // under Impl::mutex
    std::atomic<uint64_t> last_used{ 0 };
};

std::atomic<uint64_t> g_loaded_serial{ 1 };

// What a model takes once loaded, worked out from the sizes of its files without reading them: the moc3 and the textures (a DDS file is what
// the GPU gets; a PNG decodes to RGBA with mips).
size_t EstimateBytes(const std::string& model3) {
    namespace fs = std::filesystem;
    size_t bytes = 1u << 20;  // the Core model, motions and physics
    try {
        std::ifstream f(fs::u8path(model3));
        const auto j = nlohmann::json::parse(f);
        const fs::path dir = fs::u8path(model3).parent_path();
        const auto& refs = j["FileReferences"];
        std::error_code ec;
        if (refs.contains("Moc")) bytes += (size_t)fs::file_size(dir / fs::u8path(refs["Moc"].get<std::string>()), ec);
        for (const auto& t : refs.value("Textures", nlohmann::json::array())) {
            const fs::path file = dir / fs::u8path(t.get<std::string>());
            const size_t size = (size_t)fs::file_size(file, ec);
            if (file.extension() == ".dds") {
                bytes += size;
            } else if (file.extension() == ".png") {
                std::ifstream png(file, std::ios::binary);
                unsigned char h[24] = {};
                png.read((char*)h, 24);
                const size_t w = (size_t)h[16] << 24 | (size_t)h[17] << 16 | (size_t)h[18] << 8 | h[19];
                const size_t ht = (size_t)h[20] << 24 | (size_t)h[21] << 16 | (size_t)h[22] << 8 | h[23];
                bytes += w * ht * 4 * 4 / 3;
            } else {
                bytes += size * 8;
            }
        }
    } catch (...) {
    }
    return bytes;
}

// The GPU resources of a loaded model for `device`, made if missing (by the loader just after the load, or by the render thread, whichever
// comes first), and the pixel data of the textures freed once the GPU has them. False when that cannot be done because the textures were
// already freed for another device: the model has to be loaded again.
bool EnsureGpu(Loaded& l, Renderer& renderer, ID3D11Device* device, std::string* error) {
    std::lock_guard<std::mutex> lock(l.gpu_mutex);
    if (l.gpu && l.gpu_device == device) return true;
    l.gpu.reset();
    Model& model = l.character->model();
    if (model.textures_released) {
        *error = "its textures were freed for another device";
        return false;
    }
    l.gpu = renderer.CreateModel(model, error);
    if (!l.gpu) return false;
    l.gpu_device = device;
    model.ReleaseTextures();
    return true;
}

// How a portrait key shows a model: the framing, and what it does when the mouse is near or it is clicked. Cheap: the model, its
// textures and its animation are the slot's; this holds only settings, and the frames drawn for it.
struct ActionState {
    std::vector<uint32_t> voice_next;  // render thread: per `voices` entry of the action, which of its lines comes next
    uint32_t sound_next = 0;           // the same for its `sounds`
};
enum { kStateClick = 0, kStateHover = 1, kStateAppear = 2, kStateIdle = 3, kStateGreeting = 4, kStateAreas = 5 };

// One framing of a presentation: the default, or one for a kind of portrait or a size of picture.
struct ViewVariant {
    ViewSpec spec;
    View view;                  // the view in use: worked out from the loaded model's bounds when automatic
    uint64_t view_serial = 0;   // the loading of the model that `view` was worked out for
};

struct Presentation {
    int slot = -1;  // index into the slots
    std::vector<ViewVariant> variants;  // [0] the default, then the ones for a kind or size
    MouseFollow follow;
    EventAction click, hover, appear, idle, greeting;
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
    std::unordered_map<std::string, std::shared_ptr<Slot>> slot_by_path;  // every model a mod names
    struct Request {
        std::shared_ptr<Slot> slot;  // null: make the GPU resources of the loaded models
        bool demand = false;         // a portrait waits for it (otherwise a load in advance, which pushes nothing out)
    };
    std::deque<Request> queue;
    std::condition_variable cv;
    std::thread loader;
    bool stop = false;
    size_t loaded_bytes = 0, budget = (size_t)512 << 20;
    std::shared_ptr<Renderer> shared_renderer;  // the render thread's renderer and device, for the loader to make GPU resources with
    ID3D11Device* shared_device = nullptr;
    void LoaderMain();
    void EvictFor(size_t need, const Slot* keep);  // with the mutex held
    std::vector<std::shared_ptr<Presentation>> presentations;
    uint64_t generation = 0;  // changes whenever the slots or presentations do
    int fps = 30;
    bool physics = true;
    bool interactions = true;
    bool audio = true;
    int supersample = 2;
    std::string loaded_core;
    std::string loaded_signature;                // what the slots were made from, to see when that changes
    std::unordered_map<std::string, int> by_key; // portrait key -> presentation, when the mods register portraits
    bool registry_mode = false;
    bool voice_failed = false;  // worker thread: opening the playback device failed, do not retry every two seconds

    // --- render thread only
    struct GpuSlot {
        std::shared_ptr<Slot> slot;
        std::map<std::pair<int, uint64_t>, Frame> frames;  // by presentation, then output size, format and flip
        uint64_t stepped = 0;                              // the last step this model was advanced for
        uint64_t loaded_serial = 0;                        // the loading of the model the frames and `stepped` belong to
    };
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> deferred;
    std::shared_ptr<Renderer> renderer;
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
    std::atomic<uint64_t> ticks{ 0 }, draws{ 0 }, copies{ 0 }, failures{ 0 }, assigned{ 0 }, slot_count{ 0 }, skipped{ 0 }, waiting{ 0 };
    std::atomic<uint64_t> loads{ 0 }, evictions{ 0 }, loaded_now{ 0 }, loaded_mb{ 0 };
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

// Drops loaded models, the one unused for longest first (and none used in the last few seconds), until `need` more bytes fit in the budget.
void Live2DPainter::Impl::EvictFor(size_t need, const Slot* keep) {
    const uint64_t now = Ticks();
    const uint64_t quiet = (uint64_t)(3.0 * TickFrequency());
    std::vector<std::shared_ptr<Slot>> candidates;
    for (auto& [path, slot] : slot_by_path)
        if (slot.get() != keep && slot->loaded && now - slot->last_used.load() > quiet) candidates.push_back(slot);
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a->last_used.load() < b->last_used.load(); });
    for (const auto& slot : candidates) {
        if (loaded_bytes + need <= budget) break;
        Log("live2d: dropping %s from memory (the model cache is full)", slot->name.c_str());
        slot->loaded.reset();
        loaded_bytes -= std::min(loaded_bytes, slot->estimate);
        ++evictions;
    }
}

// The loader thread: models are loaded here, never on the game's render thread, and their textures are uploaded here as soon as the render
// thread has shown its device.
void Live2DPainter::Impl::LoaderMain() {
    for (;;) {
        Request req;
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&] { return stop || !queue.empty(); });
            if (stop) return;
            req = std::move(queue.front());
            queue.pop_front();
        }
        if (!req.slot) {  // the render thread has a device now: put the loaded models' textures on it
            std::vector<std::shared_ptr<Loaded>> todo;
            std::shared_ptr<Renderer> r;
            ID3D11Device* dev;
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (auto& [path, slot] : slot_by_path)
                    if (slot->loaded) todo.push_back(slot->loaded);
                r = shared_renderer;
                dev = shared_device;
            }
            for (auto& l : todo) {
                std::string e;
                if (r && !EnsureGpu(*l, *r, dev, &e)) Log("live2d: GPU resources: %s", e.c_str());
            }
            continue;
        }
        std::shared_ptr<core::Api> api_copy;
        bool physics_copy;
        {
            std::lock_guard<std::mutex> lock(mutex);
            Slot& s = *req.slot;
            if (s.loaded || s.state == Slot::State::Failed) {
                if (s.state == Slot::State::Queued) s.state = Slot::State::Idle;
                continue;
            }
            if (loaded_bytes + s.estimate > budget) {
                if (!req.demand) {  // a load in advance only fills what is free
                    s.state = Slot::State::Idle;
                    continue;
                }
                EvictFor(s.estimate, &s);
            }
            s.state = Slot::State::Loading;
            api_copy = api;
            physics_copy = physics;
        }
        Slot& slot = *req.slot;
        auto fail = [&](const std::string& why) {
            std::lock_guard<std::mutex> lock(mutex);
            slot.state = Slot::State::Failed;
            Log("live2d: cannot load %s: %s", slot.path.c_str(), why.c_str());
        };
        if (!api_copy) {
            fail("the Cubism Core is not loaded");
            continue;
        }
        const uint64_t t0 = Ticks();
        auto loaded = std::make_shared<Loaded>();
        loaded->api = api_copy;
        loaded->character = std::make_shared<Character>();
        std::string err;
        if (!loaded->character->Load(api_copy.get(), slot.path, &err)) {
            fail(err);
            continue;
        }
        loaded->character->set_physics_enabled(physics_copy);
        if (!loaded->character->physics_error().empty()) Log("live2d: %s: physics not loaded: %s", slot.path.c_str(), loaded->character->physics_error().c_str());
        loaded->bounds = loaded->character->model().MeasurePortrait();
        loaded->serial = g_loaded_serial++;
        std::shared_ptr<Renderer> r;
        ID3D11Device* dev;
        {
            std::lock_guard<std::mutex> lock(mutex);
            r = shared_renderer;
            dev = shared_device;
        }
        const Model& m = loaded->character->model();
        const double texture_mb = [&] { size_t b = 0; for (const Image& i : m.textures) b += i.Bytes(); return b / 1048576.0; }();
        const int canvas_w = (int)m.canvas_size.x, canvas_h = (int)m.canvas_size.y;
        std::string gpu_err;
        const bool uploaded = r && EnsureGpu(*loaded, *r, dev, &gpu_err);
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto known = slot_by_path.find(slot.path);
            if (stop || known == slot_by_path.end() || known->second.get() != &slot) {
                slot.state = Slot::State::Idle;  // no mod names it any more
                continue;
            }
            slot.loaded = loaded;
            slot.state = Slot::State::Idle;
            loaded_bytes += slot.estimate;
            ++loads;
        }
        Log("live2d: loaded %s in %.0f ms in the background: canvas %dx%d, %zu textures (%.1f MB)%s", slot.name.c_str(), (Ticks() - t0) * 1000.0 / TickFrequency(),
            canvas_w, canvas_h, m.textures.size(), texture_mb, uploaded ? ", on the GPU" : "");
    }
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
        ViewSpec view;
        std::vector<ViewSpec> views;
        bool unmirror = true;
        MouseFollow follow;
        EventAction click, hover, appear, idle, greeting;
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
        w.view = e.view;
        w.views = e.views;
        w.unmirror = e.unmirror;
        w.follow = e.mouse_follow;
        w.click = e.click;
        w.hover = e.hover;
        w.appear = e.appear;
        w.idle = e.idle;
        w.greeting = e.greeting;
        w.click_areas = e.click_areas;
        char id[256];
        snprintf(id, sizeof id, "%d|m%d f%.2f u%d|", w.model, (int)e.mouse_follow.enabled, e.mouse_follow.strength, (int)e.unmirror);
        w.identity = std::string(id) + DescribeView(w.view);
        for (const ViewSpec& v : w.views) w.identity += "|view " + v.selector + " " + DescribeView(v);
        w.identity += std::string("|click ") + Describe(e.click) + "|hover " + Describe(e.hover) + "|appear " + Describe(e.appear) + "|idle " +
                     Describe(e.idle) + "|greeting " + Describe(e.greeting);
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
            w.view.auto_view = e.auto_view;
            w.view.body = e.auto_body;
            w.view.x = e.view_x;
            w.view.y = e.view_y;
            w.view.h = e.view_h;
            w.unmirror = false;
            char id[64];
            snprintf(id, sizeof id, "list %zu|%d|", wants.size(), w.model);
            w.identity = std::string(id) + DescribeView(w.view);
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
            d.slot_by_path.clear();
            d.queue.clear();
            d.loaded_bytes = 0;
            d.presentations.clear();
            d.by_key.clear();
            d.loaded_signature.clear();
            ++d.generation;
        }
        return;
    }
    std::shared_ptr<core::Api> api;
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        d.fps = s.fps < 1 ? 1 : s.fps > 120 ? 120 : s.fps;
        d.interactions = s.interactions;
        d.audio = s.audio;
        d.supersample = s.supersample;
        d.budget = s.model_cache_mb > 0 ? (size_t)s.model_cache_mb << 20 : (size_t)-1;
        if (d.physics != s.physics) {
            d.physics = s.physics;
            for (auto& [path, slot] : d.slot_by_path)
                if (slot->loaded) slot->loaded->character->set_physics_enabled(s.physics);
        }
        if (!d.slots.empty() && d.loaded_signature == signature) return;
        if (d.api && d.loaded_core == s.core_dll) api = d.api;
    }

    // the Core library: loaded here, outside the lock
    std::string err;
    if (!api) {
        api = std::make_shared<core::Api>();
        if (!api->Load(std::wstring(s.core_dll.begin(), s.core_dll.end()), &err)) {
            Log("live2d: %s: %s", s.core_dll.c_str(), err.c_str());
            return;
        }
        Log("live2d: Cubism Core loaded from %s (version 0x%08X)", s.core_dll.c_str(), api->GetVersion());
    }
    if (!d.loader.joinable()) d.loader = std::thread(&Impl::LoaderMain, &d);

    // The models are not loaded here: a slot per model is made (or kept, with what is loaded of it) and the loader thread is asked to load
    // them in advance as far as the memory limit allows; a portrait that needs one before that asks for it itself.
    std::lock_guard<std::mutex> lock(d.mutex);
    if (d.api != api) {  // another Core library: nothing loaded with the old one may stay
        d.slot_by_path.clear();
        d.queue.clear();
        d.loaded_bytes = 0;
    }
    std::vector<std::shared_ptr<Slot>> slots;
    for (const std::string& path : paths) {
        auto it = d.slot_by_path.find(path);
        if (it == d.slot_by_path.end()) {
            auto slot = std::make_shared<Slot>();
            slot->path = path;
            slot->name = std::filesystem::path(path).parent_path().filename().string();
            slot->estimate = EstimateBytes(path);
            it = d.slot_by_path.emplace(path, std::move(slot)).first;
        } else if (it->second->state == Slot::State::Failed) {
            it->second->state = Slot::State::Idle;  // an edit may have mended it
        }
        slots.push_back(it->second);
    }
    for (auto it = d.slot_by_path.begin(); it != d.slot_by_path.end();) {  // models no mod names any more
        if (std::find(paths.begin(), paths.end(), it->first) == paths.end()) {
            if (it->second->loaded) d.loaded_bytes -= std::min(d.loaded_bytes, it->second->estimate);
            it = d.slot_by_path.erase(it);
        } else {
            ++it;
        }
    }
    // the presentations: the view is worked out when the model is loaded (it depends on the model's bounds), magnified by the scale
    std::vector<std::shared_ptr<Presentation>> presentations;
    for (const PresWant& w : wants) {
        auto p = std::make_shared<Presentation>();
        p->slot = w.model;
        p->variants.push_back({ w.view, {}, 0 });
        for (const ViewSpec& v : w.views) p->variants.push_back({ v, {}, 0 });
        p->follow = w.follow;
        p->click = w.click;
        p->hover = w.hover;
        p->appear = w.appear;
        p->idle = w.idle;
        p->greeting = w.greeting;
        p->click_areas = w.click_areas;
        p->states.resize(kStateAreas + w.click_areas.size());
        p->states[kStateClick].voice_next.assign(w.click.voices.size(), 0);
        p->states[kStateHover].voice_next.assign(w.hover.voices.size(), 0);
        p->states[kStateAppear].voice_next.assign(w.appear.voices.size(), 0);
        p->states[kStateIdle].voice_next.assign(w.idle.voices.size(), 0);
        p->states[kStateGreeting].voice_next.assign(w.greeting.voices.size(), 0);
        for (size_t i = 0; i < w.click_areas.size(); ++i) p->states[kStateAreas + i].voice_next.assign(w.click_areas[i].second.voices.size(), 0);
        p->unmirror = w.unmirror;
        p->identity = w.identity;
        presentations.push_back(std::move(p));
    }
    d.slots = std::move(slots);
    d.presentations = std::move(presentations);
    d.by_key = std::move(by_key);
    d.registry_mode = registry_mode;
    ++d.generation;
    d.api = api;
    d.loaded_core = s.core_dll;
    d.loaded_signature = signature;
    d.slot_count = d.slots.size();
    for (const auto& slot : d.slots) {
        if (!slot->loaded && slot->state == Slot::State::Idle) {
            slot->state = Slot::State::Queued;
            d.queue.push_back({ slot, false });
        }
    }
    d.cv.notify_all();
    if (registry_mode) Log("live2d: %zu portrait key(s) use %zu presentation(s) of %zu model(s); loading them in the background (cache %d MB)", d.by_key.size(), d.presentations.size(), d.slots.size(), s.model_cache_mb);
}

bool Live2DPainter::Ready() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return !impl_->slots.empty();
}

PaintResult Live2DPainter::Paint(const void* portrait, const char* key, int kind, const char* scope, ID3D11Texture2D* target, const D3D11_TEXTURE2D_DESC& desc) {
    Impl& d = *impl_;
    int fps, supersample;
    bool interactions, audio;
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        fps = d.fps;
        interactions = d.interactions;
        audio = d.audio;
        supersample = d.supersample;
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
        char id[224];
        snprintf(id, sizeof id, "%s %ux%u kind %d (%s)", key, desc.Width, desc.Height, kind, scope);
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
        // a different device (the first use, or the game recreated it): the GPU side starts over. Models loaded for another device lost their
        // textures to it and must be loaded again; the ones loaded before any device was known get theirs from the loader.
        {
            std::lock_guard<std::mutex> lock(d.mutex);
            for (auto& [path, slot] : d.slot_by_path) {
                if (!slot->loaded) continue;
                std::lock_guard<std::mutex> gpu_lock(slot->loaded->gpu_mutex);
                if (slot->loaded->gpu_device && slot->loaded->gpu_device != dev) {
                    d.loaded_bytes -= std::min(d.loaded_bytes, slot->estimate);
                    slot->loaded.reset();
                }
            }
            d.shared_renderer.reset();
            d.shared_device = nullptr;
        }
        for (auto& g : d.gpu_slots) { g.frames.clear(); g.stepped = 0; }
        d.assignment.clear();
        d.renderer.reset();
        d.deferred.Reset();
        d.device = dev;
        auto renderer = std::make_shared<Renderer>();
        std::string err;
        if (!renderer->Init(dev, &err)) {
            if (!d.reported_failure) Log("live2d: renderer: %s", err.c_str());
            d.reported_failure = true;
            d.device.Reset();
            ++d.failures;
            return PaintResult::Failed;
        }
        if (FAILED(dev->CreateDeferredContext(0, &d.deferred))) {
            Log("live2d: the game's device cannot make a deferred context");
            d.device.Reset();
            ++d.failures;
            return PaintResult::Failed;
        }
        d.renderer = renderer;
        {
            std::lock_guard<std::mutex> lock(d.mutex);
            d.shared_renderer = renderer;
            d.shared_device = dev;
            d.queue.push_back({ nullptr, false });  // the loader puts the models loaded so far on the new device
            for (auto& [path, slot] : d.slot_by_path)  // and the ones dropped above are loaded again
                if (!slot->loaded && slot->state == Slot::State::Idle) { slot->state = Slot::State::Queued; d.queue.push_back({ slot, false }); }
        }
        d.cv.notify_all();
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
    // The model is loaded in the background. Until it is, the game's own portrait stays; the first portrait that needs it asks for it.
    std::shared_ptr<Loaded> loaded;
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        loaded = gs.slot->loaded;
        if (!loaded && gs.slot->state == Slot::State::Idle) {
            gs.slot->state = Slot::State::Queued;
            d.queue.push_back({ gs.slot, true });
            d.cv.notify_one();
        }
    }
    if (!loaded) {
        ++d.waiting;
        return PaintResult::Skipped;
    }
    gs.slot->last_used = Ticks();
    {
        std::string err;
        if (!EnsureGpu(*loaded, *d.renderer, dev, &err)) {
            if (!d.reported_failure) Log("live2d: GPU resources of %s: %s", gs.slot->name.c_str(), err.c_str());
            d.reported_failure = true;
            ++d.failures;
            std::lock_guard<std::mutex> lock(d.mutex);  // freed textures cannot be uploaded again: load the model once more
            if (gs.slot->loaded == loaded) {
                gs.slot->loaded.reset();
                d.loaded_bytes -= std::min(d.loaded_bytes, gs.slot->estimate);
            }
            return PaintResult::Failed;
        }
    }
    Character& character = *loaded->character;
    if (gs.loaded_serial != loaded->serial) {  // a model loaded anew: its frames and its step counter start over
        gs.frames.clear();
        gs.stepped = 0;
        gs.loaded_serial = loaded->serial;
    }
    // the framing for this portrait: a size beats a kind beats the default
    size_t variant_index = 0;
    int specificity = 0;
    for (size_t i = 1; i < pr.variants.size(); ++i) {
        const ViewSpec& v = pr.variants[i].spec;
        const int sp = (v.width && v.width == (int)desc.Width && v.height == (int)desc.Height) ? 2 : (v.kind >= 0 && v.kind == kind) ? 1 : 0;
        if (sp > specificity) {
            specificity = sp;
            variant_index = i;
        }
    }
    ViewVariant& variant = pr.variants[variant_index];
    if (variant.view_serial != loaded->serial) {  // the view of an automatic framing comes from the loaded model's bounds
        const ViewSpec& v = variant.spec;
        variant.view = { v.x, v.y, v.h };
        if (v.auto_view) Model::ViewFromBounds(loaded->bounds, v.body, &variant.view.center_x, &variant.view.center_y, &variant.view.height);
        variant.view.height /= v.scale;  // live2d_scale: magnify around the middle of the framed part
        variant.view_serial = loaded->serial;
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
        const bool motion = !act.motion_groups.empty() && character.PlayMotionFrom(act.motion_groups, act.motion_index, &act.ignore_parameters);
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
            if (ev.type == PortraitEvent::Type::Greeting) {
                if (pr.greeting.enabled) fire(pr.greeting, pr.states[kStateGreeting], "greeting");
                continue;
            }
            const EventAction* action = &pr.click;
            ActionState* state = &pr.states[kStateClick];
            if (!pr.click_areas.empty()) {  // which part of the model was clicked
                const float u = PortraitMirrored(portrait) && !flip ? 1.0f - ev.u : ev.u;
                float mx, my;
                RectPointToModel(variant.view, character.model(), desc.Width, desc.Height, u, ev.v, &mx, &my);
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
        character.SetVoiceLevel(audio ? Voice::Get().Level(gs.slot.get()) : 0.0f);
        const uint64_t t0 = Ticks();
        character.Tick((float)step);
        d.tick_ticks += Ticks() - t0;
        gs.stepped = d.serial;
        ++d.ticks;
    }

    // frames are kept per presentation (its view differs) and per flip, because a model can be shown mirrored and not mirrored at once
    const uint64_t frame_key = ((uint64_t)variant_index << 56) | ((uint64_t)desc.Width << 40) | ((uint64_t)desc.Height << 16) | ((uint64_t)(flip ? 1 : 0) << 15) | (uint64_t)desc.Format;
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
        View view = variant.view;
        view.flip_x = flip;
        d.renderer->SetSupersample(supersample);
        d.renderer->Draw(d.deferred.Get(), *loaded->gpu, character.model(), f.rtv.Get(), desc.Width, desc.Height, view);
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

bool Live2DPainter::GreetingReplaced(const char* key) {
    Impl& d = *impl_;
    if (!d.gpu_registry_mode || !key || !key[0]) return false;  // render thread state: the hook runs on the render thread
    const auto it = d.gpu_by_key.find(key);
    if (it == d.gpu_by_key.end()) return false;
    const Presentation& pr = *d.gpu_presentations[it->second];
    return pr.greeting.enabled && pr.greeting.replace_engine_sound;
}

void Live2DPainter::Shutdown() {
    Impl& d = *impl_;
    Voice::Get().Shutdown();
    {  // the loader first: nothing may load while everything is taken down
        std::lock_guard<std::mutex> lock(d.mutex);
        d.stop = true;
    }
    d.cv.notify_all();
    if (d.loader.joinable()) d.loader.join();
    d.ResetGpu();
    std::lock_guard<std::mutex> lock(d.mutex);
    d.stop = false;
    d.queue.clear();
    d.shared_renderer.reset();
    d.shared_device = nullptr;
    d.loaded_bytes = 0;
    d.slot_by_path.clear();
    d.slots.clear();  // models before the library that made them
    d.presentations.clear();
    d.by_key.clear();
    d.api.reset();
    d.loaded_core.clear();
    d.loaded_signature.clear();
    d.gpu_generation = 0;
}

std::string Live2DPainter::Stats() const {
    Impl& d = *impl_;
    const double ms = 1000.0 / TickFrequency();
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        d.loaded_now = 0;
        for (const auto& [path, slot] : d.slot_by_path)
            if (slot->loaded) ++d.loaded_now;
        d.loaded_mb = d.loaded_bytes >> 20;
    }
    char buf[520];
    const double ticks = (double)d.ticks.load(), draws = (double)d.draws.load(), copies = (double)d.copies.load();
    snprintf(buf, sizeof buf,
             "live2d: %llu models (%llu in memory, %llu MB, %llu loads, %llu dropped), %llu portraits assigned, %llu not registered, %llu waiting for a model; advanced %llu (avg %.3f ms), drawn %llu (avg %.3f ms on the render thread), copied %llu (avg %.3f ms), failures %llu",
             (unsigned long long)d.slot_count.load(), (unsigned long long)d.loaded_now.load(), (unsigned long long)d.loaded_mb.load(), (unsigned long long)d.loads.load(),
             (unsigned long long)d.evictions.load(), (unsigned long long)d.assigned.load(), (unsigned long long)d.skipped.load(), (unsigned long long)d.waiting.load(), (unsigned long long)d.ticks.load(),
             ticks ? d.tick_ticks.load() * ms / ticks : 0.0, (unsigned long long)d.draws.load(), draws ? d.draw_ticks.load() * ms / draws : 0.0,
             (unsigned long long)d.copies.load(), copies ? d.copy_ticks.load() * ms / copies : 0.0, (unsigned long long)d.failures.load());
    return buf;
}

} // namespace l2d
