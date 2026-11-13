# overrides 清理 A · 删 12 条不可达 + kind 三值 + 模块集/AGERC 单列 + 字节一致性断言

- id: REQ-01M7A5TZ91V3MRNEFDHWKN20MY
- type: req
- status: done
- parent: REQ-01M7A5CDZC1HGDB3F2KNPJBJ16
- verify: tools/test/lift.test.mjs#① overrides.json 逐字节等于 lift sync 的派生结果
- tags: [emulator-lift, overrides, tools]

## 范围（父单 `REQ-01M7A5CDZC1HGDB3F2KNPJBJ16` 的前半，机械化部分）

审计：`.tmp/lift-overrides/audit.md`。用户已裁决：删 12 条不可达入口；AGERC 不登记为 gap；换机制（scope_ref 属后半）。

## 要做

1. **删 12 条不可达登记**（C++ 与 JSON 一起）：`user32!GetDC/ReleaseDC/SetFocus/RegisterClassA`、`gdi32!CreateCompatibleDC/CreateDIBSection/BitBlt/GetDeviceCaps`、
   `kernel32!FindResourceA/SizeofResource/LoadResource/LockResource`。删后 `lift sync --write`，`lift check` 绿、`report` 导入覆盖仍 100%。
2. **`kind` 三值化**：`replace`（add）/ `noop`（addStub：有 fn、静默成功）/ `trap`（addUnimplemented：响亮失败）；派生缺口仍 `trap`。
   `describe` / `data/lift/overrides.md` / `tools/test/lift.test.mjs` 同步。⛔ 不保留旧 `stub` 值（或明确只作向后兼容读入）。
3. **模块集**：`tools/lift.mjs` 声明 `{AGE.EXE: executed=true, corpus/assets/agerc/AGERC.DLL: executed=false}`（⛔ 不用 `install/AGERC.DLL`）。
   `bare` 只对 executed=true 强制；AGERC 的裸导入单列 `bareUnexecuted`（`report` 输出 + `check` 不判红）。
   `never-imported` 改 `not-referenced`（不在任一导入表 ∧ 不是被拦截导出），只进报告。
4. **`check` 加逐字节断言**：`readText(overrides.json) === renderOverrides(expectedEntries(...))`（唯一写入口 = `lift sync --write`）。
5. **修 reason 读取**：相邻字符串字面量必须拼接（`kernel32_agerc.cpp` 那处现在落成 `…进 "\n              "g_nameLenMax…`）。
6. **测试/变异**：`tools/test/lift.test.mjs` 加 ①字节一致 ②kind 三值 ③AGERC bare 不进 overrides ④not-referenced 判定；
   `tools/mutate-check.mjs` 加"手改 overrides.json 一个字符 ⇒ check 红"。

## 判据

- `pnpm tools lift check` 绿；`lift report` 出现 `bareUnexecuted` 与 `not-referenced`；`overrides.json` 与 sync 输出逐字节一致；
- 被删 12 条在 `rg` 中消失；导入覆盖仍 100%；
- `ctest` 95 注册 / 91 通过 / 4 SKIP（删登记不应改行为）。

## 非目标

- scope_ref / SCOPE §1 补名 / 37 条 reason 重写：属后半（另单）。
