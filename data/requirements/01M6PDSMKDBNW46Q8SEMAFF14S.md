# emulator-lift · boot 中 >1s guest 函数的 native 化

- id: REQ-01M6PDSMKDBNW46Q8SEMAFF14S
- type: req
- status: doing
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/native_override.cpp#native-override
- tags: [emulator-lift native-override boot profile]

## 目标

用**墙钟**（不是只看指令权重）列出 boot 首帧累计 >1s 的 guest 函数，逐个 native override；
每个都保持解释器回退、有 pure 守卫、默认关闭，并用真实 boot A/B 验证。

## 实测（`--profile` 墙钟表；启用 LZSS/CRT override，max-frames=1）

| 函数 | 墙钟 | 结论 |
|---|---:|---|
| `0x48B090` | 6016.7 ms | `NormalTexture::UploadImage`；已进入 `all`；Windows 侧曾观察到纹理异常、macOS 未复现；用 `all,-sub_48B090` 可隔离 |
| `0x414AC0` | 1251.5 ms | **不整函数 native**：文件 I/O + guest 可见 `vector<string>`/索引容器 + `VirtualAlloc` 池；替代路径是其插入子路径 |
| 其余 | <1.0 s | 不在本单 |

- `sub_48B090` 的 8/24/32bpp + 嵌套 guest Create 原型：8bpp/24/32 与 alpha 已按 `.lst` 修；
  4/1bpp 源、16bpp 目标、色键≠0 仍显式回退。
  `--native-overrides all` **包含它**；需要排除时用 `all,-sub_48B090`（`-名字` 优先）。
- 现有可用档：真实 boot 首帧 **25.35 s → 12.68 s（2.00×）**，首帧 hash
  `0xEBF024FEBE998B05` 不变，4 帧逐帧 hash 与 CPU 一致。
- `sub_414AC0` 的停止理由：热段是 82k 条容器构建 + 文件 I/O，整函数覆盖需要克隆 guest
  容器/堆语义；不做整函数 native。
- 2026-11-06 复核：用户实测 Windows + `--native-overrides all` 渲染正确；此前 300 帧 A/B
  的分歧是**未补偿的虚拟时钟相位**（native 第 2/3 帧 = 基线第 3/5 帧，时间差 ≈2.3 s），
  不是纹理内容错误；时间补偿明确不做。对应缺陷单已按“不是缺陷”dropped。

## 实现要点（保留给修复）

- `Block::ns` + `profile: functions by wall time`（按块计时；shim/override 不计）。
- `startNestedGuestCall()` + `NativeOverrideTable::invoke(..., suspended)`：native 可铺一个 guest
  回调帧；`Machine::run` 在 suspended 时直接继续。
- `nativeUpload48B090()` / 像素核：8/24/32bpp + alpha 目标行序；锁走
  `d3d9::lockRect`/`unlockRect`；目标函数从 vtable 取；bpp/src 在 Create 后重读。
- `native-override` 196 checks / `guest-call` 27 checks；当前 `debug=false` 进入 `all`，
  可用 `-名字` 排除。
- **时间补偿明确不做**（用户裁决）；headless A4 不以这些 override 作基线。

## 验收

1. 非时钟缺陷已修（对应缺陷单 dropped：确认不是缺陷）；8bpp alpha 0/非 0、24bpp、
   32bpp 有 pure fixture 与 arena 逐字节；4/1/16bpp 与色键显式回退。
2. `sub_414AC0` 保持“不整函数 native”的结论；若要 1.25 s 另开插入路径单。
3. ctest/现有 golden 全绿；override 默认关闭；`all,-sub_48B090` 选择正确。
