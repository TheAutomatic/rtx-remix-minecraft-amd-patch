# RTX Remix for Minecraft - AMD Radeon Compatibility Patch

[English](#english) | [中文说明](#chinese)

> [!IMPORTANT]
> **源码公开 · 完全免费 · 严禁商业倒卖及收费代安装**（如付费购买请立即退款），详见[底部许可协议](#license-zh)。
>
> **Source-Available · Completely Free · No Commercial Resale or Paid Installation** (demand a refund if you paid for this). See [License & Terms](#license-en) below.

---

<a name="english"></a>
## English

This repository provides the native compatibility patch and bridge layer that enables **NVIDIA RTX Remix for Minecraft (Java Edition 1.21.1)** to run on **AMD Radeon graphics cards** (tested on AMD Radeon RX 9070 XT, RDNA 4 / gfx1201).

### Key Problems Solved

1. **WGL / Vulkan Window Swapchain Collision (`VK_ERROR_NATIVE_WINDOW_IN_USE_KHR`)**:
   - Minecraft Java Edition uses GLFW with WGL/OpenGL on its main window.
   - AMD's Vulkan ICD (`amdvlk64.dll`) strictly enforces Vulkan specification Section 34.1: creating a `VkSwapchainKHR` on an HWND with an active OpenGL/WGL Device Context fails with `VK_ERROR_NATIVE_WINDOW_IN_USE_KHR`.
   - **Solution**: The patched `remixbridge.dll` dynamically creates an independent, clean Win32 window (or child window `WS_CHILD | WS_CLIPSIBLINGS`) dedicated to Vulkan presentation, bypassing WGL DC collision entirely.

2. **Win32 Input & Focus Forwarding**:
   - Running path-traced rendering on a clean surface would normally disconnect mouse capture and keyboard events from Minecraft's GLFW window.
   - **Solution**: Implemented bidirectional message hooking and forwarding (`WM_MOUSEACTIVATE -> MA_NOACTIVATE`, raw mouse delta propagation, and focus synchronization), ensuring seamless 1st-person gameplay control.

3. **Bridge-Assisted Injection & OptiScaler Integration**:
   - When running on AMD GPUs, DXVK-Remix's internal NGX wrapper fails with `0xbad00001` before calling `LoadLibrary("nvngx.dll")`, preventing standard proxy injection.
   - **Solution**: The custom bridge explicitly pre-loads `vulkan-1.dll` and `remix-sdk/OptiScaler.dll` prior to loading `d3d9.dll`. OptiScaler intercepts subsequent NGX calls and routes Vulkan Super Resolution to FSR 2.2+.

4. **NVX Vulkan Extension Spoofing (`m_dlssSupported` Gatekeeper)**:
   - RTX Remix's runtime (`rtx_context.cpp`) enforces a hardcoded check:
     ```cpp
     m_dlssSupported = (m_device->extensions().nvxBinaryImport &&
                        m_device->extensions().nvxImageViewHandle &&
                        m_device->extensions().khrPushDescriptor);
     ```
   - On AMD, these NVIDIA experimental extensions do not exist natively. The bundled `config/OptiScaler.ini` enables `VulkanExtensionSpoofing = true`, exposing the required extensions to Remix while safely filtering them out before physical device creation on AMD drivers.

---

### Quick Start for Players

1. Install Minecraft Java Edition 1.21.1 and the official **RTX Remix for Minecraft** mod following the [official guide](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft).
2. Download the latest release from the [Releases](https://github.com/TheAutomatic/rtx-remix-minecraft-amd-patch/releases) tab.
3. Copy `remixbridge.dll` into your game instance's `remix-sdk/` directory (e.g., `<instance>/.minecraft/remix-sdk/`), replacing the existing `remixbridge.dll`.
4. Place `OptiScaler.dll` and `OptiScaler.ini` into the same `remix-sdk/` directory.
5. In `OptiScaler.ini`, under the `[Spoofing]` section, ensure `VulkanExtensionSpoofing = true`:
   ```ini
   [Spoofing]
   VulkanExtensionSpoofing = true
   ```
   *(Required: Exposes the necessary Vulkan extensions so RTX Remix enables the DLSS option in the Alt+X menu)*.
6. Launch the game from PrismLauncher or the Minecraft Launcher and enjoy path tracing on AMD!

---

### Building from Source

**Prerequisites:**
- Windows 10/11 x64
- Visual Studio 2022 or Visual Studio Build Tools with C++ toolset (MSVC 14.3x or 14.4x)

**Build with MSVC Batch Script:**
```cmd
build.cmd
```
The compiled output will be generated at `bin/remixbridge.dll`.

**Build with CMake:**
```cmd
mkdir build && cd build
cmake .. -A x64
cmake --build . --config Release
```

---

<a name="license-en"></a>
### License, Anti-Resale Terms & Acknowledgements

- **Project License**: **PolyForm Noncommercial License 1.0.0** ([LICENSE](LICENSE), [NOTICE](NOTICE)).
  - Source code is publicly available and **completely free** for personal, hobbyist, research, and noncommercial use.
  - **Commercial resale prohibited**: Selling this software or its compiled binaries on any marketplace, bundling it in paid modpacks or paywalled groups/downloads, or providing it as part of paid "remote installation / setup / tuning" services is strictly prohibited. If you paid for this tool, request a refund immediately.
- **Upstream & Third-Party Attributions**:
  - Original RTX Remix JNI bridge code is derived from [NVIDIAGameWorks/rtx-remix-minecraft](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft) under **Apache License 2.0** ([THIRD_PARTY_LICENSES/NVIDIA-rtx-remix-minecraft-Apache-2.0.txt](THIRD_PARTY_LICENSES/NVIDIA-rtx-remix-minecraft-Apache-2.0.txt) and [NOTICE](NOTICE)).
  - `include/remix_c.h` is licensed under the MIT License; OpenJDK JNI headers (`include/jni.h`, `include/win32/jni_md.h`) are licensed under GPL-2.0-only with Classpath Exception.
  - Thanks to [OptiScaler](https://github.com/optiscaler/OptiScaler) for upscaling / spoofing middleware support.

---

<a name="chinese"></a>
## 中文说明

本项目为 **NVIDIA RTX Remix for Minecraft (Java 版 1.21.1)** 的原生兼容补丁与 JNI 桥接层，使该全景光线追踪（Path Tracing）模组能够在 **AMD Radeon 显卡**（已在 AMD Radeon RX 9070 XT, RDNA 4 / gfx1201 上完整验证）上正常启动、交互并开启超分辨率。

### 核心解决的技术痛点

1. **WGL / Vulkan 窗口交换链冲突 (`VK_ERROR_NATIVE_WINDOW_IN_USE_KHR`)**：
   - Minecraft Java 版使用 GLFW 创建基于 WGL/OpenGL 的游戏主窗口。
   - AMD 原生 Vulkan 驱动严格遵循 Vulkan 规范：当一个 HWND 已经挂载了非空的 OpenGL 设备上下文（DC）时，对其调用 `vkCreateSwapchainKHR` 将强制报出 `VK_ERROR_NATIVE_WINDOW_IN_USE_KHR` 并导致崩溃。
   - **解决机制**：重构 `remixbridge.dll`，在底层自动创建完全纯净的独立子窗口（`WS_CHILD | WS_CLIPSIBLINGS`）专供 Vulkan 交换链渲染，彻底规避 WGL DC 独占冲突。

2. **Win32 窗口消息与输入中继**：
   - 独立窗口渲染容易导致鼠标独占失效、光标错位或无法操作游戏。
   - **解决机制**：实现了全套 Win32 消息透传与焦点重定向（`WM_MOUSEACTIVATE -> MA_NOACTIVATE`、原生鼠标 Delta 增量转发与焦点同步），确保第一人称视角无缝操控与正常游玩。

3. **桥接器引导式预加载（解决 A 卡 OptiScaler 注入）**：
   - 在 A 卡环境下，RTX Remix 的 `d3d9.dll` 会直接返回 `0xbad00001` 并提前退出，根本不会触发对 `nvngx.dll` 的加载，导致常规 Hook 方式全部失效。
   - **解决机制**：定制版桥接器在加载 `d3d9.dll` 前，显式调用系统 `vulkan-1.dll` 并加载 `remix-sdk/OptiScaler.dll`，抢先在进程中安装 Vulkan 钩子，成功接管后续 NGX 调用并转接至 FSR 2.2+。

4. **NVX Vulkan 扩展伪装（绕过 `m_dlssSupported` 门禁）**：
   - RTX Remix 的渲染管线源码（`rtx_context.cpp`）硬编码要求物理设备必须同时支持 `VK_NVX_binary_import` 和 `VK_NVX_image_view_handle`，否则每帧内部计算 `m_dlssSupported == false` 并强行关闭 DLSS 调度。
   - **解决机制**：配套提供的 `config/OptiScaler.ini` 默认启用 `VulkanExtensionSpoofing = true`，向 Remix 暴露对应 NVX 扩展使门禁通过，并在底层创建真实设备时安全剔除，保证 AMD 驱动稳定运行。

---

### 玩家快速上手

1. 按照[官方教程](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)安装 Minecraft Java 版 1.21.1 及官方 RTX Remix 模组。
2. 从本仓库的 [Releases](https://github.com/TheAutomatic/rtx-remix-minecraft-amd-patch/releases) 页面下载最新预编译补丁包。
3. 将解压出的 `remixbridge.dll` 复制覆盖到游戏实例的 `remix-sdk/` 目录（例如 `<实例目录>/.minecraft/remix-sdk/`），替换原有的 `remixbridge.dll`。
4. 将 `OptiScaler.dll` 与 `OptiScaler.ini` 放置在同一个 `remix-sdk/` 目录下。
5. 在 `OptiScaler.ini` 的 `[Spoofing]` 小节下，确保开启扩展伪装（`VulkanExtensionSpoofing = true`）：
   ```ini
   [Spoofing]
   VulkanExtensionSpoofing = true
   ```
   *(必选设置：向 RTX Remix 暴露必要的 Vulkan 扩展，以解锁游戏内 Alt+X 菜单的 DLSS 选项)*。
6. 启动游戏即可在 AMD 显卡上体验全光追与超分辨率。

---

### 源码编译

**环境要求：**
- Windows 10/11 64位
- 安装了 C++ 工具集的 Visual Studio 2022 或 Visual Studio Build Tools (MSVC 14.3x / 14.4x)

**使用脚本一键编译：**
```cmd
build.cmd
```
产物将生成在 `bin/remixbridge.dll`。

---

<a name="license-zh"></a>
### 许可协议、防倒卖声明与鸣谢

- **本项目授权协议**：**PolyForm Noncommercial License 1.0.0**（详见 [LICENSE](LICENSE) 与 [NOTICE](NOTICE)）。
  - 源码公开，**完全免费**提供给个人用户非商业使用、学习与研究。
  - **严禁商业倒卖**：禁止在闲鱼、淘宝、拼多多等任何平台有偿售卖本软件及其编译产物，禁止打进收费整合包或付费网盘/付费群分发，**严禁以“收费代安装 / 远程调试 / 付费优化”等名义向他人有偿提供本工具**。如您是付费购买所得，请立即申请退款！
- **上游与第三方归属**：
  - 上游原始桥接代码来自 [NVIDIAGameWorks/rtx-remix-minecraft](https://github.com/NVIDIAGameWorks/rtx-remix-minecraft)，保留其 **Apache License 2.0** 授权（见 [THIRD_PARTY_LICENSES/NVIDIA-rtx-remix-minecraft-Apache-2.0.txt](THIRD_PARTY_LICENSES/NVIDIA-rtx-remix-minecraft-Apache-2.0.txt) 及 [NOTICE](NOTICE)）。
  - `include/remix_c.h` 采用 MIT 协议，OpenJDK JNI 头文件采用 GPL-2.0-only WITH Classpath-exception-2.0 协议。
  - 感谢 [OptiScaler](https://github.com/optiscaler/OptiScaler) 提供的中间层转接能力。
