# 把 C 单的语义取证登记进知识层：CORE-SCENE-RESET 新节 + 音频源接口虚方法族 + flip pool bank 重填（每条过 §6 准入门）

- id: REQ-01M718T18NYQDGJP71HE2XNE97
- type: req
- status: dropped
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- dropped_reason: spec 层短期冻结（2026-11-10 用户口径）：data/spec/ / data/ledger/ 暂不登记新语义 —— 不新增规则、不修订、不复审。本单是往 spec 登记 CORE-SCENE-RESET 等语义，冻结期间不做；来源的语义取证仍留在 REQ-01M6XVZ8AXJNG492WA2QAMTA35 正文与 git 历史，重启 spec 时整体重审后再登记。
- tags: [spec, ledger, shape, audio]

## 判据（存在性 + 观察）

`REQ-01M6XVZ8AXJNG492WA2QAMTA35` 的语义取证（`.tmp/reveq/<该单>/semantics.md`，每条带 EA）产出了**三条该进知识层的东西**，
但那张单是为了性能归因，⛔ 不该顺手往 `data/spec/` / `data/ledger/` 写（`AGENTS.md` §6 准入门：每条都要绑可再校验观察，且是**独立的一次登记**）。

| # | 要登记什么 | 建议落点 | 证据（已在报告里带 EA） |
|---|---|---|---|
| 1 | **"结束当前场景 + 重装根脚本（统一文件 id 0）"这个例程**（`sub_428A60`） | `data/spec/` 新节，建议 rule id `CORE-SCENE-RESET`（落 `15-frame.md` 之后） | 顺序：`[this+0xA537C]` 回调 → 清标志 → **40 次 `sub_40EA00`** → 读 `set:EnableMemFlip` → **bank 复位（40 环）** → `SendMessageA(hWnd,0x1400,1,1)` → `set:DrawMode` → `set:CreateObject` bit0/bit1、`set:AutoFreeTex` bit0 → **1000 次 `sub_49E980`（带释放守卫）** → `sub_40ED40(this, ?, 服务窗口, 0)` → 0x428D9C 抛退出型异常。与 `CORE-BOOT-ROOT-SCRIPT`（同装载器 id 0）首尾相接 |
| 2 | **音频源接口的虚方法族**（解码出 int16 PCM） | `docs/02-engine/` 或 audit 残差（spec 正文边界：`00-overview` 已把"音频输出"列为未建模） | `OggStream` 0x528FD0 / `OggVorbis` 0x52917C 共用第 3 项 0x528FD8/0x529184 → 0x48D610；`dbl_5291B8=32767.0`、`"Clipping in frame %ld"`、`ReadFile 4096/次`、饱和 `7FFFh`/`FFFF8000h`；同虚表第 4/5 项 = 输出缓冲地址/字节数 reader |
| 3 | **flip pool bank 指针数组重填**（8 指令/圈、圈数 = `this+0x5D7E8`+1、唯一写点 0x41515D 从 `this+0xAAB2C` 抄） | `data/shape/`（对象前缀）+ 上述 1 的正文 | 12 段 × 3 遍块表恒等（`00428AD0` 100%、`iters=641,875` 三遍一字不差、`edges` 空）；吞吐 34–44 M/s 与 interp 同量级 |

- 判据：三条各自**过准入门**（每条结论绑至少一条可再校验观察：EA + 指令/取值，或守卫用例 id）；`pnpm tools spec validate` 仍绿；
  `pnpm tools requirements validate` 仍绿。⛔ 不许把 `.tmp/` 的报告当登记（`.tmp/` 不入库）。
- 另：报告里点名的 4 个 `set:` 键**已在** `format/config-keys.json`（kind=int），缺的是"**按位**用法" ⇒ 写进 #1 的正文，⛔ 别再复制第二份 schema。

## 为什么单独成单

- 知识登记要**独立裁决**（每条都要能被独立复核），而性能单的判据是"段 → 归因表"。混在一起会让"性能归因做完了"和"知识登记做完了"红在一起。
- 而且 #1 一旦成节，就要处理它与 `CORE-BOOT-ROOT-SCRIPT` / `CORE-BOOT-LOOP-EXC` 的**交叉引用**（报告已点名"首尾相接"）。

## 修法边界

- ⛔ 不许把 `sub_428A60` 的直接调用者 `0x476590` thunk / IAGEService 虚表第 43 项这类**实现细节**写进 spec 正文（spec 只写领域行为）。
- ⛔ 不许凭报告里的 `<待核>` 项（`this+0x5D7E8` 的运行时值、PCM 之后谁消费、`[this+0x5EB54]` 是否窗口句柄）落结论。
- 登记前先 `pnpm tools spec describe` 看字段与证据档要求；`:knowledge-source` 素材仍只能 `external-only`/`deferred`（`AGENTS.md` §6 守卫 #8）。

## 与别的单的关系

- 来源：`REQ-01M6XVZ8AXJNG492WA2QAMTA35`（已收口，性能归因）。
- 相邻：`CORE-BOOT-ROOT-SCRIPT` 所属的启动链一批单 · `REQ-01M6WW9V9N863FDTMK6BRMDK1K`（TITLE 工作集，同为"测得准"而非"知道什么"）。
