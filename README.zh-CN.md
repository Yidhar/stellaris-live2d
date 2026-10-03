# stellaris-live2d

[English](README.md) | [简体中文](README.zh-CN.md)

**Stellaris 4.5.1**（Windows x64，`-dx11` 版本）的 Live2D 肖像：一个注入到 `stellaris.exe` 的 DLL，把渲染好的画面放进游戏自己的肖像框里。布局、遮罩和全息着色器仍由游戏负责，而且游戏只会绘制屏幕上看得到的肖像。

**状态：里程碑 1，概念验证。** 挂钩点和纹理写入已经跑通：议会界面里的肖像被测试图案替换，并且在游戏自己的遮罩之内。目前还没有 Live2D 渲染。

## 原理

Stellaris 的肖像是骨骼动画的 2D 人物，渲染到一张渲染目标纹理里，界面再通过带遮罩的精灵把它显示出来。每一帧，对屏幕上可见的每个肖像，引擎都会调用一次 `CPortraitObject::UpdatePortrait`。插件挂钩这个函数，在原函数返回之后，覆盖该肖像的渲染目标。

- **定位代码。** `tools/locate.py` 用取自引擎自身代码的指纹，找到 `UpdatePortrait` 和所需的对象字段，写出 `sdk/stellaris_sdk.hpp`；指纹不唯一时直接报错，不会猜。没有任何手写的地址。每次游戏更新后重新运行。
- **定位纹理。** 引擎把 D3D11 纹理包在 `TextureGFX` 对象里。插件扫描这个对象，找出一个「大小恰好等于肖像大小、可作渲染目标」的 D3D11 纹理 COM 指针，记住偏移，每次使用都重新校验，然后在该纹理自己的设备上下文上用 `UpdateSubresource` 写入。
- **可见性。** 只更新屏幕上的肖像：关掉议会界面后，钩子一次调用都收不到。
- **清理。** 关闭效果或卸载 DLL 时，会把引擎肖像数组里的每个肖像标记为需要重绘，所以不会有肖像留着旧画面。DLL 自己卸载：摘掉钩子，等进行中的调用结束，再释放自己。
- **多人游戏。** 插件只改变这个客户端画什么，从不触碰模拟，所以不会造成不同步。没有安装插件的玩家看到的是普通肖像。

## 概念验证的结果

`test_pattern=1` 时，每个可见肖像的渲染目标都会被画上图案（每个肖像一种颜色、网格、移动的竖条、左上红色标记、右下蓝色标记）。在游戏里，议会的六个议员框显示出了图案，方向正确，处在游戏自己的遮罩和渐隐之下；关掉图案，或者在图案开着时卸载插件，原来的肖像都会恢复。游戏的肖像渲染目标是 575×380，格式 `B8G8R8A8_UNORM`，同时绑定为渲染目标和着色器资源。

## 安装和使用

先编译（见下），启动游戏，然后运行

```
python scripts\l2dctl.py load        # 注入；还有 unload / reload / status
```

`stellaris_live2d.ini` 会在 `stellaris.exe` 旁边创建，每 2 秒重新读取一次；`stellaris_live2d.log` 里能看到钩子和计数。注入只在这一次游戏运行中有效。

| 键 | 默认值 | 含义 |
|---|---|---|
| `test_pattern` | `0` | 把测试图案画进每个可见肖像的渲染目标 |
| `only_width`、`only_height` | `0` | 只画大小恰好是这个值的渲染目标（`0` = 所有大小） |

`python scripts\capture_game.py out.png` 会保存游戏窗口客户区的截图（窗口必须可见且没有被遮住）。

## 编译

Visual Studio 2022（MSVC，x64）和 CMake 3.20+。MinHook 由 CMake 自动获取。

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release        # build\Release\stellaris_live2d.dll
```

DLL 只适用于它的 SDK 所定位的那个 `stellaris.exe`（加载时检查；版本不一致时只写日志，不安装任何钩子）。游戏更新之后：`pip install pefile capstone`，运行 `python tools/locate.py`，重新编译。

## 计划

1. **已完成：** 钩子、纹理写入、恢复、卸载。
2. 通过 Cubism Core 的 C API 加载 `moc3` 模型（运行时从 `Live2DCubismCore.dll` 加载），播放动作和物理，用自己的 D3D11 设备在 GPU 上绘制，再把画面交给钩子。
3. 按名字把肖像绑定到模型，把全身画布裁成肖像，状态变体（例如受伤）。
4. 一个用于参数、动作和表情的小型控制接口。

## 授权

这里的代码是 MIT。仓库里不包含也不下载任何 Live2D 代码或任何模型：

- **Cubism Core** 是 Live2D 的专有库，这里不分发。计划是在运行时加载 `Live2DCubismCore.dll`，可以来自 Live2D 官方 SDK（个人和小规模企业按 Live2D 的条款可免费使用，条款由使用者自己接受），也可以是兼容的替代品，例如 [Purism Core](https://github.com/SakuraMotion/PurismCore)（MIT）。请查看你所用的那个的条款；Live2D 的协议对允许第三方添加内容的应用有特别规定。
- **模型**是作者的作品，各自有自己的许可证。

## 许可证

MIT，见 [LICENSE](LICENSE)。
