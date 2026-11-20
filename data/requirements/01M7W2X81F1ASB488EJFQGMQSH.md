# 🐞S2 plugin/load-save-e2e 偶发 gate timeout（第二道确认后 30 s 未进 FIELD；1 红 1 绿）

- id: REQ-01M7W2X81F1ASB488EJFQGMQSH
- type: bug
- status: open
- parent: REQ-01M7T0VJPG2S0RZ1HNWW0HH3HY
- repro: apps/emulator-lift/tests/plugin_e2e.cpp#plugin/load-save-e2e
- severity: S2
- tags: [plugin, test-flake, c2c, macos]

## 现象

`plugin/load-save-e2e`（`plugin-headless` 的子用例）在 macOS 上 **偶发** 红：第二道确认 click(690,323) 之后
30 s 没进 `FIELD.BIN` / `SC0000`。

读数（2026-11-20/21 / mac arm64 / HEAD `4062f05` / RelWithDebInfo）：

- **红**：`plugin : … params=slot=5 calls=7 run=finished exit=5 reason=load-save: gate timeout (load:field-2)`；
  stderr 到 `SBUNKI 0x0AF8 ⇒ second confirm` 为止。
  P1 实现者用同一产品二进制、同一命令手工重放 2 次都复现；只复制 `SAVE05.{DAT,STH}` 也复现。
- **绿**：父级复跑 `ctest -R '^plugin-headless$'` 1 次 **Passed 151 s**（同 HEAD、同 `out`）。
- 第二次复跑与 Phase A 构建抢 CPU，约 7 min 未出结果，已中止（不计入统计）。

⇒ 不是确定性回归；最可能是**负载 / 时序敏感**（30 s gate 太紧）或 mac 首次真跑暴露的既有时序缺口。

## 线索（未定因果）

- 同一流程在 `.tmp/plugin-runtime/c2c-headless3.out`（14:44）是 `calls=7 run=done`；当前产品 exe 是 15:59
  构建（含 C6 `7f47235` Vfs 负缓存）⇒ **时间相关**，但 P1 红 / 父级绿两次结果相反，不能据此归因 C6。
- 该 e2e 过去在 POSIX 是 skip（Windows-only spawn），P1 `4062f05` 才第一次在 mac 真跑
  ⇒ “从未在 mac 上稳定过”也是可能解释。

## 复现

```bash
cd apps/emulator-lift && ctest --test-dir out -R '^plugin-headless$' --output-on-failure
```

约 151 s；红时看 `plugin : …` 行与 stderr 的 second confirm。

## 判据（收口）

- 连续 ≥10 次 `ctest -R '^plugin-headless$'` 全绿（贴计数与机器负载口径）；或
- 定位到明确根因（gate 超时 / C6 负缓存 / mac 时序）并以守卫固定，不再靠时间窗。
- ⛔ 不许把 gate 超时简单加大或用 skip 掩盖 —— 先证明“状态转移本身没有丢”。
  （加时门前必须证明：确认点击确实被消费、游戏没有停在错误分辨率 / 错误存档槽。）

## 边界

- ⛔ 不在本单改 C6 的负缓存语义（若归因成立，另开 / 改 C6 单）。
- ⛔ 不动 P1 的 `runChild` 语义（除非证明超时 / 收尸有关）。
- 与 `REQ-01M7VVDV69MJXRHNDGR50H7VHC`（首回调超 2 ms 预算）不是同一处：那条是 `start` 回调预算。
