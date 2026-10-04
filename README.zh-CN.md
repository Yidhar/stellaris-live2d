# stellaris-live2d

[English](README.md) | [简体中文](README.zh-CN.md)

**Stellaris 4.5.1**（Windows x64，`-dx11` 版本）的 Live2D 肖像：一个注入到 `stellaris.exe` 的 DLL，把 Live2D 模型画进游戏自己的肖像框里。布局、遮罩和着色器仍由游戏负责，而且游戏只会绘制屏幕上看得到的肖像。

**状态：里程碑 2。** 在真实的游戏里，一个 `moc3` 模型已经能加载、播放动画（待机循环、动作、物理），并画进议会和领袖界面的肖像。还没有做的：按肖像选择模型（目前是一个模型画进所选尺寸的所有肖像）、按界面裁剪、状态变体和控制接口。见[计划](#计划)。

## 原理

Stellaris 的肖像是骨骼动画的 2D 人物，渲染到一张渲染目标纹理里，界面再通过带遮罩的精灵把它显示出来。每一帧，对屏幕上可见的每个肖像，引擎都会调用一次 `CPortraitObject::UpdatePortrait`。插件挂钩这个函数，在原函数返回之后，把该肖像的渲染目标换成模型的一帧。

- **Live2D 这一侧**（`src/cubism_core.cpp`、`live2d_model.cpp`、`live2d_motion.cpp`、`live2d_physics.cpp`、`live2d_character.cpp`、`live2d_renderer.cpp`）完全不了解游戏，编成一个库，由 DLL 和离屏查看器共用。
  - **Cubism Core** 在运行时从你指定的 `Live2DCubismCore.dll` 加载，它的 C API 声明在 `include/cubism_core.hpp` 里。这个仓库里没有任何 Live2D 的东西。
  - **动作**是 `motion3.json` 的曲线（线性、贝塞尔、阶梯、反阶梯），按 Cubism 的方式施加：先恢复上一帧保存的参数，每个活动中的动作按淡入淡出权重混入，保存结果，然后再叠加不能累积的效果（物理）。`Idle` 组循环播放，其他组只播一次。
  - **物理**（`physics3.json`）是根据文件字段的含义独立写的实现，没有和 Live2D 自己的运行时对比过，所以头发和衣物的运动是同一类，但不一定完全一样。
  - **渲染器**用 Direct3D 11：预乘 alpha、加法和乘法 drawable、裁剪遮罩、模型的乘色和屏幕色、带 mip 的贴图。
- **画进游戏。** 渲染器在游戏自己的设备上录制到一个*延迟上下文*，再用 `ExecuteCommandList(…, RestoreContextState = TRUE)` 执行，所以引擎的管线状态会被原样恢复。模型每秒最多推进和绘制 `fps` 次，每种肖像尺寸画一张纹理，再复制到各个肖像的渲染目标里。
- **定位代码。** `tools/locate.py` 用取自引擎自身代码的指纹，找到 `UpdatePortrait`、它需要的对象字段和引擎里所有肖像的数组，写出 `sdk/stellaris_sdk.hpp`；指纹不唯一时直接报错，不会猜。没有任何手写的地址。每次游戏更新后重新运行。
- **定位纹理。** 引擎把 D3D11 纹理包在 `TextureGFX` 对象里。插件扫描它，找出一个「大小恰好等于肖像大小、可作渲染目标」的 D3D11 纹理 COM 指针，记住偏移，每次使用都重新校验。
- **清理。** 关闭效果或卸载 DLL 时，会把引擎肖像数组里的每个肖像标记为需要重绘，所以不会有肖像留着旧画面。DLL 自己卸载：摘掉钩子，等进行中的调用结束，释放 GPU 对象和 Cubism Core，再释放自己。
- **多人游戏。** 插件只改变这个客户端画什么，从不触碰模拟，所以不会造成不同步。没有安装插件的玩家看到的是普通肖像。

## 安装和使用

你需要一个 Cubism Core 库和一个模型，这两样都不包含在仓库里。

1. **Cubism Core。** 可以是 Live2D 官方的 `Live2DCubismCore.dll`（来自 live2d.com 的 Cubism SDK for Native，遵守 Live2D 的条款），也可以是兼容的替代品，例如 [Purism Core](https://github.com/SakuraMotion/PurismCore)（MIT）。插件使用 v5 版的 API（`csmGetDrawableRenderOrders` 和颜色相关的函数）；只有新版 API 的库暂不支持。
2. **模型**：一个文件夹，里面有 `model3.json`、`moc3`、贴图（PNG、JPEG，或 DXT1/DXT3/DXT5 的 DDS，即游戏自己贴图的格式：显存占用是 RGBA8 的四分之一，mip 链存在文件里），可选的 `physics3.json` 和动作。`l2d_pack` 可以把模型的 PNG 贴图转成 DXT5。模型有它们作者的许可证。
3. 编译插件（见下），启动游戏，运行 `python scripts\l2dctl.py load`（还有 `unload`、`reload`、`status`）。注入只在这一次游戏运行中有效。
4. 编辑 `stellaris.exe` 旁边的 `stellaris_live2d.ini`（首次运行时创建，每 2 秒重新读取），打开有肖像的界面（议会、领袖列表）；没有显示的话看 `stellaris_live2d.log`。

| 键 | 默认值 | 含义 |
|---|---|---|
| `live2d` | `0` | 把模型画进每个可见肖像的渲染目标 |
| `core_dll` | | `Live2DCubismCore.dll` 的路径 |
| `model` | | 模型 `model3.json` 的路径（一个模型，配合 `view_*` 使用） |
| `models` | | 多个模型，用 `;` 分隔：`path`、`path\|x,y,h`（指定裁剪）或 `path\|auto` / `path\|auto:0.5`（按模型自动算裁剪：从头顶往下取人物身高的这个比例，默认 `0.46`）。每个肖像第一次出现时按顺序分到一个模型 |
| `only_width`、`only_height` | `0` | 只处理渲染目标恰好是这个大小的肖像（`0` = 所有大小）；游戏的角色肖像是 575×380 |
| `view_x`、`view_y`、`view_h` | `0.44`、`0.19`、`0.26` | 显示模型画布的哪一部分：中心距左边、中心距上边、高度，都是画布的比例 |
| `fps` | `30` | 模型每秒推进和重绘多少次 |
| `physics` | `1` | 来自模型 `physics3.json` 的次级运动 |
| `test_pattern` | `0` | 改为画测试图案（用来检查钩子是否工作） |

### 肖像组 mod

推荐的用法是做一个 mod：mod 带着模型，并声明它们替换哪些肖像。mod 用游戏自己注册肖像的脚本语法写出要交给插件绘制的肖像键，再加几个额外的键（`live2d = yes`、`spine = yes`、`live2d_model`、`live2d_view`，以及描述鼠标跟随、点击、拖拽、缩放的 `live2d_actions`）。插件从已启用 mod 的 `gfx/portraits/live2d/*.txt`（引擎不读的目录）或 `gfx/portraits/portraits/*.txt` 读取，把每个模型绑定到引擎报告的肖像键上。没有插件的游戏照常画原来的肖像。详见 [docs/portrait-mod-design.md](docs/portrait-mod-design.md)。`python scripts\make_human_mod.py --enable` 会用 `models_dxt5\` 里的模型生成一个替换人类肖像的测试 mod；`python scripts\load_save.py <存档> --folder <目录>` 让游戏读入存档并注入插件。`mouse_follow`（头和眼睛跟着鼠标，以肖像自己在屏幕上的位置为准）、`click`（触摸动作）和 `live2d_unmirror`（被界面镜像的肖像先反着画，文字读起来正常）已可用；ini 里 `interactions=0` 可关闭交互。`drag`、`scale` 目前只解析，尚未实现。

### 离屏查看器

`l2d_view.exe` 不需要游戏，把一帧画成 PNG，用来检查模型和选择 `view_*`：

```
build\Release\l2d_view.exe --core Live2DCubismCore.dll --model model3.json --out out.png ^
    --size 575x380 --view 0.44,0.19,0.26 [--motion touch_1 --time 2.0] [--no-physics] [--param ParamAngleX=20]
```

`--view auto`（或 `auto:0.5`）会打印插件为该模型自动算出的裁剪。`python scripts\capture_game.py out.png` 会保存游戏窗口客户区的截图（窗口必须可见且没有被遮住）。

### 同时画很多模型的开销

`l2d_bench.exe` 按插件的方式（推进、在延迟上下文里录制、执行）每步画 N 个不同的模型，报告各阶段的 CPU 时间和用时间戳查询得到的 GPU 时间：

```
build\Release\l2d_bench.exe --core Live2DCubismCore.dll --size 575x380 --frames 300 --counts 1,6,12,24 ^
    --model a\model3.json^|auto --model b\model3.json^|auto [--no-physics]
```

`python scripts\ingame_multi_bench.py` 在真实游戏里跑几组模型（要先打开一个有肖像的界面，并加载 stellaris-perf 仓库的 `stellaris_bench.dll` 作为帧计数器），和关闭插件时的每秒帧数对比。

## 编译

Visual Studio 2022（MSVC，x64）和 CMake 3.20+。MinHook 由 CMake 自动获取；`stb_image` 和 `nlohmann/json` 是 `third_party/` 里的单头文件。

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release        # build\Release\stellaris_live2d.dll 和 l2d_view.exe
```

DLL 只适用于它的 SDK 所定位的那个 `stellaris.exe`（加载时检查；版本不一致时只写日志，不安装任何钩子）。游戏更新之后：`pip install pefile capstone`，运行 `python tools/locate.py`，重新编译。

## 计划

1. **已完成：** 钩子、纹理写入、恢复、卸载。
2. **已完成：** Core 加载、模型、动作、物理、渲染器、画进游戏。
3. **已完成：** 同时多个模型、DXT5 贴图、按肖像键绑定模型的肖像组 mod。
4. **已完成：** 鼠标跟随。接下来：点击、拖拽、缩放、Spine、按领袖或按界面绑定。
5. 状态变体（例如受伤）。
6. 由 CI 发布构建。

## 授权

这里的代码是 MIT。仓库里不包含也不下载任何 Live2D 代码或任何模型：

- **Cubism Core** 是 Live2D 的专有库，这里不分发（见上面的第 1 步）。请查看你所用的那个的条款；Live2D 的协议对允许第三方添加内容的应用有特别规定。
- **模型**是作者的作品，各自有自己的许可证。
- `third_party/` 里有 `stb_image`（公有领域）和 `nlohmann/json`（MIT），见 `third_party/README.md`。

## 许可证

MIT，见 [LICENSE](LICENSE)。
