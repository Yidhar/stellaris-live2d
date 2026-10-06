// l2d_bench: how much do N different Live2D models cost per step, drawn at once? Offscreen, on a real GPU, the way the
// plugin draws them in the game: each model is advanced (Core, motion, physics), recorded on a deferred context, and run on
// the immediate context. Reports CPU time per stage and GPU time (timestamp queries) per step, over many steps.
//
//   l2d_bench --core Live2DCubismCore.dll --size 575x380 --frames 300 --counts 1,2,4,6,8,12,16
//             --model path[|x,y,h|auto] [--model ...] [--no-physics]
#include "live2d_character.hpp"
#include "utf8_path.hpp"
#include "live2d_renderer.hpp"

#include <d3d11.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

double Seconds() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

struct Item {
    std::string name;
    l2d::Character character;
    l2d::View view;
    l2d::Renderer::GpuPtr gpu;
    ComPtr<ID3D11Texture2D> rt;
    ComPtr<ID3D11RenderTargetView> rtv;
    size_t texture_bytes = 0;
    int drawables = 0, vertices = 0;
};

struct Stats {
    std::vector<double> v;
    void add(double x) { v.push_back(x); }
    double mean() const { double s = 0; for (double x : v) s += x; return v.empty() ? 0 : s / v.size(); }
    double pct(double p) const {
        if (v.empty()) return 0;
        std::vector<double> c = v;
        std::sort(c.begin(), c.end());
        return c[(size_t)(p * (c.size() - 1))];
    }
};

} // namespace

int main(int argc, char** argv) {
    std::string core_path;
    UINT width = 575, height = 380;
    int frames = 300;
    bool physics = true;
    int supersample = 1;
    std::vector<int> counts = { 1, 2, 4, 6, 8, 12, 16 };
    std::vector<std::string> model_args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--core") core_path = next();
        else if (a == "--size") sscanf(next().c_str(), "%ux%u", &width, &height);
        else if (a == "--frames") frames = atoi(next().c_str());
        else if (a == "--no-physics") physics = false;
        else if (a == "--supersample") supersample = atoi(next().c_str());
        else if (a == "--model") model_args.push_back(next());
        else if (a == "--counts") {
            counts.clear();
            const std::string c = next();
            for (size_t p = 0; p < c.size();) { counts.push_back(atoi(c.c_str() + p)); p = c.find(',', p); if (p == std::string::npos) break; ++p; }
        }
    }
    if (core_path.empty() || model_args.empty()) {
        fprintf(stderr, "usage: l2d_bench --core <dll> --model path[|x,y,h|auto] [--model ...] [--size WxH] [--frames N] [--counts 1,2,4] [--no-physics]\n");
        return 2;
    }

    std::string err;
    l2d::core::Api api;
    if (!api.Load(std::wstring(core_path.begin(), core_path.end()), &err)) { fprintf(stderr, "core: %s\n", err.c_str()); return 1; }

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx))) {
        fprintf(stderr, "no D3D11 device\n");
        return 1;
    }
    {
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC ad = {};
        if (SUCCEEDED(dev.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&ad)))
            printf("GPU: %ls, output %ux%u, %d frames per count, physics %s\n", ad.Description, width, height, frames, physics ? "on" : "off");
    }
    l2d::Renderer renderer;
    if (!renderer.Init(dev.Get(), &err)) { fprintf(stderr, "renderer: %s\n", err.c_str()); return 1; }
    renderer.SetSupersample(supersample);
    ComPtr<ID3D11DeviceContext> deferred;
    dev->CreateDeferredContext(0, &deferred);

    // load the models
    std::vector<std::unique_ptr<Item>> items;
    size_t total_texture = 0;
    for (const std::string& arg : model_args) {
        auto it = std::make_unique<Item>();
        const size_t bar = arg.find('|');
        const std::string path = arg.substr(0, bar);
        it->name = l2d::U8(std::filesystem::path(path).parent_path().filename());
        if (!it->character.Load(&api, path, &err)) { fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str()); return 1; }
        it->character.set_physics_enabled(physics);
        l2d::Model& m = it->character.model();
        it->view = { 0.44f, 0.19f, 0.26f };
        if (bar != std::string::npos) {
            const std::string v = arg.substr(bar + 1);
            if (v.rfind("auto", 0) == 0) m.SuggestPortraitView(&it->view.center_x, &it->view.center_y, &it->view.height, v.size() > 5 && v[4] == ':' ? (float)atof(v.c_str() + 5) : 0.46f);
            else sscanf(v.c_str(), "%f,%f,%f", &it->view.center_x, &it->view.center_y, &it->view.height);
        }
        it->gpu = renderer.CreateModel(m, &err);
        if (!it->gpu) { fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str()); return 1; }
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = width; td.Height = height; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        dev->CreateTexture2D(&td, nullptr, &it->rt);
        dev->CreateRenderTargetView(it->rt.Get(), nullptr, &it->rtv);
        for (const l2d::Image& img : m.textures)
            for (size_t l = 0; l < img.mips.size(); ++l) it->texture_bytes += img.mips[l].size();
        total_texture += it->texture_bytes;
        it->drawables = m.drawable_count;
        const int* vc = api.GetDrawableVertexCounts(m.handle());
        for (int i = 0; i < m.drawable_count; ++i) it->vertices += vc[i];
        // warm up: a few seconds of motion so physics and idle are in a normal state
        for (int i = 0; i < 60; ++i) it->character.Tick(1.0f / 30.0f);
        printf("  %-14s %4d drawables %6d vertices, %zu texture(s) %5.1f MB with mips, view (%.2f, %.2f, %.2f)\n", it->name.c_str(), it->drawables,
               it->vertices, m.textures.size(), it->texture_bytes / 1048576.0, it->view.center_x, it->view.center_y, it->view.height);
        items.push_back(std::move(it));
    }
    printf("  all %zu models: %.1f MB of textures\n\n", items.size(), total_texture / 1048576.0);

    // timestamp queries: a ring, read a few frames late so the CPU never waits for the GPU
    struct Q { ComPtr<ID3D11Query> disjoint, begin, end; bool pending = false; };
    std::vector<Q> ring(6);
    for (Q& q : ring) {
        D3D11_QUERY_DESC d = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
        dev->CreateQuery(&d, &q.disjoint);
        d.Query = D3D11_QUERY_TIMESTAMP;
        dev->CreateQuery(&d, &q.begin);
        dev->CreateQuery(&d, &q.end);
    }

    printf("%7s | %-26s | %-26s | %-21s | %s\n", "models", "CPU per step: total ms", "  advance (Core+motion+phys)", "GPU per step: ms", "per model: CPU ms, GPU ms");
    printf("%7s | %-26s | %-26s | %-21s |\n", "", "mean / p99", "  mean ms", "mean / p99", "");
    for (int n : counts) {
        Stats cpu, advance, record, execute, gpu;
        auto collect = [&](Q& q) {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
            UINT64 t0 = 0, t1 = 0;
            while (ctx->GetData(q.disjoint.Get(), &dj, sizeof dj, 0) != S_OK) Sleep(0);
            while (ctx->GetData(q.begin.Get(), &t0, sizeof t0, 0) != S_OK) Sleep(0);
            while (ctx->GetData(q.end.Get(), &t1, sizeof t1, 0) != S_OK) Sleep(0);
            if (!dj.Disjoint && t1 >= t0) gpu.add((double)(t1 - t0) * 1000.0 / (double)dj.Frequency);
            q.pending = false;
        };
        const int warmup = 30;
        for (int f = 0; f < frames + warmup; ++f) {
            Q& q = ring[f % ring.size()];
            if (q.pending) collect(q);
            const double t_start = Seconds();
            double t_adv = 0, t_rec = 0, t_exe = 0;
            std::vector<ComPtr<ID3D11CommandList>> lists(n);
            for (int i = 0; i < n; ++i) {
                Item& it = *items[i % items.size()];
                double a = Seconds();
                it.character.Tick(1.0f / 30.0f);
                double b = Seconds();
                renderer.Draw(deferred.Get(), *it.gpu, it.character.model(), it.rtv.Get(), width, height, it.view);
                deferred->FinishCommandList(FALSE, &lists[i]);
                double c = Seconds();
                t_adv += b - a;
                t_rec += c - b;
            }
            ctx->Begin(q.disjoint.Get());
            ctx->End(q.begin.Get());
            for (int i = 0; i < n; ++i) {
                double a = Seconds();
                ctx->ExecuteCommandList(lists[i].Get(), TRUE);
                t_exe += Seconds() - a;
            }
            ctx->End(q.end.Get());
            ctx->End(q.disjoint.Get());
            q.pending = true;
            const double t_end = Seconds();
            if (f >= warmup) {
                cpu.add((t_end - t_start) * 1000.0);
                advance.add(t_adv * 1000.0);
                record.add(t_rec * 1000.0);
                execute.add(t_exe * 1000.0);
            }
            ctx->Flush();
        }
        for (Q& q : ring) if (q.pending) collect(q);
        // drop the warm-up frames from the GPU series (collected in order)
        if (gpu.v.size() > (size_t)frames) gpu.v.erase(gpu.v.begin(), gpu.v.begin() + (gpu.v.size() - frames));
        printf("%7d | %7.3f / %7.3f        | %7.3f                    | %6.3f / %6.3f       | %6.3f, %6.3f   (record %.3f, execute %.3f)\n", n,
               cpu.mean(), cpu.pct(0.99), advance.mean(), gpu.mean(), gpu.pct(0.99), cpu.mean() / n, gpu.mean() / n, record.mean(), execute.mean());
    }
    printf("\nAt 30 steps a second, a step's milliseconds x 30 / 10 is the percentage of one core (CPU) or of the GPU's time.\n");
    return 0;
}
