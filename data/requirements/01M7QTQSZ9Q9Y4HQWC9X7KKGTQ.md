# 🐞S2 win32：lift-gen 构建期输出在 936 终端乱码（改纯 ASCII）+ initConsoleUtf8 误判 stdout

- id: REQ-01M7QTQSZ9Q9Y4HQWC9X7KKGTQ
- type: bug
- status: done
- parent: REQ-01M6JFZFN15MBPG7Y89WNZACFW
- verify: apps/emulator-lift/tests/lift_gen_ascii_output.cpp#gen-ascii-output
- repro: apps/emulator-lift/tests/lift_gen_ascii_output.cpp#gen-ascii-output
- severity: S2
- done_reason: lift_gen.cpp 全部输出改 ASCII（--help 2183 个非 ASCII → 0）+ initConsoleUtf8 判据改 GetConsoleMode(stdout)；守卫 gen-ascii-output 改前会红（offset 18, 0xE2）、变异实测红。全量 ctest 119/119。
- tags: [emulator-lift, win32, encoding, build]

## 现象（用户口径 2026-11-19）

默认中文 Windows 终端（chcp 936）里 `lift-gen` / `emulator-lift-aot` 打印中文乱码。

## 根因（分两条，`analysis.md` / `repro.md` 有逐路径字节读数）

- **`lift-gen` 在构建期必然乱**：它是构建期工具，被 ninja 调用；ninja 1.13.2 **捕获子进程输出、命令结束后
  才回放**，那时子进程已把 console 码页恢复成 936 ⇒ UTF-8 被 936 解（`chcp 65001` 后同一条 ninja 正确）。
  构建日志的消费方编码不可知 ⇒ 只许 ASCII。
- **`emulator-lift-aot` 直接附着 console 是正确**的（码页 936→65001→936、屏幕缓冲区逐字正确）；只有输出被
  **重定向 / 管道**接走时才乱 —— 那时字节是 UTF-8 才是对的（机器消费的字节不因终端而变）。
- 附带一个真 bug：`initConsoleUtf8()` 旧判据 `GetConsoleOutputCP() != 0` 只说明"进程挂着 console"，
  **不说明 stdout 是那个 console**；`stdout→文件` 时它照样把**共享 console** 的码页拨到 65001 再拨回
  （实测序列 `936,65001,936`），既是副作用，又救不了任何乱码场景。

## 落地（2026-11-19）

- `src/aot/lift_gen.cpp`：**所有会打印的**字符串改 ASCII（usage / 参数错 / `--check` / S7 / D14 / D15 /
  D17 / D21 / D26 / `--verify-*` / 样例 tag / 生成物头注释）；注释中文未动。
- `src/runtime/host_encoding.cpp`：判据改 `GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE))`
  （stdout 真是 console 才切码页；RAII 恢复不变）。重定向下不再动共享码页。
- 产品面（`emulator-lift-aot` / `closure`）的中文**保留**：console 下是对的，改成英文要动 ≈650 处字面量
  且会撞多处"输出逐字不变"的既有约定 ⇒ 用户裁决"若没有简单办法再改成英文"，此处有简单办法。
- 守卫：CTest `gen-ascii-output`（`tests/lift_gen_ascii_output.cpp`，运行时、7 条无资产路径、stdout/stderr
  **重定向到文件**——⛔ 不用管道，§5.1 ① 有 `spawn EPERM` 先例，逐字节断言 < 0x80）。

## 读数（2026-11-19 / win32 / clang-cl / RelWithDebInfo）

- `lift-gen --help`：改前 4053 B / **2183 个非 ASCII**（offset 18 = `E2 80 94` 的 `——`）→ 改后 4295 B / **0**；
  无参 4396 B / 0（exit 2）。
- 码页：stdout=console `936,65001,936`（不变）；stdout→文件 改前 `936,65001,936` → 改后 **`936`**。
- 守卫"改前会红"：临时改回中文 ⇒ `FAIL … offset 18, 0xE2`，ctest 退出码 8 ⇒ 还原后 1/1 绿。
- 变异（usage 首行改回中文）⇒ 守卫红（CTest 8）/ 1/1 按预期 / 最终 exit 0（含收尾全量重建）。
- 全量：构建 exit 0；`ctest` = **119/119**（118 基线 + 新增），0 skip。

## 未做 / 边界

- `src/aot/lift_gen_emit.cpp` 的**错误分支**仍是中文（同样进构建日志；本轮范围为 `lift_gen.cpp`）。
- `--closure-gate` 的成功路径没有运行时守卫（要 AGE.EXE，≈48 s，属 assets 档）。
- 非 ASCII **argv 回显**仍可能是用户输入本身（不是本缺陷）。
- `--scale` 的 readout 文案等产品输出未动（见"产品面保留"）。
