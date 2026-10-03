#include "portrait_live2d.hpp"

#include "live2d_character.hpp"
#include "live2d_renderer.hpp"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <map>
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

struct Frame {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    uint64_t serial = 0;  // the step this frame was drawn for
};

// A loaded model and the view that frames it as a portrait. Shared between the worker and the render thread; after it is
// published only the render thread touches the character.
struct Slot {
    std::shared_ptr<Character> character;
    View view;
    std::string name;
};

} // namespace

struct Live2DPainter::Impl {
    // --- shared between the worker and the render thread, under `mutex`
    std::mutex mutex;
    std::shared_ptr<core::Api> api;
    std::vector<std::shared_ptr<Slot>> slots;
    uint64_t generation = 0;  // changes whenever `slots` does
    int fps = 30;
    bool physics = true;
    std::string loaded_core;
    std::string loaded_signature;                // what the slots were made from, to see when that changes
    std::unordered_map<std::string, int> by_key; // portrait key -> slot, when the mods register portraits
    bool registry_mode = false;

    // --- render thread only
    struct GpuSlot {
        std::shared_ptr<Slot> slot;
        Renderer::GpuPtr gpu;
        std::map<uint64_t, Frame> frames;  // by output size and format
        uint64_t stepped = 0;              // the last step this model was advanced for
    };
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> deferred;
    std::unique_ptr<Renderer> renderer;
    std::vector<GpuSlot> gpu_slots;
    uint64_t gpu_generation = 0;
    std::unordered_map<const void*, int> assignment;  // fallback mode: portrait object -> slot
    std::unordered_map<std::string, int> gpu_by_key;  // registry mode: copy of by_key for this generation
    bool gpu_registry_mode = false;
    std::set<std::string> seen;                       // portrait keys already logged
    int next_slot = 0;
    uint64_t last_call = 0, serial = 0;
    double pending = 0.0;  // seconds since the last step that have not been stepped yet
    bool reported_failure = false;

    // --- statistics
    std::atomic<uint64_t> ticks{ 0 }, draws{ 0 }, copies{ 0 }, failures{ 0 }, assigned{ 0 }, slot_count{ 0 }, skipped{ 0 };
    std::atomic<uint64_t> tick_ticks{ 0 }, draw_ticks{ 0 }, copy_ticks{ 0 };

    void ResetGpu() {
        gpu_slots.clear();
        assignment.clear();
        gpu_by_key.clear();
        seen.clear();
        next_slot = 0;
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

    // what to load: the models the mods register (several portrait keys can share one), or else the ini's list
    struct Want {
        std::string path;
        bool auto_view;
        float body, x, y, h;
        std::string actions;  // part of what makes two uses of one model different slots
    };
    std::vector<Want> wants;
    std::unordered_map<std::string, int> by_key;
    for (const PortraitEntry& e : registry.entries) {
        if (!e.live2d) continue;  // spine entries are read but not drawn yet
        char id[256];
        snprintf(id, sizeof id, "m%d f%.2f c%d:%s:%d d%d s%d", (int)e.mouse_follow.enabled, e.mouse_follow.strength, (int)e.click.enabled,
                 e.click.motion_group.c_str(), e.click.motion_index, (int)e.drag.enabled, (int)e.scale.enabled);
        Want w{ e.model, e.auto_view, e.auto_body, e.view_x, e.view_y, e.view_h, id };
        int index = -1;
        for (size_t i = 0; i < wants.size(); ++i) {
            const Want& o = wants[i];
            if (o.path == w.path && o.auto_view == w.auto_view && o.body == w.body && o.x == w.x && o.y == w.y && o.h == w.h && o.actions == w.actions) {
                index = (int)i;
                break;
            }
        }
        if (index < 0) { wants.push_back(w); index = (int)wants.size() - 1; }
        by_key[e.key] = index;
    }
    const bool registry_mode = !wants.empty();
    if (!registry_mode) {
        for (const Settings::ModelEntry& e : s.models) wants.push_back({ e.path, e.auto_view, e.auto_body, e.view_x, e.view_y, e.view_h, "" });
    }
    std::string signature = s.core_dll + (registry_mode ? "|registry" : "|list");
    for (const Want& w : wants) {
        char buf[64];
        snprintf(buf, sizeof buf, "|%d %.3f %.3f %.3f %.3f ", (int)w.auto_view, w.body, w.x, w.y, w.h);
        signature += "\n" + w.path + buf + w.actions;
    }
    {
        std::vector<std::pair<std::string, int>> keys(by_key.begin(), by_key.end());
        std::sort(keys.begin(), keys.end());
        for (const auto& k : keys) signature += "\n" + k.first + "=" + std::to_string(k.second);
    }

    if (!s.live2d || s.core_dll.empty() || wants.empty()) {
        std::lock_guard<std::mutex> lock(d.mutex);
        if (!d.slots.empty()) {
            Log("live2d: models unloaded");
            d.slots.clear();
            d.by_key.clear();
            d.loaded_signature.clear();
            ++d.generation;
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        d.fps = s.fps < 1 ? 1 : s.fps > 120 ? 120 : s.fps;
        if (d.physics != s.physics) {
            d.physics = s.physics;
            for (auto& slot : d.slots) slot->character->set_physics_enabled(s.physics);
        }
        if (!d.slots.empty() && d.loaded_signature == signature) return;
    }

    // load outside the lock: the render thread keeps drawing whatever is current
    std::string err;
    std::shared_ptr<core::Api> api;
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        if (d.api && d.loaded_core == s.core_dll) api = d.api;
    }
    if (!api) {
        api = std::make_shared<core::Api>();
        if (!api->Load(std::wstring(s.core_dll.begin(), s.core_dll.end()), &err)) {
            Log("live2d: %s: %s", s.core_dll.c_str(), err.c_str());
            return;
        }
        Log("live2d: Cubism Core loaded from %s (version 0x%08X)", s.core_dll.c_str(), api->GetVersion());
    }
    std::vector<std::shared_ptr<Slot>> slots;
    std::vector<int> slot_of_want(wants.size(), -1);
    for (size_t wi = 0; wi < wants.size(); ++wi) {
        const Want& w = wants[wi];
        const uint64_t t0 = Ticks();
        auto character = std::make_shared<Character>();
        if (!character->Load(api.get(), w.path, &err)) {
            Log("live2d: cannot load %s: %s", w.path.c_str(), err.c_str());
            continue;
        }
        character->set_physics_enabled(s.physics);
        if (!character->physics_error().empty()) Log("live2d: %s: physics not loaded: %s", w.path.c_str(), character->physics_error().c_str());
        auto slot = std::make_shared<Slot>();
        slot->character = character;
        slot->name = std::filesystem::path(w.path).parent_path().filename().string();
        slot->view = { w.x, w.y, w.h };
        if (w.auto_view) character->model().SuggestPortraitView(&slot->view.center_x, &slot->view.center_y, &slot->view.height, w.body);
        const Model& m = character->model();
        Log("live2d: loaded %s in %.0f ms: canvas %.0fx%.0f, %d parameters, %d drawables, %zu textures (%.1f MB); view (%.3f, %.3f, %.3f)%s",
            slot->name.c_str(), (Ticks() - t0) * 1000.0 / TickFrequency(), m.canvas_size.x, m.canvas_size.y, m.parameter_count,
            m.drawable_count, m.textures.size(), [&] { size_t b = 0; for (const Image& i : m.textures) b += i.Bytes(); return b / 1048576.0; }(),
            slot->view.center_x, slot->view.center_y, slot->view.height, w.auto_view ? " (auto)" : "");
        slot_of_want[wi] = (int)slots.size();
        slots.push_back(std::move(slot));
    }
    std::unordered_map<std::string, int> keys;
    for (const auto& [key, wi] : by_key)
        if (slot_of_want[wi] >= 0) keys[key] = slot_of_want[wi];
    std::lock_guard<std::mutex> lock(d.mutex);
    d.slots = std::move(slots);  // the previous slots live on until the render thread drops its copies
    d.by_key = std::move(keys);
    d.registry_mode = registry_mode;
    ++d.generation;
    d.api = api;
    d.loaded_core = s.core_dll;
    d.loaded_signature = signature;
    d.slot_count = d.slots.size();
    if (registry_mode) Log("live2d: %zu portrait key(s) are registered for %zu model(s)", d.by_key.size(), d.slots.size());
}

bool Live2DPainter::Ready() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return !impl_->slots.empty();
}

PaintResult Live2DPainter::Paint(const void* portrait, const char* key, ID3D11Texture2D* target, const D3D11_TEXTURE2D_DESC& desc) {
    Impl& d = *impl_;
    int fps;
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        fps = d.fps;
        if (d.slots.empty()) return PaintResult::Skipped;
        if (d.generation != d.gpu_generation) {
            // the set of models changed: start over with the new ones (GPU resources are made when a model is first used)
            d.gpu_slots.clear();
            d.assignment.clear();
            d.next_slot = 0;
            d.gpu_by_key = d.by_key;
            d.gpu_registry_mode = d.registry_mode;
            d.seen.clear();
            for (const auto& slot : d.slots) { Impl::GpuSlot g; g.slot = slot; d.gpu_slots.push_back(std::move(g)); }
            d.gpu_generation = d.generation;
        }
    }

    // which model this portrait gets: the one registered for its key, or in the fallback mode the next one in turn
    int index = -1;
    if (d.gpu_registry_mode) {
        if (!key[0]) return PaintResult::Skipped;  // planets and other objects that show no portrait have no key
        const std::string k = key;
        const auto it = d.gpu_by_key.find(k);
        char id[96];
        snprintf(id, sizeof id, "%s %ux%u", key, desc.Width, desc.Height);
        if (d.seen.insert(id).second)
            Log("live2d: portrait key %s -> %s", id, it == d.gpu_by_key.end() ? "not registered, the game draws it" : d.gpu_slots[it->second].slot->name.c_str());
        if (it == d.gpu_by_key.end()) {
            ++d.skipped;
            return PaintResult::Skipped;
        }
        index = it->second;
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
            index = d.next_slot++ % (int)d.gpu_slots.size();
            d.assignment[portrait] = index;
            ++d.assigned;
        } else {
            index = found->second;
        }
    }
    Impl::GpuSlot& gs = d.gpu_slots[index];
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
    if (gs.stepped != d.serial) {
        const uint64_t t0 = Ticks();
        character.Tick((float)step);
        d.tick_ticks += Ticks() - t0;
        gs.stepped = d.serial;
        ++d.ticks;
    }

    const uint64_t frame_key = ((uint64_t)desc.Width << 40) | ((uint64_t)desc.Height << 16) | (uint64_t)desc.Format;
    Frame& f = gs.frames[frame_key];
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
            gs.frames.erase(frame_key);
            ++d.failures;
            return PaintResult::Failed;
        }
    }

    ComPtr<ID3D11DeviceContext> immediate;
    dev->GetImmediateContext(&immediate);
    if (f.serial != d.serial) {
        const uint64_t t0 = Ticks();
        d.renderer->Draw(d.deferred.Get(), *gs.gpu, character.model(), f.rtv.Get(), desc.Width, desc.Height, gs.slot->view);
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
    d.ResetGpu();
    std::lock_guard<std::mutex> lock(d.mutex);
    d.slots.clear();  // models before the library that made them
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
