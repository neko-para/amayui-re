# emulator-lift · 其余两处"折叠成成功"：ExitProcess(n) 丢 n · LZSS override 静默截断

- id: REQ-01M77CMJGJ9EJVNPFS2TKMMHD1
- type: bug
- status: open
- parent: REQ-01M75S724HE50D26EEMDNY3Z54
- repro: apps/emulator-lift/src/runtime/native_lzss.cpp#in >= in_end
- severity: S1
- tags: [emulator-lift, diagnosability]

## 一句话
`REQ-01M75S724HE50D26EEMDNY3Z54` 的三处"把失败折叠成成功/看不见"里，**MessageBox 一条已由本轮落地**
（自定义模态对话框 + 正文 warn + 按 uType 返回）；本单只收剩下两处（同一类失效方式）：
1. `ExitProcess(n)` 的 `n` 被丢弃 ⇒ 一切非 0 退出伪装成 0；
2. LZSS native override 静默截断却 `return true`。

## 已核事实（沿用父单，未变）
- `n` 只进 `cpu.shim.exit_code`（`x86.h:152`），从不参与进程退出码；`main.cpp` 只看 `ok()`。
- `src/runtime/native_lzss.cpp:30-56`：三处 `in >= in_end` ⇒ `break`，无条件 `return true`；不检查 `in == in_end` / `out == out_end`；
  只走 `--native-overrides all`。
- 父单的 MessageBox 条：正文升 warn + 按 uType 返回 + 真对话框 ⇒ 本轮已落（`tests/messagebox.cpp`、`tests/recovery_dialog.cpp`）。

## 要做
1. `ExitProcess(n)` 且 `n != 0` ⇒ 进程退出码非 0（建议 2，与 decode-error 同档），`n` 进日志；`n == 0` 保持 0；
   ⛔ 不把 guest 的任意 `n` 直接当宿主退出码。
2. `nativeLzssDecode`：`break` 后区分"正常结束（`out == out_end`）⇒ true"与"输入提前耗尽/输出没填满 ⇒ false"，
   让调用方回退解释器；⛔ 不要用"剩余填 0 + true"糊。
3. 守卫：
   - ① 用例：guest 调 `ExitProcess(3)` ⇒ 进程退出码非 0；`ExitProcess(0)` ⇒ 0。
   - ③ 用例：喂**被截断**的压缩流 ⇒ `nativeLzssDecode` 返回 false；合法流 ⇒ true 且输出逐字节正确（期望值用**解释器臂**算，⛔ 不手写参考实现）。
4. 门禁：`ctest` / `pnpm test` / `pnpm verify` / requirements；`git diff --check`。

## 不做
- MessageBox（父单已收）；`FindFirstFile` / `GetSystemMetrics` / 菜单族等其它"折叠"另单。
