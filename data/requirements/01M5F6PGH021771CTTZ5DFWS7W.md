# spec：写 `0x192 set-string` 的规则（启动链最高频指令，目前无条目）

- id: REQ-01M5F6PGH021771CTTZ5DFWS7W
- type: req
- status: done
- parent: REQ-01M5D1G31PN9EGANRVAR753BPC
- verify: tools/test/emulator-new-string.test.mjs#★★ `0x192 set-string`
- tags: [spec, ops, string]

## 一句话

`0x192 set-string` 是启动链里出现最多的一条（台账记 **7731** 次；`INITCONFIG0.BIN` 用它把字体名写进全局串池），但 spec 里**没有条目**。本单把它写成 `CORE-OPS-STRING-SET`，并补上它源侧所依赖的一条**口径缺口**（内联文本是逐字节取反存的）。

## 已做

- `60-ops` 新增「字符串（opcode 0x192）」小节 + `CORE-OPS-STRING-SET`；章首 **47 → 48 个 opcode**、**41 → 42 条规则**。
- `bindings`：14 处站点（handler 体 5 处 + 写回分派分支 8 处 + 公共写入路径 1 处），
  `functions` = `sub_433660` / `sub_433310` / `sub_42A420`；
  `evidence` = 台账 `01M4G4N4NZ0C036Y6F3F002B3K`（accepted：内联串偏移 = `headerLen + 4·raw`）+ 两条新守卫。
- `audit`：`draft` / `handled` + 3 条残差（局部串池与两个高位标签分支 / type 8·14 的指针表角色 / 数值→文本的格式）。
- ★ **顺带订正 `40-pools`**：原文只写「以 NUL 结束」，**没有写字节是逐字节取反的** ⇒ 照原文实现会把整段内联文本解成乱码。已补「逐字节与 0xFF 异或、取反后为 0 处终止（存储字节 `0xFF`）」。
- `RES-OPS-UNWRITTEN` 的 47 → 48。

## 证据（写规则时用的）

- **handler** `sub_433660`：长度槽 5（argc 2）→ 读 op2（文本原语 `sub_42A420`）→ 按 op1 的类型标签写回（`sub_433310`）。
- **落点分派**（逐字）：type 5 ⇒ 全局串池；type 11 ⇒ 当前帧的局部串池；type 8 ⇒ 一条全局指针表；
  type 14 ⇒ 一条按帧指针表；`0x8005` / `0x800B` ⇒ 局部池的两个变体分支；其余 ⇒ 抛类型异常。
- **源侧**：type 2 内联文本（文件偏移 `headerLen + 4·raw`、逐字节 `^0xFF`、到 0 止、cp932）；
  数值型操作数被格式化成十进制文本（台账 `01M4B3DQVB3G390N2E6E2S3Q2T` 的原始结论已因"整型路"部分被撤回并被取代，
  本条只采信仍有效的偏移/解码部分，见 `01M4G4N4NZ0C036Y6F3F002B3K`）。
- **参考实现**：`apps/emulator/src/vm/ops.ts#opSetString`（旧）、`apps/emulator-new/src/handlers.ts#setString192`（新）。
- **守卫**：`tools/test/emulator-new-string.test.mjs` 的 `★★ \`0x192 set-string\`` 与 `★ type 2（内联字符串）`；
  **变异**（各 1/1 红）：偏移漏 `4×` / 少 `^0xFF` / 未注册。

## 判据

- `pnpm tools spec validate` 13/13；条目有 `binding.evidence`（accepted 台账 + 纯档守卫）；
- `pnpm test` 全绿；守卫会红（见上三条变异）。
