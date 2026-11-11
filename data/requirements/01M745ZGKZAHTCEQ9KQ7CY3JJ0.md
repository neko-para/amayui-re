# emulator-lift · quit-tail 夹具的 at frame 锚在 boot 帧停滞后退化成墙钟 ⇒ 跨运行读数不确定

- id: REQ-01M745ZGKZAHTCEQ9KQ7CY3JJ0
- type: bug
- status: dropped
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- severity: S3
- dropped_reason: 夹具已删（2026-11-11 裁决）：quit-tail 的输入锚 at frame 在 boot 帧停滞里被折算成墙钟——这是文档化且有纯档守卫（input_tick ④ / profile_window ④）的行为，用 88 s 的 e2e + 13 min 的变异去证明"夹具自己会红"不属于守卫门槛（docs/03-emulator/README.md §0.1）。知识落到 defects.md 的 BUG-21。重开条件：出现"只能靠这套夹具看见"的静默回归。
- tags: [emulator-lift test determinism input-script]

## 一句话（原假设）
`quit-tail` 夹具的输入锚是 `at frame 130`；boot 出现帧停滞（30001 ms、`n=0`）时该锚被折算成**墙钟**触发（`waited_samples=0`）
⇒ 点不到 TITLE 的 Quit ⇒ 两轮都停在 `--time-limit 600` ⇒ 判据数由墙钟停点决定 ⇒ **跨运行不确定**。

## 读数（2026-11-11）
- 修前：`LIFT_ASSETS=1` 下 `ctest -R quit-tail` = **24 checks / 1 failure（1203 s）**，两轮都 `stop: time-limit`。
- **修后**：**43 checks / 0 failures（219.91 s）**；紧接着再跑一次 **244.98 s 也 0/0**。
  两次 `tail_insns=131104827`、`waited_samples=3`、`click frame=1472/1473`、`stop=stopped`（`kernel32!ExitProcess`）。
  ⇒ 20 分钟 → **~4 分钟**（省掉的是两臂各自烧满 600 s 的墙钟兜底）。
- 反证（修前）：换成 `at guest_ms 90000` ⇒ `stop: stopped` + `ExitProcess` + `tail_insns=131104827`。

## 已修
1. 锚 → `at guest_ms 120000`（依据：Quit 点实测 `guest_ms=87991`，+32 s ≈ 4.3 倍 boot 差）。
2. 判据加两条：**锚必须命中**（`waited_samples>0`）、**停因必须是 `stopped` + `detail` 含 `kernel32!ExitProcess`**（⛔ 不许被 `--time-limit` 兜底成"通过"）。
3. ★ **顺带揪出第二个不确定源**：`--appdata` 跨调用复用 + 退出写 `SAVE.DAT` ⇒ `tail` 131M/143M、a/b 差一帧 ⇒ 已改成每次 `remove_all`，之后连跑两次绿。
4. 变异条目 +1：锚退回 `at frame 130` ⇒ 红（变异体实测 20 checks / 5 failures、601.63 s、"锚未命中（n=0）"）。

## 没验证完（**接手从这里继续**）
- 最终变异：`✅红 + 还原基线复验`只做到前半段（还原后基线曾红，已修 appdata；改成绿之后**没有重跑该变异**）。
- 默认档 `ctest` **81/81 未实跑**（默认 configure 已恢复、`ctest -N` = 81、`cmake --build` = no work to do）。
- `pnpm test` / `pnpm tools lift check` / 独立复审 subagent **未跑**。
- `report.md` 只写到"读数 + 变异前半段"。

## 交接注意
- ⛔ **不要整文件 revert `quit_tail.cpp`**：它同时含上一单的**路径编码修复**（`wstring` + `CreateProcessW`，实测 12/7 → 24/1）。
- ⛔ **不要整文件 revert `mutate-check.mjs`**：含上一单的 `optInEnv` 第三态。
- 该测试是 **opt-in**（`LIFT_ASSETS=1` + 重新 configure），跑完要**恢复默认 configure**（`ctest -N` 应回 81）。
- 建议：把 `--time-limit 600`（现在只是兜底上限）降到 **180 s**，让未来任何锚失效 3 分钟就红而不是 10 分钟/臂。

## 未做
- 不改 `--input-script` 语法（`at guest_ms` 已存在）。
- `movie-logo` 的 `stop: time-limit` 同源嫌疑另查。
