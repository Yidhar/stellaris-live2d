// l2d_view: draws one frame of a Live2D model offscreen and writes it to a PNG, to check the loader and the renderer
// without the game. The frame is premultiplied alpha; the PNG is composited over a grey background.
//
//   l2d_view --core <Live2DCubismCore.dll> --model <model3.json> --out out.png
//            [--size 575x380] [--view cx,cy,h] [--param ID=value ...] [--time seconds --motion GROUP]
#include "live2d_character.hpp"
#include "live2d_renderer.hpp"

#include <d3d11.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <wrl/client.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

using Microsoft::WRL::ComPtr;

int main(int argc, char** argv) {
    std::string core_path, model_path, out_path = "l2d_view.png";
    UINT width = 575, height = 380;
    l2d::View view;
    std::vector<std::pair<std::string, float>> overrides;
    std::string motion_group;
    float play_time = 0.0f;
    bool physics = true;
    bool auto_view = false;
    float auto_body = 0.46f;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--core") core_path = next();
        else if (a == "--model") model_path = next();
        else if (a == "--out") out_path = next();
        else if (a == "--size") sscanf(next().c_str(), "%ux%u", &width, &height);
        else if (a == "--view") {
            const std::string v = next();
            if (v.rfind("auto", 0) == 0) { auto_view = true; if (v.size() > 5 && v[4] == ':') auto_body = (float)atof(v.c_str() + 5); }
            else sscanf(v.c_str(), "%f,%f,%f", &view.center_x, &view.center_y, &view.height);
        }
        else if (a == "--no-physics") physics = false;
        else if (a == "--motion") motion_group = next();
        else if (a == "--time") play_time = (float)atof(next().c_str());
        else if (a == "--param") {
            const std::string kv = next();
            const size_t eq = kv.find('=');
            if (eq != std::string::npos) overrides.push_back({ kv.substr(0, eq), (float)atof(kv.c_str() + eq + 1) });
        }
    }
    if (core_path.empty() || model_path.empty()) {
        fprintf(stderr, "usage: l2d_view --core <Live2DCubismCore.dll> --model <model3.json> --out out.png [--size WxH] [--view cx,cy,h] [--param ID=v] [--motion GROUP --time SECONDS]\n");
        return 2;
    }

    std::string err;
    l2d::core::Api api;
    if (!api.Load(std::wstring(core_path.begin(), core_path.end()), &err)) { fprintf(stderr, "core: %s\n", err.c_str()); return 1; }
    l2d::Character character;
    if (!character.Load(&api, model_path, &err)) { fprintf(stderr, "model: %s\n", err.c_str()); return 1; }
    character.set_physics_enabled(physics);
    l2d::Model& model = character.model();
    printf("core 0x%08X; model: canvas %.0fx%.0f, %d parameters, %d parts, %d drawables, %zu textures, %zu motion groups\n",
           api.GetVersion(), model.canvas_size.x, model.canvas_size.y, model.parameter_count, model.part_count,
           model.drawable_count, model.textures.size(), model.motions.size());
    for (const auto& [id, v] : overrides) {
        const int p = model.FindParameter(id);
        if (p < 0) { fprintf(stderr, "no parameter %s\n", id.c_str()); return 1; }
        model.parameter_values[p] = v;
    }
    if (play_time > 0.0f) {
        // run the character at 30 frames per second up to the requested time
        if (!motion_group.empty() && !character.PlayMotion(motion_group, 0)) {
            fprintf(stderr, "no motion group %s\n", motion_group.c_str());
            return 1;
        }
        for (float t = 0.0f; t < play_time; t += 1.0f / 30.0f) character.Tick(1.0f / 30.0f);
    } else {
        model.Update();
    }
    if (auto_view) {
        model.SuggestPortraitView(&view.center_x, &view.center_y, &view.height, auto_body);
        printf("auto view: %.3f,%.3f,%.3f\n", view.center_x, view.center_y, view.height);
    }

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx))) {
        fprintf(stderr, "no D3D11 device\n");
        return 1;
    }
    l2d::Renderer renderer;
    if (!renderer.Init(dev.Get(), &err)) { fprintf(stderr, "renderer: %s\n", err.c_str()); return 1; }
    auto gpu = renderer.CreateModel(model, &err);
    if (!gpu) { fprintf(stderr, "gpu: %s\n", err.c_str()); return 1; }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width; td.Height = height; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> rt;
    ComPtr<ID3D11RenderTargetView> rtv;
    dev->CreateTexture2D(&td, nullptr, &rt);
    dev->CreateRenderTargetView(rt.Get(), nullptr, &rtv);

    renderer.Draw(ctx.Get(), *gpu, model, rtv.Get(), width, height, view);

    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    dev->CreateTexture2D(&td, nullptr, &staging);
    ctx->CopyResource(staging.Get(), rt.Get());
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) { fprintf(stderr, "readback failed\n"); return 1; }
    std::vector<uint8_t> png((size_t)width * height * 3);
    for (UINT y = 0; y < height; ++y) {
        const uint8_t* row = (const uint8_t*)map.pData + (size_t)y * map.RowPitch;
        for (UINT x = 0; x < width; ++x) {
            const float a = row[x * 4 + 3] / 255.0f;
            const float bg[3] = { 46, 51, 61 };
            png[((size_t)y * width + x) * 3 + 0] = (uint8_t)std::min(255.0f, row[x * 4 + 2] + bg[0] * (1 - a));
            png[((size_t)y * width + x) * 3 + 1] = (uint8_t)std::min(255.0f, row[x * 4 + 1] + bg[1] * (1 - a));
            png[((size_t)y * width + x) * 3 + 2] = (uint8_t)std::min(255.0f, row[x * 4 + 0] + bg[2] * (1 - a));
        }
    }
    ctx->Unmap(staging.Get(), 0);
    stbi_write_png(out_path.c_str(), (int)width, (int)height, 3, png.data(), (int)width * 3);
    printf("wrote %s (%ux%u)\n", out_path.c_str(), width, height);
    return 0;
}
