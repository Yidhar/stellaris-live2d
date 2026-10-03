#include "portrait_live2d.hpp"

#include "live2d_character.hpp"
#include "live2d_renderer.hpp"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
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
    uint64_t serial = 0;
};

} // namespace

struct Live2DPainter::Impl {
    // --- shared between the worker and the render thread, under `mutex`
    std::mutex mutex;
    std::shared_ptr<core::Api> api;
    std::shared_ptr<Character> character;
    View view;
    int fps = 30;
    std::string loaded_core, loaded_model;

    // --- render thread only
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> deferred;
    std::unique_ptr<Renderer> renderer;
    Renderer::GpuPtr gpu;
    std::shared_ptr<Character> gpu_for;
    std::map<uint64_t, Frame> frames;  // by output size and format
    uint64_t last_call = 0, serial = 0;
    double pending = 0.0;  // seconds since the last step that have not been stepped yet
    bool reported_failure = false;

    // --- statistics
    std::atomic<uint64_t> ticks{ 0 }, draws{ 0 }, copies{ 0 }, failures{ 0 };
    std::atomic<uint64_t> tick_ticks{ 0 }, draw_ticks{ 0 }, copy_ticks{ 0 };

    void ResetGpu() {
        frames.clear();
        gpu.reset();
        gpu_for.reset();
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

void Live2DPainter::Configure(const Settings& s) {
    Impl& d = *impl_;
    if (!s.live2d || s.core_dll.empty() || s.model.empty()) {
        std::lock_guard<std::mutex> lock(d.mutex);
        if (d.character) Log("live2d: model unloaded");
        d.character.reset();
        d.loaded_model.clear();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        d.view = { s.view_x, s.view_y, s.view_h };
        d.fps = s.fps < 1 ? 1 : s.fps > 120 ? 120 : s.fps;
        if (d.character) d.character->set_physics_enabled(s.physics);
        if (d.character && d.loaded_core == s.core_dll && d.loaded_model == s.model) return;
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
    const uint64_t t0 = Ticks();
    auto character = std::make_shared<Character>();
    if (!character->Load(api.get(), s.model, &err)) {
        Log("live2d: cannot load %s: %s", s.model.c_str(), err.c_str());
        return;
    }
    character->set_physics_enabled(s.physics);
    if (!character->physics_error().empty()) Log("live2d: physics not loaded: %s", character->physics_error().c_str());
    const Model& m = character->model();
    Log("live2d: loaded %s in %.0f ms: canvas %.0fx%.0f, %d parameters, %d drawables, %zu textures, %zu motion groups",
        s.model.c_str(), (Ticks() - t0) * 1000.0 / TickFrequency(), m.canvas_size.x, m.canvas_size.y, m.parameter_count,
        m.drawable_count, m.textures.size(), m.motions.size());
    std::lock_guard<std::mutex> lock(d.mutex);
    d.character = character;  // the previous character (if any) lives on until the render thread drops its copy
    d.api = api;
    d.loaded_core = s.core_dll;
    d.loaded_model = s.model;
}

bool Live2DPainter::Ready() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->character != nullptr;
}

bool Live2DPainter::Paint(ID3D11Texture2D* target, const D3D11_TEXTURE2D_DESC& desc) {
    Impl& d = *impl_;
    std::shared_ptr<Character> character;
    View view;
    int fps;
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        character = d.character;
        view = d.view;
        fps = d.fps;
    }
    if (!character) return false;

    ID3D11Device* dev = nullptr;
    target->GetDevice(&dev);
    if (!dev) return false;
    ComPtr<ID3D11Device> device_guard;
    device_guard.Attach(dev);
    if (dev != d.device.Get()) {
        d.ResetGpu();
        d.device = dev;
        d.renderer.reset(new Renderer);
        std::string err;
        if (!d.renderer->Init(dev, &err)) {
            if (!d.reported_failure) Log("live2d: renderer: %s", err.c_str());
            d.reported_failure = true;
            d.renderer.reset();
            d.device.Reset();
            ++d.failures;
            return false;
        }
        if (FAILED(dev->CreateDeferredContext(0, &d.deferred))) {
            Log("live2d: the game's device cannot make a deferred context");
            d.ResetGpu();
            ++d.failures;
            return false;
        }
    }
    if (d.gpu_for != character) {
        std::string err;
        d.frames.clear();
        d.gpu = d.renderer->CreateModel(character->model(), &err);
        if (!d.gpu) {
            if (!d.reported_failure) Log("live2d: GPU resources: %s", err.c_str());
            d.reported_failure = true;
            d.gpu_for.reset();
            ++d.failures;
            return false;
        }
        d.gpu_for = character;
        Log("live2d: GPU resources created");
    }

    // Advance the model in fixed steps of 1/fps. Time since the last call piles up in `pending` and a step is taken when a
    // whole one has piled up, so the rate is fps on average even when the game's frames do not divide evenly. At most one
    // step per call, and the pile is capped so a pause does not turn into a burst. Every size redraws once per step.
    const uint64_t now = Ticks();
    const double step = 1.0 / fps;
    if (d.last_call) d.pending += (now - d.last_call) / TickFrequency();
    d.last_call = now;
    if (d.serial == 0 || d.pending >= step) {
        d.pending = d.serial == 0 ? 0.0 : std::min(d.pending - step, step * 2);
        const uint64_t t0 = Ticks();
        character->Tick((float)step);
        d.tick_ticks += Ticks() - t0;
        ++d.serial;
        ++d.ticks;
    }

    const uint64_t key = ((uint64_t)desc.Width << 40) | ((uint64_t)desc.Height << 16) | (uint64_t)desc.Format;
    Frame& f = d.frames[key];
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
            d.frames.erase(key);
            ++d.failures;
            return false;
        }
    }

    ComPtr<ID3D11DeviceContext> immediate;
    dev->GetImmediateContext(&immediate);
    if (f.serial != d.serial) {
        const uint64_t t0 = Ticks();
        d.renderer->Draw(d.deferred.Get(), *d.gpu, character->model(), f.rtv.Get(), desc.Width, desc.Height, view);
        ComPtr<ID3D11CommandList> list;
        if (FAILED(d.deferred->FinishCommandList(FALSE, &list))) {
            ++d.failures;
            return false;
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
    return true;
}

void Live2DPainter::Shutdown() {
    Impl& d = *impl_;
    d.ResetGpu();
    std::lock_guard<std::mutex> lock(d.mutex);
    d.character.reset();  // models before the library that made them
    d.api.reset();
    d.loaded_core.clear();
    d.loaded_model.clear();
}

std::string Live2DPainter::Stats() const {
    const Impl& d = *impl_;
    const double ms = 1000.0 / TickFrequency();
    char buf[300];
    const double ticks = (double)d.ticks.load(), draws = (double)d.draws.load(), copies = (double)d.copies.load();
    snprintf(buf, sizeof buf, "live2d: advanced %llu (avg %.3f ms), drawn %llu (avg %.3f ms on the render thread), copied %llu (avg %.3f ms), failures %llu",
             (unsigned long long)d.ticks.load(), ticks ? d.tick_ticks.load() * ms / ticks : 0.0, (unsigned long long)d.draws.load(),
             draws ? d.draw_ticks.load() * ms / draws : 0.0, (unsigned long long)d.copies.load(),
             copies ? d.copy_ticks.load() * ms / copies : 0.0, (unsigned long long)d.failures.load());
    return buf;
}

} // namespace l2d
