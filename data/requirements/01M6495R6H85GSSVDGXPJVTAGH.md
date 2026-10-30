# GameStart → ADV：路径上缺失的指令逐条补齐

- id: REQ-01M6495R6H85GSSVDGXPJVTAGH
- type: req
- status: done
- parent: REQ-01M5SZ3M7GCR0M23RSYBD7GEA5
- verify: apps/emulator-cxx/CMakeLists.txt#emulator-core-adv-choice-add
- done_reason: 路径已连续跑到 ADV 界面并渲染出正文：headless 端到端 walk-adv2.txt 跑到 3188 帧 / 57 秒，帧证据见 .tmp/adv/bmp5（frame-0700 夕阳背景、frame-3000 消息窗正文）与 bmp6（frame-1500 章节标题卡）。全部门禁绿。已知偏离：文本层绘制键基数在脚本未给时用宿主兜底（残差 ADV-TEXT-LAYER-DRAW-KEY）；再往后的 CALCREVISE 0x132 已在 ADV 之后，另行立单。
- tags: [emulator spec]

# GameStart → ADV：路径上缺失的指令逐条补齐

## 目标

从 TITLE 的 **Game Get Started**（はじめから）点下去，一路推进到**第 0 章 ADV 画面**（夕阳背景 + 屏幕中央的旁白正文），
途中的每一条「模拟器没有 handler」的指令都按规范补齐，并留下会红的守卫。

## 方法（可复现）

1. 用宿主输入脚本驱动（`--play-headless --input-script`），停机点就是下一条要补的指令；
   停机时 `[stack]` 给出**哪个 BIN 的哪个文件偏移**（`0x195` 那次就是这个诊断缺位才加上的）。
2. 每条先看**规范有没有这一则**：
   - 有 ⇒ 照规范实现（差异按规范，不按实现）；
   - 没有 ⇒ 先按 `AGENTS.md` §6 取证（`.lst` 逐字 + EA 锚），把规范写成合同，再实现。
3. 守卫：C++ 侧 `apps/emulator-cxx/tests/core_selftest.cpp` 一条用例 + 顶层 CTest；手动施加一处破坏确认它会红。
4. spec 侧：章节加规则 / 补条目、`bindings.json` 加站点与守卫证据、`audit.json` 加条目并把该 opcode 从 gap 的 `ops` 里移出。

## 路径（实测）

| 步骤 | 触发 | 到达 |
|---|---|---|
| 1 | boot（`--skip-logo`） | `TITLE.BIN`（id 21092） |
| 2 | 左键点 (1174,377) | `GAMESTART.BIN`（id 21099） |
| 3 | 左键点 (715,576) | `INITGAME.BIN`（id 20964）→ `UNITECH` → `CALCCC` → `CCINIT` → `SETFATE` → 返回 |
| 4 | 自动 | 根脚本 `SYSTEM4.BIN` → `call-script 115` = `SC0000.BIN`（第 0 章） |

★ 路径登记在 `data/routes/screens.json`（`pnpm tools routes list`），宿主输入脚本由 `pnpm tools routes emit` 派生。

## 当前停机点

★ 2026-10-30 进展：已进到 **`SN0000.BIN`（id 116，第 0 章剧本）**，并在 NOVEL.BIN（ADV 模块）里走通开头（`0x1f9` 载纹理 + `0x1b1`）。当前停在 `SN0000` ip **58028** → `0x215`（`call_at=3651`）。

（旧记录）

`SETWEATHER.BIN`（id 71）ip **20** → `0x32d`（argc 2，实参 `255, 16777215`）。这是根脚本在语音收尾之后**给场景设天气 / 显示参数**的一段（后面紧跟 `0x1fa` 释放纹理 62、`0x32a 0/1/2`）。

## 剩余缺口（现算）

`node .tmp/adv/need.mjs SN0000.BIN DRAWCHARM.BIN NOVEL.BIN`（临时探针：脚本用到的 opcode − 已实现）：
NOVEL.BIN 与 DRAWCHARM.BIN **已无缺口**；SN0000.BIN 还缺 **9 条**：`0x32 0x72 0x10a 0x193 0x20a 0x243 0x2f7 0x304 0x305`。

## 第二轮进展（2026-10-30）

又补 **8 条**：`0x215 / 0x242 / 0x219 / 0x220 / 0x224 / 0x7a / 0x1cb / 0x256`。
`presented` 从 256 涨到 **595 帧**（场景真的在画了），**DRAWCHARM.BIN（画角色）整支跑完**。
停机点现为 `SN0000` ip **60320** → `0x20a`。

★ `0x215` 的取证纠正了一处建模：引擎里 **`0x1fb` 才是「场景对象记录」的建记录者**（置首字 bit0 + 写整数标签），
`0x215` 读的正是那格标签 —— 已按此实现并把 `0x1fb` 接上（`0x220` 的门也靠这个位）。


## 第四轮：正文渲染打通（2026-10-30）

找到了"只有窗框没有字"的根因：**逐字符绘制条目需要一个绘制键基数**（引擎里由另一条指令给），而 ADV 的消息层
从来没有那条 ⇒ `create_text_layer_item` 一条都不建。加了**宿主兜底基数**（`kTextLayerFallbackBase`），
并把这处偏离写进规范的残差 `ADV-TEXT-LAYER-DRAW-KEY`。

证据（`.tmp/adv/bmp5`）：`frame-0700` = 夕阳背景；`frame-3000` = **消息窗里有正文**
「騎士の鬨け声を受け、修理技師たちは適当な」。`presented` 3188 帧 / 57 秒。
