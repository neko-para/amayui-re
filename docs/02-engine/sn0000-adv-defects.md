# SN0000 序章 ADV 两个缺陷：复现、定位与处置

范围：`apps/emulator-cxx`（维护中的模拟器）。`--play-headless` 是宿主测试装置，不是引擎语义。
复现基线：`--install links/install --skip-logo --play-headless --anchor-script 21092 --tick-ms 16`。

## 复现输入

`route-sn0000.txt` 只到 116；进入 SN0000 后还需要一次点击才会加载正文。探针脚本 =
`route-sn0000.txt` + `4000 down 0` / `4100 up 0`（存在 `.cache/probe-one.txt`，不进仓库）。

```bash
apps/emulator-cxx/out/emulator-cxx --install links/install --skip-logo --play-headless \
  --input-script .cache/probe-one.txt --anchor-script 21092 --max-steps 4500000 --tick-ms 16
```

`0xBF`（BGM）前有一个 10 秒的 BGM 淡变（第 9 位）；约 1.9 万步后才出现正文。
`--bmp` 与 `--expect-script 116` 同用会在到达 116 时提前收口；要抓正文帧用 `--expect-script 0`
（或 `--max-steps`）并把 `--bmp-stride` 调小。

## 缺陷 1：第一句加载完自动多推进一轮（已修）

**现象**：点击进入 SN0000 后，第一段正文加载完，第一条 `0x72`（等推进）不等待点击就放行；
第二段正文加载后才停下等点击。

**根因（spec + 机器）**：`60-ops-input.md:872` 规定 `0x72` **起播时**（跳过位 27 未置）先做一次
事件并入并把输入字整字清 0；`60-ops-input.md:55/858-859/892-894` 说输入是事件锁存、消费者读一次就清。
机器：`0x41F04C call sub_478090` + `0x41F055 mov dword ptr [ebx], 0`（清输入字）在
`0x41F07B or [..+0AAB44h], 80000000h`（置第 31 位）**之前**。模拟器 `op_72` 原来没有这一步 ⇒
TITLE 的 GAMESTART 点击留下的输入锁存被第一条 `0x72` 直接消费，第一句自动过。

**修法**：`op_72` 在置第 31 位前，若第 27 位未置则清 `advance_input_latch_`（= 清输入字）；
`sample_advance_input()` 保持原样（任何上升沿都锁存，事件锁存语义不变）。

**守卫**：`core_selftest.cpp` 的 `check_adv_wait()` ⑤（`--only-adv-wait` / CTest
`emulator-core-adv-wait`）。旧代码会红：
`adv-prewait：位 31 置起之前的点击把第一条消息放行了`。

**实测**：同一探针修复前 `text-appends=9`（第一段自动过、第二段加载），修复后 `text-appends=6`
且最终停在 `opcode=0x72`（第一条消息的等待点）。

## 缺陷 2：正文在左上角，真机在正中心（已实现 `0x303` 对齐，待视觉确认）

**真值**：真机（原版 + 中文 patch；patch 不改表现）里正文在**正中心**；模拟器在左上角。
`62-adv-text.md:237/283` 的「引擎只有左对齐、没有居中」是**错的/不完整**；以真机为准。

**根因（spec + 机器）**：居中不在 `0x6e` / `0x79` / 层左/上里，而在**绘制期**读层记录的
「对位模式 +0x120 / 基准 +0x124」，由 **`0x303`** 写：

- `0x303 group=8 mode=1 base=500`（SYSTEM4.BIN 启动脚本；`op_303` 写 f120/f124）。
- x 读者 `sub_4574A0`：`0x45754F..0x45756C` ⇒ `x = base − (max−min)/2`（模式 1）；
  模式 2 ⇒ `base − (max−min)`（右边贴 base）。
- y 读者 `sub_4575B0`（`sub_45FF00` 调用）：同一对格，`y = base − (max−min)/2`。
- 消费点 `sub_469260`：`0x46A175 call sub_4574A0` 成功后把居中 x 乘适配比、交给绘制条目；
  只有失败时才回退到「层左/上 + 字符格」（`0x46A1FD/0x46A200/0x46A241`）。
- 模拟器 `op_303` 只写 `f120/f124`（`vm_ops_engine.cpp:820-821`），**全仓无读点**；
  正文仍走「层左/上 + 布局字符格」，所以是 (140,16)。这不是 `op_70/79/198` 的算术错，
  也不是层 1 的选择问题。

**已实现（2026-11-02，x 逐行平移）**：`align_text_layer()` 按 `layer.cells` 的 **y 归组**（振り仮名
`span_exclude` 不参与），种子 min = 层宽拷贝 / max = 文本原点；模式 1 `Δ = 基准 − trunc(跨度/2)`、
模式 2 `Δ = 基准 − 右界 + 左界`、模式 0 无 Δ、其它 `Δ = 0`；只改绘制项 `aux[0]`（`aux[1]` 跟层上），
**不写回** `f0c/f10`；`0x303` / `0x70` / `0x198` 写入后重算；`0x71` 不清对齐对。
守卫：`--only-text-layer-align` / CTest `emulator-core-text-layer-align`（单行行心 120 = 层左 0 + 原点 20
+ 基准 100；两行各自 120；模式 2 右界 120；模式 0 / 1→0 无残留；`0x303` 后置；`0x198` 的 x/y）。
★ 机器的事实：Δ 是**平移**，叠在绘制项自己的位置上；行心 = 层左 + 起点 + 基准。SN0000 组 8 原点 140、
层左 0、基准 500 ⇒ 行心 **640 = 500 + 140**，适配比 1.0，不是 500。
**仍待**（`REQ-01M6E2P82N8M4S07QDTX1MXTDC`）：y 读者（`sub_4575B0` / `sub_4573A0`）/ 120B 行记录日志；
`0x70` 画布 `f7c/f80` 与绘制项位置 / 源矩形 / Δ 的适配比；`0x300` 重放后的重对齐。

## 门禁

- `--only-adv-wait`：PASS
- CTest 103/103：PASS
- `pnpm test` / `spec validate` / `requirements validate` / `routes validate`：见本轮输出。


## 复审（2026-11-02，三份独立 subagent）

输入：staged 的 emulator 改动 + `data/spec/` + `.lst`；报告在 `.tmp/adv-review/`。

- `62:237/283` 的「引擎只有左对齐、⛔ 不许居中」与 `60-ops-list` 的 0x303 及机器冲突；已改成
  「**正文 run 的排版**只有左对齐；绘制期另有逐行平移」。
- 对齐是**平移**、逐行、有跳过掩码 `0x6000000E` 与两个种子（min = 层宽拷贝、max = 文本原点）；
  三条序列（行记录日志 / 布局字符格 / 定位记录）都读同一对。新规则 `ADV-TEXT-DRAW-POSITION` 落这些。
- 适配比是**字体对象**的一对（视口 ÷ 配置客户区），只在窗口态 + 虚拟全屏类型 2 重算；`0x70` 画布 /
  绘制项位置 / Δ 用它；场景章「显示适配」的 `s` 是另一对。
- 原「(500,500)→(640,360) 由适配比给出」**不成立**：640 = 原点 140 + 基准 500（适配比 1.0）；
  x 读者把 y 出参写 0、y 读者把 x 出参写 0，两条不同时发生，360 不是基准给的。
- `0x72` 是**事件并入**（并入挂起事件 + 摇杆边沿轮询一次）后整字清 0；跳过位已置时整段不做
  （第 31 / 30 位都不置）。staged 的 `op_72` 只清了推进锁存，已补清 `input_word_` / 两个事件锁存，
  并补 skip 支守卫（`adv-wait` ⑥ / ⑦；变异「把清字移到 bit27 判断之前」实测红）。
