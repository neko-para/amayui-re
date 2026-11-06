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
| `0x48B090` | 6016.7 ms | `NormalTexture::UploadImage`；native 原型**已实现但破坏后续帧纹理**，已从 known 表撤下（见缺陷单） |
| `0x414AC0` | 1251.5 ms | **不整函数 native**：文件 I/O + guest 可见 `vector<string>`/索引容器 + `VirtualAlloc` 池；替代路径是其插入子路径 |
| 其余 | <1.0 s | 不在本单 |

- `sub_48B090` 的 8bpp/24bpp→32bpp + 嵌套 guest Create 原型：首帧 hash 一致，但
  **300 帧 A/B 从第 2/47 帧起分歧**（详情与 repro 见缺陷单 `REQ-01M6PGQEXZYPYE3Y07C8JCH7HT`）。
  当前 known 表**不注册**它；`--native-overrides all` 只包含 6 项（CRT + LZSS）。
- 现有可用档：真实 boot 首帧 **25.35 s → 12.68 s（2.00×）**，首帧 hash
  `0xEBF024FEBE998B05` 不变，4 帧逐帧 hash 与 CPU 一致。
- `sub_414AC0` 的停止理由：热段是 82k 条容器构建 + 文件 I/O，整函数覆盖需要克隆 guest
  容器/堆语义；不做整函数 native。

## 实现要点（保留给修复）

- `Block::ns` + `profile: functions by wall time`（按块计时；shim/override 不计）。
- `startNestedGuestCall()` + `NativeOverrideTable::invoke(..., suspended)`：native 可铺一个 guest
  回调帧；`Machine::run` 在 suspended 时直接继续。
- `nativeUpload48B090()` / `convertPal8To32()` / `convert24To32()`：原型与 pure 守卫
  （`native-override` 129+ checks）；**未注册**，不能启用。

## 验收（未满足）

1. 修 `REQ-01M6PGQEXZYPYE3Y07C8JCH7HT`：重新注册后 300 帧 CPU vs override 0 差异；
   8bpp（alpha 0/非 0）与 24bpp 都要有 pure fixture。
2. `sub_414AC0` 保持“不整函数 native”的结论；若要 1.25 s 另开插入路径单。
3. ctest/现有 golden 全绿；override 默认关闭。
