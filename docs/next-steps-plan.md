# 下一步开发与优化计划（RTX Remix for Minecraft - AMD）

> 更新日期：2026-11-20  
> 关联实测文档：[raytrace-mode-benchmark-9070xt.md](raytrace-mode-benchmark-9070xt.md)

当前阶段已完成 AMD 显卡基础兼容（WGL/Vulkan 交换链分离、Win32 输入转发、OptiScaler/OpenRR 注入）、RX 9070 XT 光追管线模式调优（`(2, 1, 2)` 全 RGS），以及 **基于 `remixbridge.dll` Vulkan IAT Hook 免重编 `d3d9.dll` 的 AMD Wave32 稀疏光追解锁方案实现与实机验收**。

---

## 一、已完成的核心里程碑（2026-11-20）

### 1. 第一顺位方案落地：`remixbridge.dll` 解锁 AMD Wave32 稀疏光追（已实现并实机验收）
在无需搭建 `dxvk-remix` 重型构建环境、不改动/重编 ~300 MB `d3d9.dll` 的前提下，通过改造 `src/remixbridge.cpp` 完成轻量 Vulkan 拦截并实机跑通：
- **隔离挂钩点**：仅对 `d3d9.dll` 的 `vulkan-1.dll!vkGetInstanceProcAddr` IAT 槽位挂钩（对 OptiScaler、OpenRR、NRC、LWJGL 等其余模块完全透明）；提供 `REMIX_AMD_SPARSE_WAVE32=0`（支持 `0/false/off`）环境变量一键关停。
- **零副作用能力门禁**：按真实硬件能力（`subgroupSize == 64` 且 `VK_EXT_subgroup_size_control` 支持 `minSubgroupSize <= 32 <= maxSubgroupSize`、`COMPUTE_BIT`、`computeFullSubgroups`、`maxComputeWorkgroupSubgroups >= 32`）而非 `vendorID` 触发（免疫 OptiScaler NVIDIA 伪装），仅向 `d3d9.dll` 汇报 `subgroupSize = 32`；通过全量二进制审计确认 `d3d9.dll` 仅在稀疏准入和通用 `>= 1` 检查处读取该值。
- **VUID 合规特性开启**：在 `vkCreateDevice` 中安全确保 `subgroupSizeControl` 与 `computeFullSubgroups` 为 `VK_TRUE`（复用已有 `Vulkan13Features` 或 `SubgroupSizeControlFeatures`，严格避免双挂冲突，并在 `< 1.3` 时按需补全 `VK_EXT_subgroup_size_control` 扩展），与 OptiScaler 和 OpenRR `PrepareVkDevice` 和谐共存。
- **精准 SPIR-V 结构匹配（`1/647`）**：仅针对全引擎唯一存在 32 位 `ballot().x` 假设的 `compact_active_pixels.comp.slang`（`GLCompute`、`LocalSize=(32, 32, 1)`、`OpGroupNonUniformBallot` / `s_blockActiveMask` / `TraceRaysIndirectArgs`）挂载 `VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT (0x2)` 与 `requiredSubgroupSize = 32`（`& ~ALLOW_VARYING_SUBGROUP_SIZE_BIT`），不误伤其余 Wave64 安全且 `localSizeX=16/8` 的稀疏后续核。

### 2. RX 9070 XT 实机验收与 A/B 对比结果（已完成 `S1–S4`）
- **日志与导出验证**：`bin/remixbridge.dll` 编译零警告零错误，70/70 PE 导出符号完整对齐；游戏运行确认 `matched compact compute shader ... VkResult=0`，`remix-dxvk.log` 稀疏禁用警告消除。
- **性能与显存实测（4K 性能档 `1920×1080 → 3840×2160` + OpenRR）**：
  - 从无稀疏全 RGS 的 **`29.6–30.4 FPS`**（默认 `(0, 0, 2)` 为 `28.4–28.7 FPS`）直接飙升至 **`58.9–59.1 FPS`**（**接近 2.0× 总帧率提升**，路径光追本体加速 ~2.7×）；
  - 进程显存因 Compact GBuffer 生效从 `5697 MB` 降至 **`~5390 MB`（节省 ~310 MB）**，OpenRR 保持稳定 `p50 7.40–7.43 ms`。
- **完整管线复测与调优定标**：完成 `S1 (2, 1, 2)`、`S2 (1, 1, 2)`、`S3 (0, 0, 2)`、`S4 (0, 1, 2)` 及画质预设/体积雾稀疏实验，已整理沉淀至 [raytrace-mode-benchmark-9070xt.md](raytrace-mode-benchmark-9070xt.md)。

---

## 二、后续待办与可选方向

### 1. 长期场景验证与跨卡兼容性检查
- **多场景长时间游玩稳定性检查**：在不同光照/天气/材质场景（水体折射、彩色玻璃、密林树叶、洞穴、雨雪与快速移动）下持续观察是否有任何边缘伪影或崩溃，并核对关闭/切换 OptiScaler + FSR 与开启 OpenRR 两种路径下的表现。
- **RDNA3 / RDNA2 兼容性实测**：在 Radeon 780M / RX 7000 系列（驱动已验证上报 `minSubgroupSize = 32`）上确认稀疏光追解锁路径同样零异常跑通。

### 2. 本仓库代码与文档收尾、发版准备
- 核对 [README.md](../README.md) 中英文说明与 `docs/` 引用是否准确无误；
- 审阅 `src/remixbridge.cpp` 的 `git diff`，待确认无误后准备新版 Release 打包与发布；
- 同步评估与 `OptiScaler-ASI-Launcher`（MC 模式预设/提示）及新版 `DLSSNR-AMD` 的联合回归测试。

### 3. 可选深入探索（不阻塞当前版本发布）
- **向上游 `dxvk-remix` 贡献原生支持**：如后续需长期彻底免 Hook，可基于 [NVIDIAGameWorks/dxvk-remix](https://github.com/NVIDIAGameWorks/dxvk-remix) 提交上游 PR（原生接入 `VK_EXT_subgroup_size_control` 或让 `compact_active_pixels.comp.slang` 兼容 Wave64）。
- **“Wave32 CS vs RGS”机制验证**：利用已建好的 IAT Hook 通道，可单独实验将 `Direct CS` 管线也强制切为 `Wave32`，进一步验证先前推测的 `Direct RGS > Direct CS` 是主要源于 Wave64 还是 RT 管线专属优化。
