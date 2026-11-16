# 配方改 position-based：为各屏取证可用位置区间，逐步去掉 wait <ms> 推进

- id: REQ-01M7HTXBADJC7R5CHKRJ1MMADG
- type: req
- status: open
- parent: REQ-01M7G7EJF062NKQ0D40RF3BZ4Z
- tags: [emulator-lift, input, position, recipes]

## 一句话

6 份配方（`apps/emulator-lift/scripts/recipes/*.plan`）现在是"**`await script` 门 + `wait <ms>` 推进**"的模式；
用户口径（2026-11-16）：后面要用**真正的 position-based** 重写，而不是基于 `wait ms`。

## 为什么（已有读数）

- `wait <ms>` 是**宿主墙钟**，而 guest 推进不是墙钟 ⇒ 同一配方**不可复现**（实测 12 跑 4 成，见 `OBS-20261116-04` 与 `input-plan.md` §2.6）。
- `await position` 是**guest 可见的确定量**（脚本内绝对偏移）⇒ 同一配方多次 run 逐位相同（`OBS-20261116-05`：4 次一致）。
- 今天 `position` 只有 **TITLE 输入循环**那一段的可用区间（`[0x0460,0x0520)` = 点击会被消费的窗口）。

## 缺什么（本单的真正工作 = 取证）

逐屏给出"**可用位置区间**"，格式同 `TITLE.BIN:[0x0460,0x0520)`（半开、hex、4 字节对齐）：

| 屏 | 现在的写法 | 需要的区间 |
|---|---|---|
| TITLE | `await script TITLE` + `wait 5000` | ✅ 已有 `[0x0460,0x0520)` |
| GAMESTART | `await script GAMESTART` + `wait 2000` | ❌ 缺 |
| SN0000 | `await script SN0000` + `wait 25000` | ❌ 缺 |
| FELLOW | `await script FELLOW` + `wait 2000` | ❌ 缺（那一屏还要穿过 ~7–17 s 的同步长活） |
| SAVE | `await script SAVE` + `wait 2000` | ❌ 缺 |
| 加载存档界面 | 无（旧 `title-loaddata` 用 click 后等） | ❌ 缺 |

## 判据（收口时）

1. 每份配方**去掉全部 `wait <ms>`** 后，跑到同一个收尾读数（`region` 三数 + 步进摘要），`fired`/`pending` 一致。
2. 同一配方连跑 3 次逐位相同（现 `wait` 版做不到）。
3. `wait` 的残留只允许出现在**真·时间语义**处（例如"等音频喂完"），且每处写明理由；否则归零。
4. 位置谓词只对**同一份脚本字节**成立 ⇒ 带 `script_sha`（汉化 `insert-after` 会移偏移）。

## 未做

不实现；先取证区间（每屏一条 EA 锚）。`timeout` 内联写法是另一单，不冲突。
