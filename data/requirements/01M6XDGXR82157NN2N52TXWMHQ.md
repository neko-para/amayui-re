# emulator-lift · 音频默认静音（--sound 才出声）：设备/混音/喂料照跑，只在最终交付缓冲置 0

- id: REQ-01M6XDGXR82157NN2N52TXWMHQ
- type: req
- status: done
- parent: REQ-01M6K84KHPA1XF3B1HSHXME6W0
- verify: apps/emulator-lift/tests/audio.cpp#audio-device/default-muted-delivery
- tags: [emulator-lift, audio]

## 一句话

`emulator-lift` 的**默认状态是静音**：设备 / 混音 / 喂料 / 格式协商 / guest DirectSound 调用 / 统计**全部照跑**，只在**最终交付缓冲**上把 PCM 置 0；`--sound` 才把同一份缓冲原样送出去。

## 默认静音 ≠ 跳过音频（这是本单唯一要防的误读）

⛔ 静音**不是**"不建设备 / 不起 feeder / 提前 return / 把 guest 的 SetVolume 改成 0 / 降填充率"。
那些做法都会改变 guest 侧可观测行为或计数，于是"静音一次跑"与"出声跑"不再是同一份执行。
本实现只有一个覆盖点：`AudioEngine` 在 `mix()` 生成 `out` 之后、`pushMixed()` 之前的那一次 `std::memset(out.data(), 0, out.size())`（`src/runtime/audio.cpp`）。

因此判据写成"**同一条输入只差 `--sound`**"：`fills` / `mixed` / `notify` / 交付字节数必须**逐项相等**，只有 `muted_bytes` 与交付内容不同。

## 开关口径

- `--sound`：**输出门**，缺省关闭（= 静音）。与后端正交。
- `--audio <backend>`：`sdl`（缺省，真设备）· `virtual`（确定性时钟，CTest / dump 用）· `off`（旧 silent 口径，本就没有宿主设备）。
- 两者是**两个维度**：`--audio sdl` + 无 `--sound` = 真设备、真混音、真计数，交付缓冲置 0（设备打开与喂料不受影响）。

## 怎么查（可观测面）

- 运行摘要固定有一行：
  `audio     : mode=<sdl|virtual|off> device=<ok|fallback|none> muted=<0|1> muted_bytes=<n> fills=<n> fill_bytes=<n> mixed=<n> played=<n> notify=<n> deferred=<n> dropped=<n> underruns=<n>`
  - `muted=` 读的就是那个开关；
  - `muted_bytes=` 必须等于 `mixed=`（静音期间**确实有内容被覆盖**，不是"没有数据"）；
  - `fills=` 证明喂料真的跑过（缺省静音下 `fills` 不得为 0）。
- `device=none` + `muted_bytes=0` 是**合法**组合（`--audio virtual/off` 本来就没有交付），所以判据不要只看 `muted_bytes>0`。
- CTest `audio-device`（顶层无条件注册 ⇒ 默认档）：
  - `audio-device/default-muted-delivery`：dump 的 data 区**逐字节全 0**、长度 == 期望、`fills==1`、`notify>=1`、`muted==mixed`；
  - `audio-device/sound-on-delivery`：同一夹具出声 ⇒ 内容**不是**全 0，且逐字节等于 guest 环的斜坡、`muted==0`；
  - `audio-device/muted-delivery-parity`：两遍只差 `sound` ⇒ 喂料 / 混音 / 交付字节数相等。

## 判据

- `ctest -R audio-device` 绿。
- 去掉交付前那一次 `memset` ⇒ `audio-device/default-muted-delivery` 当场红（已在 `tools/mutate-check.mjs` 里登记为一条变异）。
- `pnpm test:mutation` 里 `--match runtime/audio.cpp` 那条 = 上述破坏 ⇒ 该 CTest 非 0。

## 边界（别当成已覆盖）

- 静音只覆盖**最终交付缓冲**；guest 自己往环里写的字节、混音结果、统计都不改（这正是要点）。
- `--audio off` 不是"静音"的另一种写法：它是没有设备模型的旧口径（`device=none`）。
- 真机听感（是否真的出声）不在自动判据里；自动判据只咬"缓冲内容 / 计数 / 摘要字段"。
