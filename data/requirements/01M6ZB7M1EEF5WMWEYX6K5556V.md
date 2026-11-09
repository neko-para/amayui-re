# CP932 编码后端：系统接口（Windows MBCS / POSIX iconv）替代自带表的裁决与落地

- id: REQ-01M6ZB7M1EEF5WMWEYX6K5556V
- type: req
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- tags: [cp932, encoding, platform]

## 一句话
`apps/emulator-lift` 的 CP932 ⇄ UTF-16 现在走**自带派生表**（`src/runtime/cp932_table.h` + `tools/lift-cp932-gen.mjs`）。license 不再是约束（纯开源非商业项目），所以要裁决：能不能换成**平台原生编码接口**（Windows `MultiByteToWideChar` / `WideCharToMultiByte(932)`；macOS/glibc 系统 `iconv("CP932")`），从而不再维护 1909 行表。

## 已有评估（2026-11-09，读数可重跑）
全文与命令：`repo:docs/03-emulator/encoding-backend.md`。结论摘要：
- **解码等价**：256 单字节 + 9604 双字节对，除 5 个单字节外与 macOS 系统 iconv 逐条相同；这 5 个（`0x80` / `0xA0` / `0xFD` / `0xFE` / `0xFF`）正是 Windows CP932 扩展，Windows 原生给它们、iconv 判非法 ⇒ 换系统接口只需 5 条平台胶水（不是表）。
- **编码不等价**：373 个码位有重复映射（NEC `0xED/0xEE` vs IBM `0xFA..0xFC`），我们的表选 NEC、系统 iconv 选 IBM ⇒ 直接换会让同一字符串在 Windows 与 macOS 编出不同字节（存档名 / 配置串）。
- 可用性：musl 无 iconv；性能：表是进程内二分，iconv 需缓存描述符（非热路径）。

## 要裁决 / 要做的
1. **拿到 Windows 侧的读数**（需 Windows 机器）：对那 373 个码位调 `WideCharToMultiByte(932, 0, …)`，确认它挑 NEC 还是 IBM。命令/程序见评估文档 §4。
2. 二选一：
   - **A 保持现表**（判据：评估文档 + 这条单的裁决理由；表是"三平台逐字节一致"的保证本身）；
   - **B 换系统接口**（解码：MBCS / iconv + 5 条单字节胶水；编码：把 373 个重复映射的选择钉成显式策略表 + 跨平台等价守卫）。
3. 选 B 时，等价守卫必须**在 Windows 与 macOS 各跑一次**（`@env pure`，全定义域摘要或与现表对拍），否则"平台表悄悄漂了"没人发现。

## 判据
- 选定路线的裁决写进 `done_reason`（A：为什么继续养表；B：373 条策略的来源 + 两平台守卫的读数）。
- 选 B 之后：`cp932.cpp` 与现表**只能留一处**（表可降级为测试 oracle，但必须在文档里写清）；`pnpm tools requirements validate` / `pnpm test` / `pnpm verify` / lift `ctest` 全绿。
- ⛔ 不拿"某台机器上能跑"当等价证明：boot 的 A4 trace 只覆盖走到 `_WinMain@16` 用到的字符。

## 不做什么
- ⛔ 不在没有 Windows 读数的情况下改 `cp932.cpp`（它是 boot 必经路径，回归面大）。
- ⛔ 不为这条单扩 `SCOPE.md` 的偏差清单（这是实现取舍，不是用户可见偏差）。
