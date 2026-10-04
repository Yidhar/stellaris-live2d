# stellaris-live2d

[English](README.md) | [简体中文](README.zh-CN.md)

**Stellaris 4.5.1**（Windows x64，`-dx11` 版本）的 Live2D 肖像：一个注入到 `stellaris.exe` 的 DLL，把 Live2D 模型画进游戏自己的肖像框里。布局、遮罩和着色器仍由游戏负责，而且游戏只会绘制屏幕上看得到的肖像。

**状态。** Live2D `moc3` 模型已经画进游戏的肖像（领袖、人口、物种、议政厅、星球界面），有动画（动作、物理、表情、眨眼、呼吸、口型），也能交互（鼠标跟随、点击画面或模型的点击区域、悬停、出现、待机、问候音效），语音跟随游戏音量。这一切由 mod 用肖像脚本语法声明；用 `scripts\deploy.py` 装上加载器之后，游戏会自己加载插件。还没有做：Spine、`pose3.json`、UI 缩放不等于 1 的测试、用 Live2D 官方 Core 的测试（只用过 Purism Core）、多人游戏。只针对 Stellaris 4.5.1 编译和测试。见[计划](#计划)。

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
3. 编译插件（见下），运行 `python scripts\deploy.py`：它把 `stellaris_live2d.dll` 和加载器 `d3dx9_43.dll` 复制到 `stellaris.exe` 旁边，之后游戏启动几秒后会自己加载插件（`deploy.py --remove` 把两者都删掉；`stellaris.exe` 旁边放一个 `stellaris_live2d.disabled` 文件，这一次运行就不加载）。加载器是替身，替的是只有游戏的 exe 才会导入的一个系统 DLL：exe 所在的文件夹先被搜索，所以游戏会用它；它把每个调用转给真正的 `d3dx9_43.dll`，并加载插件（在别的程序里它什么也不做）。没有它时，`python scripts\l2dctl.py load` 把插件注入到正在运行的游戏里，只在这一次运行有效（还有 `unload`、`reload`、`status`）。
4. 编辑 `stellaris.exe` 旁边的 `stellaris_live2d.ini`（首次运行时创建，每 2 秒重新读取），打开有肖像的界面（议会、领袖列表）；没有显示的话看 `stellaris_live2d.log`。

| 键 | 默认值 | 含义 |
|---|---|---|
| `live2d` | `0` | 把模型画进每个可见肖像的渲染目标 |
| `core_dll` | | `Live2DCubismCore.dll` 的路径 |
| `model` | | 模型 `model3.json` 的路径（一个模型，配合 `view_*` 使用） |
| `models` | | 多个模型，用 `;` 分隔：`path`、`path\|x,y,h`（指定裁剪）或 `path\|auto` / `path\|auto:0.5`（按模型自动算裁剪：从头顶往下取人物身高的这个比例，默认 `0.46`）。每个肖像第一次出现时按顺序分到一个模型 |
| `only_width`、`only_height` | `0` | 只处理渲染目标恰好是这个大小的肖像（`0` = 所有大小）；游戏的角色肖像是 575×380 |
| `view_x`、`view_y`、`view_h` | `0.44`、`0.19`、`0.26` | 显示模型画布的哪一部分：中心距左边、中心距上边、高度，都是画布的比例 |
| `model_cache_mb` | `512` | 已加载的模型可以占用的内存：在此之内所有模型都在后台提前加载，超出则按需加载，并丢掉最久没用的 |
| `supersample` | `2` | `2` 以两倍大小绘制再平均缩小（细线更清晰，开销很小）；`1` 为关闭 |
| `fps` | `30` | 模型每秒推进和重绘多少次 |
| `physics` | `1` | 来自模型 `physics3.json` 的次级运动 |
| `interactions` | `1` | mod 声明的事件（鼠标跟随、点击、悬停……）；`0` = 模型只播自己的动作 |
| `audio`、`volume` | `1`、`0.8` | 事件的语音开关，以及插件自己的音量（再乘以游戏的音量） |
| `volume_channel` | `voice` | 语音跟随游戏声音设置里的哪个滑块：`voice`、`effects` 或 `none` |
| `mute_in_background` | `0` | `1` = 游戏窗口不在前台时静音 |
| `extra_mod_dirs` | | 当作已启用来读取的 mod 根目录（开发用） |
| `test_pattern` | `0` | 改为画测试图案（用来检查钩子是否工作） |

### 肖像组 mod

推荐的用法是做一个 mod：mod 带着模型，并声明它们替换哪些肖像。mod 用游戏自己注册肖像的脚本语法写出要交给插件绘制的肖像键，再加几个额外的键（`live2d = yes`、`spine = yes`、`live2d_model`、`live2d_view`、`live2d_scale`、`live2d_unmirror`，以及 `live2d_actions`，用来声明鼠标跟随、点击、悬停、出现、待机和游戏问候音效时发生什么）。插件从已启用 mod 的 `gfx/portraits/live2d/*.txt`（引擎会忽略的文件夹）或 `gfx/portraits/portraits/*.txt` 读取它们，并把每个模型绑定到引擎为某个肖像报告的肖像键上。没有安装插件的游戏照常画普通肖像。领袖、统治者、物种和人口用哪些肖像，由游戏自己的 `portrait_groups` 语法决定：mod 把自己的键列进某个组，就能让每个领袖或人口用不同的模型，不需要额外语法。动作组、表情和点击区域来自模型自己的 `model3.json`，mod 只说在哪个事件播哪个。

- 语法、事件、取景、加载和肖像组的规则：[docs/portrait-mod-design.md](docs/portrait-mod-design.md)。
- **演示 mod** 在单独的仓库 [stellaris-live2d-demo-mod](https://github.com/Yidhar/stellaris-live2d-demo-mod)：把 `human` 肖像组换成十个 Live2D 肖像（每个作用域先写 `set`，再写 `add`），原版肖像键也绑定了模型，每一种事件、语音、取景和缩放选项都用了一次。它不带模型（做它时用的模型是别人的作品），请自备模型。
- 许多模型的 `login` 动作是舞台入场（黑幕、镜头推拉）；`live2d_ignore_parameters` 会跳过你指定的参数的曲线，`python tools/motion_diff.py <model3.json>` 能帮你找出它们（见设计文档的 *Stage effects in motions* 一节）。
- 两条值得知道的规则：在多个文件里定义的肖像组会被*合并*，所以每个作用域的第一条必须是 `set`，才能丢掉原版列出的肖像；脚本或帝国设计器直接按名字指定的肖像（统治者的）不是从组里抽的，所以原版的键也要绑定。
- `python scripts\make_human_mod.py --enable` 用本地模型生成测试 mod（`--export-demo <文件夹>` 写出演示仓库的文本文件）；`python scripts\load_save.py <存档> --folder <文件夹>` 在某个存档上重启游戏。
- ini 里 `interactions=0` 关闭交互。不提供拖拽和滚轮缩放；`live2d_scale` 是对取景部分的固定放大倍率。

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

DLL 只适用于它的 SDK 所定位的那个 `stellaris.exe`（加载时检查；版本不一致时只写日志，不安装任何钩子）。游戏更新之后：`pip install pefile capstone`，运行 `python tools/locate.py`，重新编译，然后检查：

- `python tools/validate.py`：定位器找到的仍是头文件里写的，每个手工或在游戏里验证过的值没有变，布局的不变量成立；没有指纹可找的常量（GUI 对象的布局）如果是为另一个版本验证的，它会说出来。这相当于主仓库 SDK dumper 的 `validate.py`。
- 游戏运行、屏幕上有肖像时运行 `python tools/live_verify.py`：插件自己检查活的对象（肖像键和种类、矩形、窗口和 GUI 大小、游戏音量，以及游戏窗口在前台时的鼠标位置），把结果写进日志，这个脚本把它打印出来。
- `build\Release\l2d_tests.exe`（或在 `build` 里运行 `ctest -C Release`）：脚本读取器、肖像注册表、DDS 读取、mip 链和取景计算的离线检查。

## 计划

1. **已完成：** 钩子、纹理写入、恢复、卸载；Core 加载、模型、动作、物理、渲染器；同时多个模型、DXT5 贴图、肖像组 mod。
2. **已完成：** 鼠标跟随、点击、点击区域、悬停、出现、待机、问候；表情、眨眼、呼吸、口型；跟随游戏音量的语音；按肖像种类和大小的取景；超采样；在内存预算内后台加载。
3. **已完成：** 让游戏自己加载插件的加载器；SDK 检查（`tools/validate.py`、插件自检）和离线测试。
4. mod 检查器（启动游戏之前就发现肖像文件的问题）、Spine、`pose3.json`、由 CI 发布构建。

## 授权

这里的代码是 MIT。仓库里不包含也不下载任何 Live2D 代码或任何模型：

- **Cubism Core** 是 Live2D 的专有库，这里不分发（见上面的第 1 步）。请查看你所用的那个的条款；Live2D 的协议对允许第三方添加内容的应用有特别规定。
- **模型**是作者的作品，各自有自己的许可证。
- `third_party/` 里是单头文件库（miniaudio、stb 系列、`nlohmann/json`），见 `third_party/README.md`；MinHook（BSD-2-Clause）由 CMake 获取。

## 许可证

MIT，见 [LICENSE](LICENSE)。
