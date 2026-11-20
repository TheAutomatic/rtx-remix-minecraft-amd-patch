# RX 9070 XT（RDNA4）光追管线模式与 Wave32 稀疏光追实测记录与调优建议

> 测试日期：2026-11-20  
> 适用对象：NVIDIA RTX Remix for Minecraft（Java Edition 1.21.1）+ AMD Radeon RX 9070 XT（RDNA4 / `gfx1201`）

---

## 一、给 RX 9070 XT（RDNA4）的推荐建议（速查）

**结论：使用已解锁 Wave32 稀疏光追（Sparse Rendering）的 `remixbridge.dll`，配合全 RGS 组合 `(2, 1, 2)` 与 Ray Reconstruction（如 OpenRR），可从官方 AMD 默认无稀疏的 `28.4–28.7 FPS` 直接提升至 `58.9–59.1 FPS`（约 2.0×），同时降低进程显存约 310 MB。**

在游戏实例目录的 `.minecraft/user.conf` 中保持以下 4 项（**必须包含 `rtx.raytraceModePreset = 0`**，否则单项配置会被默认 `Auto` 覆盖）：

```ini
rtx.raytraceModePreset = 0
rtx.renderPassGBufferRaytraceMode = 2
rtx.renderPassIntegrateDirectRaytraceMode = 1
rtx.renderPassIntegrateIndirectRaytraceMode = 2
```

### 核心要点速览
1. **无论是否开启稀疏光追，`(2, 1, 2)` 全 RGS 都是 RX 9070 XT 第一名**：
   - **关闭稀疏光追（全像素）**：`29.6–30.4 FPS`（比官方 AMD 默认 `(0, 0, 2)` 的 `28.4–28.7 FPS` 快 ~1.3 FPS / +4.5%～5%，且消除 Direct 走 CS 的偶发掉帧波动）；
   - **开启 Wave32 稀疏光追**：**`58.9–59.1 FPS`**（相比无稀疏接近 **2.0×**，相比官方默认无稀疏提升 **+105%**，仍领先开启稀疏下的 `(0, 0, 2)` / `(0, 1, 2)` `58.4–58.9 FPS` 及 `(1, 1, 2)` `56.3–56.5 FPS`）。
2. **三个 Pass 的选择规律（RX 9070 XT）**：
   - **Indirect（间接光）**：**必须保持 `2`（`Trace Ray (RGS)`）**；切 `1`（`Ray Query (RGS)`）会掉约 4 FPS，切 `0`（`Ray Query (CS)`）会腰斩至 ~15 FPS。
   - **GBuffer（主光线 + PSR）**：推荐 **`2`（`Trace Ray (RGS)`）**；开启稀疏光追后 Direct/Indirect 光线量大幅减少、而 GBuffer 仍覆盖 100% 像素，切 `1`（`Ray Query (RGS)`）会直接落后 `~2.6 FPS`（`56.3–56.5` vs `58.9–59.1`）。
   - **Direct（直接光）**：推荐 **`1`（`Ray Query (RGS)`）**；无稀疏时单点提速 `+1.1～1.2 FPS` 且更稳，开启稀疏后虽与 `0` 差距收窄（Direct 光线占比降低），整体仍以 `(2, 1, 2)` 表现最佳。
3. **开启 OpenRR 不比“关 RR + FSR 4.1”慢**：本机实测 4K 性能档下 OpenRR（Vulkan 原生直通 + ACO，fast mode）耗时始终稳定在 `p50 7.39–7.45 ms`（~886 MB），开启稀疏光追后总帧时间仅 `~16.95 ms`（其中路径光追主干压缩至 `~9.5 ms`，相比无稀疏的 `~25.9 ms` 快约 **2.7×**）。
4. **调优避坑（已实测踩坑项）**：
   - 保留 Minecraft 模组原有的 `Custom` 参数基线（`quality = 0.3`、`samplingRate = 0.1`），不要随意切到 `rtx.graphicsPreset = 0 (Ultra)` + `quality = 0.5`（自适应采样率会顶过 `maxSamplingRate = 0.75` 并大幅推高全套开销，显存回涨至 `5702 MB`，均帧跌至 ~18.5 FPS）；
   - 不要开启 `enableSparseVolumetricsPrimaryHit / PrimaryMiss = True`（当前轻量化体积雾下反而增加同步/带宽开销，稳态掉约 2～3 FPS 且 OpenRR `p95` 从 `7.47 ms` 升至 `9.19–9.34 ms`）。

---

## 二、测试环境与实测数据

### 测试环境
- **GPU / 驱动**：AMD Radeon RX 9070 XT（RDNA4 `gfx1201`），Windows Vulkan 驱动 `26.11.2`
- **RTX Remix 渲染器**：`d3d9.dll`（`DXVK_Remix: remix-main+a00da12a`）
- **桥接补丁**：`remixbridge.dll`（集成仅对 `d3d9.dll` 生效的 Vulkan IAT `Wave32` 稀疏光追补丁；支持 `REMIX_AMD_SPARSE_WAVE32=0` 开关关闭）
- **超分与光追重建**：OpenRR `v0.1.7plus`（Vulkan Host 原生直通 + ACO strict，fast mode），DLSS 性能档（`rtx.resolutionScale = 0.5`，`rtx.qualityDLSS = 1`，`rtx.dlssPreset = 2`）
- **显示模式**：`options.txt` 中 `fullscreen:true`，真全屏 `1920×1080 → 3840×2160`
- **光照与路径设置**：SHaRC（Spatially Hashed Radiance Cache）激活，`rtx.pathMinBounces = 0`，`rtx.pathMaxBounces = 2`，半透明彩色阴影开启（`rtx.conf` 默认）

### 参数编码说明（按 `d3d9.dll` 实际支持范围）

| 配置键 | `0` | `1` | `2` |
| :--- | :--- | :--- | :--- |
| `rtx.renderPassGBufferRaytraceMode` | `Ray Query (CS)` | `Ray Query (RGS)` | `Trace Ray (RGS)` |
| `rtx.renderPassIntegrateDirectRaytraceMode` | `Ray Query (CS)` | `Ray Query (RGS)` | *(不支持)* |
| `rtx.renderPassIntegrateIndirectRaytraceMode` | `Ray Query (CS)` | `Ray Query (RGS)` | `Trace Ray (RGS)` |

---

### A 组：开启 Wave32 稀疏光追（`S1–S4`，`quality = 0.3`，`samplingRate = 0.1`）

> `remix-bridge-amd.log` 确认：`subgroupSize 64 -> 32` 能力放行，`compact_active_pixels`（`44,604 B`，`LocalSize=(32, 32, 1)`）强制 `requiredSubgroupSize = 32` + `REQUIRE_FULL_SUBGROUPS_BIT` 返回 `VkResult = 0`，`remix-dxvk.log` 无禁用警告。

| 轮次 | 配置 `(GBuf, Dir, Ind)` | 稳态 FPS（实测读数） | OpenRR p50 | 全程均帧（含进图预热） | 进程显存 | 备注 |
| :---: | :---: | :---: | :---: | :---: | :---: | :--- |
| **`S1`** | **`(2, 1, 2)` 全 RGS + 稀疏** | **58.9–59.1** | 7.41 ms | 56.82 FPS (5217f / 91.8s) | 5390 MB | **总榜第一（稳态最高）** |
| **`S4`** | `(0, 1, 2)` GBuf CS + Dir RQ RGS | ~58.4–58.9 | 7.40 ms | 57.13 FPS (5597f / 98.0s) | 5389 MB | 与 S3 体感几乎无差，全程均帧因跑得更久略摊薄预热 |
| **`S3`** | `(0, 0, 2)` 官方默认 + 稀疏 | 58.4–58.9 | 7.40–7.43 ms | 56.08 FPS (3946f / 70.4s) | 5375 MB | Direct 稀疏减负后追近，但仍略逊于 S1 |
| **`S2`** | `(1, 1, 2)` GBuf+Dir 双 RQ RGS | **56.3–56.5** | 7.42 ms | 53.31 FPS (2373f / 44.5s) | 5397 MB | 稠密 GBuffer 用 `RQ RGS` 掉约 2.6 FPS |

---

### B 组：未开启稀疏光追基线（`R1–R7`，全像素追踪参考对照）

> 此前受 `[RTX Sparse Rendering] Device subgroup size is 64, but compact_active_pixels.comp.slang requires 32` 限制，稀疏光追处于禁用状态。

| 轮次 | 配置 `(GBuf, Dir, Ind)` | 稳态 FPS（实测读数） | OpenRR p50 | 全程均帧（含进图预热） | 进程显存 | 备注 |
| :---: | :---: | :---: | :---: | :---: | :---: | :--- |
| **R7** | **`(2, 1, 2)` 全 RGS（复测）** | **~30.0，高峰 30.4** | 7.41–7.42 ms | **29.00 FPS** (2017f / 69.6s) | 5697 MB | State cache `296` 条，热缓存复测 |
| **R2** | **`(2, 1, 2)` 全 RGS** | **29.6–30.0（稳）** | 7.41 ms | 28.60 FPS (1596f / 55.8s) | 5697 MB | 无稀疏状态冠军 |
| **R5** | `(1, 1, 2)` GBuf+Dir 双 RQ RGS | ~29.6–29.9 | 7.40–7.45 ms | 29.17 FPS (4683f / 160.5s) | 5732 MB | 中途天气变化，体感稳态略逊于 R2 |
| **R4** | `(0, 1, 2)` 仅 Direct 改 `1` | 29.6–29.8 | 7.40 ms | 28.77 FPS (2184f / 75.9s) | 5696 MB | 验证 Direct 切 RGS 为核心提速项 |
| **R3** | `(2, 0, 2)` 仅 GBuffer 改 `2` | 29.0–29.4 | 7.42 ms | 28.34 FPS (2580f / 91.0s) | 5691 MB | 偶发回落至 28.7，帧率抖动明显 |
| **R1** | `(0, 0, 2)` 官方 AMD 默认 | 28.4–28.7 | 7.44 ms | 26.69 FPS (1136f / 42.6s) | 5695 MB | 官方 AMD 默认无稀疏基线 |
| **R6** | `(2, 1, 1)` Indirect 改 RQ RGS | **25.6–25.8** | 7.43 ms | 24.91 FPS (1960f / 78.7s) | 5705 MB | 比 `Indirect = 2` 慢约 5.5 ms/帧 |
| *(前期)* | `(0, 0, 0)` 全 CS（窗口化参考） | **~15** | ~7.2 ms | ~15 FPS (548f / 37s) | — | Indirect 走 CS 性能腰斩 |

> 说明：两组测试中 OpenRR GPU `p50` 始终锁定在 `7.39–7.45 ms`，证明不同轮次的帧率与显存变化（`~5697 MB → ~5390 MB`）完全来自光追管线与 Wave32 稀疏光追本身。

---

## 三、机制与原因分析

1. **为什么解锁 Wave32 稀疏光追能直接从 ~30 FPS 翻倍到 ~59 FPS？**
   - 解锁后 `compact_active_pixels.comp.slang` 以真正的 **Wave32** 执行像素活跃度筛选与前缀压缩（`CompactedPixelIndices`），只对约 `samplingRate`（10%～30%）的活跃像素调度 Direct / Indirect 光追追踪，同时启用 Compact GBuffer 省下约 **310 MB** 显存。
   - 扣除每帧固定约 `7.4 ms` 的 OpenRR 开销，光追与渲染主干耗时从 `~25.9 ms` 骤降到 `~9.5 ms`（**提速 ~2.7×**），按阿姆达尔定律折合总帧率接近 **2.0×**。

2. **为什么开启稀疏光追后，GBuffer `2 (TraceRay RGS)` vs `1 (RayQuery RGS)` 拉开到 2.6 FPS 差距，而 `(0, 0, 2)` 追近到 `58.4–58.9`？**
   - 稀疏光追只削减 **Direct 与 Indirect** 光线，**GBuffer（主光线 + PSR）仍必须覆盖 100% 屏幕像素**以供给 OpenRR/DLSS-RR 稠密 Guide。
   - 当 Direct/Indirect 耗时被砍薄、总帧时间缩至 `~17 ms` 时，GBuffer 在剩余光追时间里的占比大幅上升，暴露出 **`2 (Trace Ray RGS)` ≥ `0 (Ray Query CS)` ≫ `1 (Ray Query RGS)`** 的固有差距（`S1` 比 `S2` 快 ~0.8 ms/帧，在 60 帧尺度上对应 `+2.6 FPS`）。
   - 同时 Direct 的光线基数被显著压缩后，Direct `0` 与 `1` 的差距自然缩小，但 `(2, 1, 2)` 依然守住稳态第一。

3. **为什么不建议强切 `Ultra` 预设或开启体积光稀疏？**
   - **`rtx.graphicsPreset = 0 (Ultra)` + `quality = 0.5`**：实测 `39.2 s` 仅 `725` 帧（~18.5 FPS），进程显存回到非稀疏的 `5702 MB`。在 1080p 内部渲染分辨率下，Ultra 自带的较高基准采样率叠加 `quality = 0.5` 容易触碰 `maxSamplingRate = 0.75` 关闭阈值，加之 Ultra 全量反弹/SHaRC/体积雾成本大幅增加，反而严重拖垮性能。
   - **`enableSparseVolumetricsPrimaryHit / PrimaryMiss = True`**：实测对比显示在保留轻量化 froxel 参数（`froxelGridResolutionScale = 32`、`froxelDepthSlices = 48`）下开启两项会导致稳态降低约 `2–3 FPS`，且 `openrr-amd.log` 的 `p95` 出现 `9.19–9.34 ms` 毛刺（其余各轮均稳定在 `7.45–7.50 ms`）。

---

## 四、自测比较与其它显卡复测建议

如果你使用的是其他 AMD 显卡（如 RDNA3 / 其他 RDNA4 型号）或后续升级了驱动 / RTX Remix，可按以下控制变量方法快速对比：

### 1. 控制变量清单
- **固定真全屏**：`.minecraft/options.txt` 设置 `fullscreen:true`（避免窗口化边框导致非标准尺寸）。
- **锁时间与天气**：在测试存档执行 `/time set noon`、`/gamerule doDaylightCycle false`、`/weather clear`、`/gamerule doWeatherCycle false`（严格基准可再设 `renderClouds:"false"`）。
- **固定视角与热缓存**：进图不动视角静置 10 秒等 SHaRC/NRC 收敛后读稳态 FPS；新组合首轮建议视作预热局。
- **核对双日志生效**：
  - `rtx-remix/logs/remix-bridge-amd.log` 确认 `matched compact compute shader ... VkResult=0`；
  - `rtx-remix/logs/remix-dxvk.log` 确认无 `Sparse rendering will be disabled` 警告且三行 `RenderPass ... Raytrace Mode:` 与目标一致。

### 2. 精简对比组（只需 3～4 轮）
保持 `rtx.raytraceModePreset = 0` 且固定 `Indirect = 2`：
1. **`(2, 1, 2)`**：全 RGS（RX 9070 XT 推荐冠军，开启与关闭稀疏均为第一）；
2. **`(0, 1, 2)`**：GBuffer CS + Direct RQ RGS；
3. **`(0, 0, 2)`**：官方 AMD 默认基线（可用 `REMIX_AMD_SPARSE_WAVE32=0` 与开启稀疏做直接 A/B 对比）；
4. **`(1, 1, 2)`**（可选）：验证当前卡型下稠密 GBuffer 是否同样明显弱于 `2` 和 `0`。
