# emulator-lift · 每帧固定税：0x4BBAB0（DirectSound 槽管理器）空转 79 s 内 2.77M 次调用（3 槽轮询）

- id: REQ-01M6XVZF3VDC5NSBJ20J2451VJ
- type: req
- status: done
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- order: 6
- verify: apps/emulator-lift/tests/voice_poll.cpp#lift-voice-poll
- done_reason: A 案已落地：0x4BBAB0 的 NativeOverride 只接管「三槽全惰性」纯查询支（只读 7 dword、零写、*result=0），其余回退解释器；补偿 k=60。守卫 lift-voice-poll 13 组/134 断言（变异：放宽 active 判据 ⇒ 29 红）。A/B（RelWithDebInfo，N=3 交错，只差 --native-overrides all vs all,-sub_4BBAB0）：hits 274,541/274,685/274,505 且 fast_hits==hits=100%、declines=[]；被替代 16.47M insn = 总 insns 1.41%；墙钟 A 中位 1.15% 快但组内散布 ≥ 差值 ⇒ 测不出可靠方向性；像素 15 张 BMP 逐字节相同。判据订正：原写「调用次数下降」与设计不符，改成「被替代指令数 + 墙钟 + 像素」。
- tags: [emulator-lift, performance]

## 观测

空转（无输入、不换画面）时 `0x4BBAB0` 在 79 s 内被调用 **2.77M 次**，每次轮询 3 个槽。它每帧都交、与画面和输入都无关 —— 这是**固定税**，不是某个装载帧的热点。

## 要回答的三件事

1. 这 2.77M 次里，多少是**引擎语义必需**的（真的会改变某个 guest 可观察状态），多少只是查询？
2. 如果只是查询且结果不变，能否用"值变了才算"的路径消掉宿主侧开销 —— ⛔ 但 guest 侧观察到的**返回值序列必须逐次相同**（这是判据，不是优化目标）。
3. 3 个槽的轮询顺序与次数是否本身承载语义（例如轮询推进了游标 / 计数器）。

## 判据

- 基线：同一空转脚本 + profiler 窗口，调用次数与自耗毫秒（可复跑、落文件）。
- 改后：调用次数下降，且**空转 79 s 的终态与像素与改前相同**（A/B 工具见子单 C / E）。
- ⛔ 不许靠"少查一次"改变 guest 可见结果；拿不准就**不改**，把结论写在这里。

## 不做

- ⛔ 不与 F2（令牌等待）混在一起：一个是"查得太勤"，一个是"等锁"。
- ⛔ 不动音频输出的可听性（音量 / 时序）。

## 取证与设计已就绪（2026-11-09，只读，**未实施**）

- **身份**：`0x4BBAB0` = `Voice::Tick(now)`（`??_7Voice@@6B@ @0x52E060`，非虚，唯一调用点）；`ecx = T_Engine+0x148A0`，
  1 个栈参 = ms；3 个槽 ↔ 声道 12/13/14。
- **路径**：`_WinMain@16`(0x4BA890) → `sub_412290` 主循环帧体头 `0x4124C8`，**每迭代一次**（同窗口 Present=0），
  不是定时器、不是音频 feeder；2.77M ≈ 35k/s。
- **顺序/次数**：**无游标 / 无计数器 / 不碰全局**（`0x4BBAC7` 清 ArgList；`0x4BBB19` 只把内层扫描 OR 进布尔）⇒ 扫描顺序无关；
  但**跨槽顺序在"有写"时**会影响后处理槽的内层扫描（Stop 清 `[obj+0x2460]`、Play 置 1）⇒ 有写时不许跳读 / 换序。
- **返回值**：`EAX=1` ⇔ 真启动了一个槽；调用方立刻 `sub_407120` + 写 `0x77A14/1C/20` ⇒ 序列不可省。稳态调用是纯查询。
- **设计（A 案，先做）**：`NativeOverride 0x4BBAB0`（thiscall/4B）+ **stateless 判定**：读 12~15 dword、零写零可变调用 ⇒ `*result=0`；
  有写 / 未到期 / `dev==0` ⇒ `return false` 回退解释器（⛔ 不重写 `sub_4BB840`）。**必须时钟补偿** `tick(k-1)`（k=60），否则 `now` 这个输入序列本身变了。
- **B 案（"值变了才算"的 memo）只值 ~0.2% 墙钟**，且硬前提"feeder 不写 `T_Voice+0x418..0x490`"**未取证** ⇒ 不做。
- **守卫**：pure CTest `lift-voice-poll` 8 条（全惰性零写 / 未到期 / 到期必须回退 / `C==0` 必须回退 / 纯查询支 / 时钟恰好 +60 tick / 无设备回退 …）
  + 7 个变异点；非重言的一半只能在 `@env assets` 做（相同 insn 数下内存 hash + 帧 hash 差分）。
- **诚实项**：这 2.77M 里"语义必需"的**逐次分类数字**本轮给不出（原始 profile artifacts 已不在，且不许跑）——只给结构判据与取数口
  （`fast_hits` / `declines[]`）。副产品：`T_Voice` 实体 ≥`0x490`（shape 记 `0x484` 且缺 `0x484` 那张表）⇒ 建议挂 shape 残差。
- 分析全文 `.tmp/reveq/01M6XVZF3VDC5NSBJ20J2451VJ/analysis.md`（**不入库**；入库的是本节要点）。

## 收口（2026-11-10）：A 案已落地 + A/B 读数 + 一处判据订正

- **实现**：`repo:apps/emulator-lift/src/runtime/native_voice.cpp`（纯核 `voiceTickIsPureQuery` + `nativeVoiceTick4BBAB0`）注册进
  `kKnown`（thiscall / 4 B，`kMax 12→13`）；只接管"**三槽全惰性**"的纯查询支（`*result=0`、**只读 7 个 dword、零写**），
  内层扫描 / 已启动 / 首 tick / 未到期**与**到期 / 无设备 / 读不到 ⇒ 一律回退解释器。时钟 + insns 补偿 **k=60**（fn 补 59，dispatcher 补 1）。
- **守卫**：pure CTest `lift-voice-poll`（顶层 `add_test`）**13 组 / 134 断言**；变异一条（把 `active==0` 放宽成 `active<2`）⇒ **29 条断言红**。
- **判据订正（父级裁决）**：原判据写"改后**调用次数下降**"——那与设计不符：**调用次数不变**，省的是每次被替代的 60 条解释指令。
  ⇒ 计量口径改成"被替代的指令数 + 墙钟 + 像素等价"，本条不再是"次数下降"。
- **A/B 读数**（RelWithDebInfo，两条命令只差 `--native-overrides all` vs `all,-sub_4BBAB0`，交错 N=3，`at frame 600 / wait 2900 frames / quit`）：
  - `sub_4BBAB0` 进入 **274,541 / 274,685 / 274,505** 次，**`fast_hits == hits`（100.00%）、`declines=[]`（8 桶全 0）**；
    被替代 = 274,541×60 = **16.47 M 条 = 总 insns 的 1.41%**。
  - 墙钟 A 中位 111,881 ms vs B 113,166 ms（A 快 1.15%，CPU 0.77%）—— 但**组内散布 ≥ 差值、3 对只 2 对同向 ⇒ 本机测不出可靠方向性**（机制预测正是 0.5–1% 量级）。
  - **像素逐字节 0 差**：15 张 BMP（含 boot 帧）同一 sha256。⚠ TITLE 空转是静态画面 ⇒ 只算"无可见差异"。
  - 交叉核对：A 的 `native-ovr` 总 hits − B 的 149,221（三遍逐位相同）= 逐遍 `sub_4BBAB0` hits ⇒ 两臂**只差这一项**。
- **残差**（不要当"没做"重复记）：① 墙钟方向性要 N=8–10 或 ~5 min/遍空转才能坐实；② 单里 2.77M / 79 s 的原窗口**复现不出**
  （本窗口 ≈1.5k 次/s），可传输口径是**比例（本窗口 100% 纯查询）**而不是绝对次数；③ 函数级自耗毫秒必须开 `--profile`
  （profiler 有膨胀，⛔ 不能当基线）；④ `T_Voice` 实体 ≥`0x490`（shape 记 `0x484`）⇒ 归 shape 侧，本单未动 `data/shape/`。
