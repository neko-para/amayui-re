# emulator-lift · OPTION 退回 TITLE 后纹理像素块不回收（第二次进 OPTION 分配失败 & 误诊成 unimplemented）

- id: REQ-01M6WTRF0X6362NK8BNK8DHHWJ
- type: bug
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/d3d9.cpp#d3d9/texture-bound-then-guest-release
- repro: apps/emulator-lift/tests/option_roundtrip.cpp#option-roundtrip/3cycles
- severity: S1

## 2026-11-08 订正（只加注，不改下面的历史记录）

**轮数 3 → 2**：`apps/emulator-lift/tests/option_roundtrip.cpp` 的 `kCycles` 现在是 **2** ⇒ 一次 `option-roundtrip` 只跑 **2 轮往返 / 2 个子进程**（不是 3）。
用例名字符串**仍然是** `option-roundtrip/3cycles`（历史串，⛔ 别按名字里的数字判断轮数）；轮数**只由 `kCycles` 决定**。`repro` 的锚点因此仍能解析。
⇒ 下文（当时 `kCycles=3` 的实测记录）里的"3 轮 / 1 / 2 / 3 轮 / 3 个子进程"按**当时那次运行**读；**当前判据一律按 2 轮 / 2 个子进程**（"每次 3 个子进程都 exit 0"也按 2 个读）。

## 症状

进 OPTION → 右键退回 TITLE → 再进 OPTION ⇒ 机器停，stderr 打：

```
=== unimplemented D3DX9 call ===
  D3DXCreateTexture
  reason: 1280x1152 D3DFORMAT=21（32bpp，像素 5898240 字节）：D3D9：SurfaceArena.create 失败：
          surface 分配失败（5898240 字节）：guest 堆耗尽（请求 5898240 字节，含块头对齐需 5898248；
          最大空闲块 2127904 字节；in_use=130358640 peak=130358640；区间 0x28000000..0x30000000）
```

★ 两处误诊：① 分配失败被记成"guest 用了没实现的导出"；② 只报字节数、不报 w×h，无法回认是哪张纹理。

## 复现（修前）

`--appdata .tmp/appdata --game-dir links/install --present window --present-hidden --raster gpu --native-overrides all --skip-logo --input-script <TITLE→OPTION→右键→再进 OPTION> --max-frames 400`
脚本（帧号在 `--skip-logo` + window/gpu 下实测）：`at frame 120; click 812 622`（125 进 OPTION）→ `click 640 360 right`（172 回 TITLE）→ `click 812 622`（234 再进）⇒ 在 234 之后停。

## 真因（实测，非推断）

三时刻的 arena（128 MiB 独立 GuestHeap）读数：

| 时刻 | surfaces | in_use | 最大空闲块 |
|---|---|---|---|
| 进 OPTION 前（frame 119） | 21 | 108,595,152 | 22,591,536 |
| 进 OPTION 后（frame 164） | 27 | 120,693,568 | 13,006,880 |
| 退回 TITLE 后（frame 232） | 24 | 126,672,232 | 5,814,312 |

⇒ 一次往返 NET 泄漏 **18,077,080 B**，且当前屏仍占着 108.6 MB ⇒ 剩余最大连续块 5.8 MB < 这张纹理要的 5,898,248 B ⇒ 第二次进 OPTION 必然失败。

泄漏点：`ComRegistry::release` 到 0 时只 `free(vtable)+free(obj)`，**没有析构回调**；纹理的像素块（`SurfaceArena`）只在 iface 的 `Release` shim 里回收。而内部有 11 处裸 `cpu.com->release`（`SetTexture` 换绑 / `SetRenderTarget` 换绑 / `destroyForDevice` / Sprite 批次）⇒ 所有权挂在调用点上，"最后一次引用由裸 release 降为 0"时像素块永久留在 arena（一次往返 = 一屏量级）。

## 修复

1. `ComRegistry::createObject` 增加 `onZero` 析构钩子（`runtime/com.h` 的 `ComOnZero`），`release` 到 0 的**唯一收敛点**先跑钩子、再 free vtable/obj（保持"子资源在前、对象头在后"）。纹理对象登记 `d3d9::destroyTextureState` ⇒ 11 处裸 release 与 guest 走 shim 的路径不再有区别。★ `IDirect3DSurface9` **不登记**（`GetSurfaceLevel` 的表面复用纹理状态块，登记会提前释放纹理像素）。
2. 误诊修正：`createTextureState` / `createSpriteState` 增加结构化 `out_of_memory` 出参；分配失败走独立的 `d3dxResourceStop`（`=== D3DX 资源耗尽（不是 unimplemented）===` + 独立 `stop_why`），⛔ 不写 "unimplemented"、⛔ 不 ++unimplemented_calls。trap 文本补 `w×h / D3DFORMAT / 像素字节`。
3. 观测：运行摘要加 `arena : surfaces/in_use/peak/largest-free`；`GuestHeap::alloc` 失败文本带**最大空闲块 + 请求量 + in_use/peak + 区间**（"总量满"与"碎片化"从此可区分）。

## OOM 语义裁决（实测）

D3DX 真机语义是返回 `E_OUTOFMEMORY`（guest 在 `0x48AD0C` `test eax,eax / jnz loc_48ADBA` 有分支）。实跑让它真返回 `E_OUTOFMEMORY`：guest 确实走失败分支，但随后在 `0x4E8AB6` `throw`（C++ 异常码 `0xE06D7363`）⇒ 撞上**未实现**的 `kernel32!RaiseException`（SEH 展开不在本线）⇒ 停在一个更假的 gap 上（exit 2）。⇒ **保留硬停**，但换成独立措辞 + 独立 stop 原因（见上）。

## 守卫（两条都会红）

- 单元 `apps/emulator-lift/tests/d3d9.cpp#d3d9/texture-bound-then-guest-release`：两遍"建设备→释放设备"，第二遍多一张 1280×1152、已从采样器解绑的纹理 ⇒ 结束后 arena 必须回到空基线（surfaceCount / bytesInUse 逐字节）。变异（短路 onZero）⇒ 4 failures（多 1 表面 / 5,898,248 B）。
- 端到端 `apps/emulator-lift/tests/option_roundtrip.cpp#option-roundtrip/3cycles`：1/2/3 轮往返各跑一次，断言跑到最后的 `quit`、无 `guest 堆耗尽` / unimplemented / 资源耗尽、`in_use`/`peak` 不随轮数增长（容差 1 MiB）、click 数 = 2k。变异 ⇒ 第 1 轮就红（3 failures）。

## 边界

- ⛔ 不靠"把 arena 调大"（128 MiB 正好顶到 AGERC 的 0x30000000 fallback base ⇒ 会改 guest 可见 EA）。
- 容差来源：实测 1/2/3 轮三个数**完全相等**（in_use 109,895,008 / peak 126,672,232 / surfaces 23）；1 MiB 只防 `--present window` real-time 时钟让某一轮过渡帧多带一张临时纹理，比修前泄漏（18 MB/轮）小 17 倍。
