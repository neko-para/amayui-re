# 复审洞收口：守卫/文案与实现不符（d3dx9 拷贝族 3 条 + AOT 音频 4 条）

- id: REQ-01M7K04EY6D3GQNB5DRWP0DYJ8
- type: req
- status: open
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- tags: [emulator-lift, review, d3dx9, audio]

## 一句话

独立复审（2026-11-17，只读、按提交态）在两处**已合并**的改动里找到 6 个洞 + 1 条自证式断言。
本单只收「复审找出来的、还没修」的部分；每条的**修复判据**与**父级裁决**都写在下面，⛔ 不要重新裁决。

## 洞 1 · `22→21` 把 X8 的顶字节写成目标 alpha（🔴 真可见像素）

`src/runtime/d3d9.cpp` 的 `surfaceCopyPlan` 现在让 `21↔22` **双向**恒等直拷。
本仓自己的格式表就写 X8 顶字节**不携带信息**（`f.amask = 0u`）⇒ `21→22` 丢掉安全，
但 **`22→21` 会把它写成目标的 alpha** ⇒ 目标被采样 / 混色时**像素变透明**（MS 文档：本函数 "loads … with color conversion"）。

- **裁决：`22→21` 一律 `supported=false` ⇒ 响亮停（`Trap`）**。⛔ **不要**悄悄填 `0xFF` —— 那是"猜一个转换语义"，
  与 §6 准入门冲突；真机口径未取证，猜错就是静默错像素。
- 判据：`format-pairs` 里加一格真 surface 用例（src=X8、顶字节 0 → dst=A8）⇒ 断言 `Trap` + 目标一字节不变。
- ⛔ `21→22`（丢 alpha）保持允许 —— 那是**丢信息**，不是造信息。

## 洞 2 · 同一 surface 自拷贝 ⇒ `memcpy` 重叠 UB（🔴 真 UB）

`copySurfaceRect` 的行拷贝不比较源/目标对象；`src_surface == dst_surface` 且矩形错位时行区间重叠 ⇒
`memcpy` UB，且会把尚未读到的源行覆盖掉（像素搬花）。

- **裁决：`src == dst` 一律 `D3DERR_INVALIDCALL`**（真机行为未取证，⛔ 不要猜"应该支持自拷贝"）。
- 判据：`contract` 组加一格"同一 surface、目标偏移 (1,1)" ⇒ INVALIDCALL + 目标一字节不变 + `unimplemented_calls` 不涨。

## 洞 3 · ColorKey 的拒绝**理由**写错了（🟡 只是文案，但会误导下一个人）

代码注释说"文档没写死比较口径"——文档**明写** "always a 32-bit ARGB color … Alpha is significant"。
⇒ 不是无法定义，是**本期没做**。

- **裁决**：文案改成「本期未实现（文档已定义：32-bit ARGB、alpha 参与比较）」，并引链接。
- 判据：`color-key` 用例断言 `Trap`（已有）+ 注释带上文档口径。

## 洞 4 · `present_flags.h` 的 `kAotSoundNotice` 指向一行可能不存在的东西（🟡 文案）

它说"读数：收口时 stdout 的 `audio     :` 一行"，但 `aot_host.cpp` 只在 `enabled()` 时打，
而**只给 `--sound`** 时档位是 off ⇒ 用户按它去找 `muted=0` 会找不到任何行。

- **裁决**：文案限定「仅 `--audio virtual|sdl` 或 `--audio-dump` 时才有这一行」。
- 判据：`--sound`（不给 `--audio`）跑完 ⇒ stdout **无**该行；`--sound --audio virtual` ⇒ **有**。

## 洞 5 · `kAotSoundNotice` 说"要 PCM 落盘请给 `--audio-dump`"，而 dump 只有 44 字节头（🟡 文案）

`writeDump` 只由 `generateTo` 调；S3 接线前 AOT 没有 `pump()` ⇒ dump = 44 B 头 + `data=0`。

- **裁决**：文案写成"只建 44 字节头（data=0）；（S3 接线后才有 PCM）"。S3 落地后**再改一次**。
- 判据：`--audio-dump x.wav` 跑完 ⇒ 断言 `size == 44` / `data` 字段 0。

## 洞 6 · 自证式断言：`cli-aot-flags` 的 `sound-accepted-with-once-notice`（🔴 假守卫）

它只断言 notice 文本里含 `muted=0`，**从不断言 AOT 路径的 `AudioEngine::muted() == false`** ⇒
`AotHost::configureAudio` 就算丢掉 `--sound` 也照绿。

- **裁决**：断言必须落在**引擎状态**上（`muted()` 或等价的机械读数），⛔ 不许拿自己打的文案当证据。
- 判据：把 `--sound` 的接线删掉 ⇒ 该用例**必须红**（实测贴输出）。

## 洞 7（低）· `std::abort()` 路径跳过 `end()` ⇒ WAV 两个长度字段不回填（🟡）

`heap.cpp` / `handles.cpp` / `shims.cpp` 的 abort 路径不经过 `aot_run.cpp` 的收口 ⇒ dump 头没回填。
⛔ 不修 abort 语义；**裁决 = 明确声明**「abort 之后 dump 不可信」，并写进 `--audio-dump` 的说明。

## 判不了（不要在这张单里猜）

`22→21` 是否真被游戏走到（缺两个 surface 的 `D3DFORMAT` 机械记录）· 行尾 padding 无夹具（代码对，只能读代码判）·
32/33 与跨序臂只有纯函数守卫（`formatForD3dFormat` 不认 32/33 ⇒ 建不出真 surface）· `D3DX_FILTER_SRGB_IN/OUT` 在 1:1 里要不要拒绝。

## 判据

1. 每条洞的修法都要有**能红**的用例（洞 4/5/7 是文案，用"有没有那一行 / 文件多大"这类机械断言）。
2. ⛔ 不改 guest 可见语义的既有部分（`exit`/`frames`/`stop @`/arena/`misses` 逐位不变）。
3. 修完 `node tools/mutate-check.mjs --list` 绿；新条目由父级统一加。
