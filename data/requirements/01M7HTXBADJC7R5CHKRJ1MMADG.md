# 配方改 position-based：为各屏取证可用位置区间，逐步去掉 wait <ms> 推进

- id: REQ-01M7HTXBADJC7R5CHKRJ1MMADG
- type: req
- status: doing
- parent: REQ-01M7G7EJF062NKQ0D40RF3BZ4Z
- tags: [emulator-lift, input, position, recipes]

## 一句话

6 份配方（`apps/emulator-lift/scripts/recipes/*.plan`）现在是"**`await script` 门 + `wait <ms>` 推进**"的模式；
用户口径（2026-11-16）：后面要用**真正的 position-based** 重写，而不是基于 `wait ms`。

## 为什么（已有读数）

- `wait <ms>` 是**宿主墙钟**，而 guest 推进不是墙钟 ⇒ 同一配方**不可复现**（实测 12 跑 4 成，见 `OBS-20261116-04` 与 `input-plan.md` §2.6）。
- `await position` 是**guest 可见的确定量**（脚本内绝对偏移）⇒ 同一配方多次 run 逐位相同（`OBS-20261116-05`：4 次一致）。

## 缺什么（本单的真正工作 = 取证）

逐屏给出"**可用位置区间**"，格式同 `TITLE.BIN:[0x0460,0x0520)`（半开、hex、4 字节对齐）：

| 屏 | 现在的写法 | 需要的区间 |
|---|---|---|
| TITLE | `await script TITLE` + `wait 5000` | ✅ 已有 `[0x0460,0x0520)` |
| GAMESTART | `await script GAMESTART` + `wait 2000` | ❌ 缺（候选 `[0x0664,0x0724)` 待核） |
| SN0000 | `await script SN0000` + `wait 25000` | ❌ 缺（**无 mouse-callback** ⇒ 只能粗区间或逐 `wait-for-input` 窗口） |
| FELLOW | `await script FELLOW` + `wait 2000` | ❌ 缺（候选 `[0x15A8,0x16A4)` 待核） |
| SAVE | `await script SAVE` + `wait 2000` | ❌ 缺（候选 `[0x1104,0x11C4)` 待核） |
| 加载存档界面 | 无 | ❌ 缺，且**位置门本身可能不够**（见下） |

## 2026-11-16 补：取证起点与三条硬结论

- ★ 起点不是空白：候选区间 + 依据 + 点击链 + 9 步采样清单在 **`repo:docs/03-emulator/scripts/_recipe-map.md`**；
  TITLE 的正式区间在 `repo:docs/03-emulator/scripts/TITLE.md`（四条读数，含两条负对照）。格式与口径见同目录 `README.md`。
- ★ **行号 ≠ 字序号**：清单每行占 `1 + 2×操作数个数` 个字，`label = 0x3C + 字序号×4`（5 份清单 1491 个 label 零偏差）
  ⇒ 拿行号乘 4 算区间会**静默偏**。
- ★ **`SAVE.BIN` 上的位置门区分不了「保存」与「加载」两屏**：两者共用同一脚本
  （`SAVE.BIN.txt:901` `ロードしますか？` vs `:954` `セーブしますか？`），真正的区分量是 `:898`/`:951` 的 `global a9cb` 位。
  ⇒ 加载屏**不能只靠 position 门**：要么加"取 global"的第三条门（另立单），要么换一个 guest 可见判据。⛔ 不许用 `wait` 兜。
- ★ **加载存档的运行环境可以外部组装**：槽号**只在文件名里**（头里没有槽号，EA `0x42D980` / `0x438120`）⇒ `SAVE76.*` 改名成
  `SAVE00.*` 成立；一页 10 项、页基初值 = `SAVE.DAT` 里持久化的"上次槽号 / 10 × 10" ⇒ 组出 `00..09` 后**第一页必有项**。
  逐条命令与 EA 锚见 `repo:docs/03-emulator/save-format.md`（含 `--appdata` 该指哪一层、翻页 = 横向滚轮）。
- ⚠ `title-loaddata.plan` **不是**一条"到达某屏"的配方，它是**缺陷复现件**（点击没被消费 ⇒ 进程干净退出）⇒
  它的 `wait 20000` 属于"真·时间语义"，⛔ 不要为了"归零 wait"把它硬改成 position 门（该屏的新配方另立）。

## 判据（收口时）

1. 每份配方**去掉全部 `wait <ms>`** 后，跑到同一个收尾读数（`region` 三数 + 步进摘要），`fired`/`pending` 一致。
2. 同一配方连跑 3 次逐位相同（现 `wait` 版做不到）。
3. `wait` 的残留只允许出现在**真·时间语义**处（等音频喂完 / 缺陷复现件），且每处写明理由；否则归零。
4. 位置谓词只对**同一份脚本字节**成立 ⇒ 带 `script_sha`（汉化 `insert-after` 会移偏移）。
5. ★ **每条门要有负对照**：换屏之后 pos **必须离开**该区间（同一区间在两屏都成立的"门"是无效门，⛔ 不许登记）。

## 2026-11-16 进展：前缀已落地，尾巴卡在"未取证"

- ✅ **已改成位置门的配方**：`se-eof` / `sn0000`（全量）+ `sn0000-quit` / `sn0000-save` / `sn0000-save-profile`（**只改前缀**）。
  门命中率：`TITLE 0x0460 0x0520` **15/15** · `GAMESTART 0x0664 0x0724` **15/15** · `SN0000 页1 0x46B8 0x470C` **12/12** · 页2/页3 **3/3**。
- ✅ **负对照成立**：把 TITLE 门换成该屏永不执行的 `[0x388C,0x38A4)` ⇒ `fired=0` + `pending=5` + **exit 5** 且点名
  `step1（行 5，门）`；`stop` 仍是 `max-frames`（⛔ 不是 `--time-limit` 到期），`region` = TITLE.md 的基线档。
- ✅ **5 条 `repro:` 点击行逐字节未变**（30 格 md5 全 SAME，全仓 130 条 `repro:` broken=0）。
- ⚠ **`sn0000-save` 达不到 3/3 逐位相同**：3 次里 2 次卡在**没取证的尾巴** `await script SAVE`（那一段本轮按纪律原样保留）；
  卡住的 run 也走完了前缀 10 步 ⇒ 不是新门不稳。`sn0000-quit`（`await script TITLE`）/ `sn0000-save-profile`（`await script SAVE`）同理。
- ★ **新的阻塞点（下一单要先回答）**：**FELLOW 屏什么时候才吃 `click 100 140`** —— 在页 1 停点门后额外加 `wait 12000`
  再右键，3 次仍全卡在 `await script SAVE` ⇒ 排除"页 1 语音没播完"这一个解释。累积 8 次尝试只有 2 次进 SAVE。
- ⚠ 改前基线（同一产物）：三份长配方**根本没到 FELLOW**（headless 卡 `await script GAMESTART` / `await script SN0000`）⇒ 改后是严格变好。

## 未做

- **FELLOW / SAVE / 加载屏** 的区间（③-4 / ③-5 / ③-6）；`sn0000-quit` 的"回 TITLE"；SN0000 页 4+；`script_sha`（另一单）。
- 加载屏**不能只靠位置门**：它与保存屏共用 `SAVE.BIN`，区分量是 `global a9cb` ⇒ 见 `REQ-01M7J97QY66KMP5SBPCTR0S3TP`（第三条门）。
- `timeout` 内联写法是另一单，不冲突。
