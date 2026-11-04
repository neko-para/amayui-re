# 码页族：932 的 A/W 转换仍按字节直通（真表已存在但没接上）

- id: REQ-01M6K4VCMVGT95RRBRRK9Q22PA
- type: bug
- status: done
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- verify: apps/emulator-lift/tests/codepage.cpp#mb-to-wide-932
- repro: apps/emulator-lift/src/shims/kernel32_bootstrap.cpp#shimMultiByteToWideChar
- severity: S2
- tags: [emulator-lift, m1, s6]

## 一句话
`MultiByteToWideChar` / `WideCharToMultiByte` 仍是 M1a 的"ASCII 直通"：每个字节原样扩成同一个
16 位值（非 ASCII 得到 U+00XX），并且每次调用打一行"M1a 只保证 ASCII"的日志。但 CP932 真表
（`runtime/cp932`，S6 码页族）**早就在仓库里**，`GetLocaleInfoA` / `wsprintfA` 已经在用它 ——
这两个 API 只是没接上，且日志把"已有真表"说成了"要等 M1b"。

## 现状（2026-11-04，用户实际运行命中）
- 启动后立即看到两行日志：
  `[shim] kernel32!WideCharToMultiByte: CP932 真表属 S6 码页族（M1b）；M1a 只保证 ASCII 逐字节正确…`
  `[shim] kernel32!MultiByteToWideChar: …`
- 代码：`kernel32_bootstrap.cpp` 的 `shimMultiByteToWideChar` 把 `src` 的每个字节 `wr16(dst+i*2, b)`；
  `shimWideCharToMultiByte` 对 `w > 0xFF` 写 `'?'`、否则写低 8 位。返回值直接拿**字节数**当**字符数**。
- 真表：`runtime/cp932.h` 的 `cp932ToWide` / `wideToCp932`（`tools/lift-cp932-gen.mjs` 派生、
  三平台同一份）；`kernel32_string.cpp` 的 `wsprintfA` `%S` / `GetLocaleInfoA` 已按它编码。
- 为什么这对 M1 是缺陷、而不是"已知边界"：A/W 往返恰好还能对齐（字节 ⇒ 同值 ⇒ 同字节），
  所以启动路径不炸；但一旦 guest 对宽串做**比较 / 长度 / 字符分类**（M2 排版、消息框、路径），
  非 ASCII 就会静默错，而且 `GetCPInfo` 又已声称 `MaxCharSize=2` ⇒ 代码会按双字节字符分配。
- 台账层面：`M1` 父单的"已知边界"写着"码页现状只有 ASCII 直通"，但进度树里没有对应子单 ⇒
  `requirements plan` 看起来 M1 只剩 3 张单，像是"码页已经支持"。本单把这条边界收成实现 + 可再校验观察。

## 落地（2026-11-04）
- `kernel32_bootstrap.cpp`：
  - 932 / `CP_ACP(0)` / `CP_OEMCP(1)` / `CP_THREAD_ACP(3)` → `cp932Decode`（非法序列默认 U+FFFD、
    只前进 1 字节；`MB_ERR_INVALID_CHARS` ⇒ 0 + `ERROR_NO_UNICODE_TRANSLATION(1113)`）；
  - `CP_UTF8(65001)` → `utf8ToUtf16` / `utf16ToUtf8`（替换字符可参数化：932 用 `'?'`，UTF-8 用 U+FFFD）；
  - 返回值 / 缓冲检查一律按**字符数**（MB→W）或**字节数**（W→A），`-1` 含结尾 0，
    `dst == NULL || len == 0` 是"问大小"，不足 ⇒ 0 + `ERROR_INSUFFICIENT_BUFFER(122)`；
  - `cbMultiByte == 0` / `cchWideChar == 0` / 源指针空 ⇒ 0 + `ERROR_INVALID_PARAMETER(87)`（旧实现当成"0 个字符"）；
  - W→A 支持 `lpDefaultChar` / `lpUsedDefaultChar`（成功时写 0/1），代理对按一个替换字符，
    `WC_ERR_INVALID_CHARS` ⇒ 1113；
  - 只有**非 932 / 非 65001**的码页才保留"ASCII 正确 + 字节直通"并记日志（日志不再声称 M1b）。
- `runtime/cp932.{h,cpp}`：`utf8ToUtf16` / `utf16ToUtf8` 增加替换字符参数（默认仍是 `'?'`，路径 API 不变）。
- 计划文档 `docs/plan-m1-cpu-base.md` 的两处"要引 `libiconv`"改成"已落地的自带表 `runtime/cp932`"。

## 判据（已落实为 `codepage#mb-to-wide-932` 等 6 组）
- 932：`0x82A0 → U+3042`（1 字符）、`U+3042/U+30BD → 82 A0 83 5C`（4 字节）、半角片假名 `U+FF71 → 0xB1`；
- 长度：`-1` 含结尾 0；查询返回字符数 / 字节数；缓冲不足 ⇒ 0 + 122；
- 非法：默认 U+FFFD（`81 20` → U+FFFD + 0x20），严格模式 ⇒ 0 + 1113；末尾孤立 lead 同理；
- W→A 默认字符：`U+2603 → '?'`、自定义 `'#'`；可编码时 `*lpUsedDefaultChar == 0`；
  增补面 `U+1F600`（代理对）⇒ 一个 `'?'`；孤立代理 + 严格模式 ⇒ 1113；
- 65001：`あ` 往返 3 字节；孤立代理默认写 `EF BF BD`、严格模式 1113。

## 门禁（本机 macOS arm64）
- `ctest` **24/24**（新增 `codepage`）；`LIFT_SAFE_MEMORY=1 ctest` **24/24**。
- A4 `boot-trace`：`insns=60660 hash=E0379BF37C2DFAF3 inputs=35CB9281F6D0881B`（改前 60652；
  多的 8 条来自 `_setlocale` 分支真的走宽字符转换，最终 `eip=0x004BA890` 与寄存器 hash 不变）。
- 变异（`node tools/mutate-check.mjs --tier lift` **30/30**）：`MultiByteToWideChar` 退回逐字节直通 ⇒ `ctest:codepage` 红；
  `lpUsedDefaultChar` 回报取反 ⇒ 同一夹具的 default-char 用例红。

## 保留 / 边界
- 1252 / 437 等**其它码页**仍只有 ASCII 正确（首次使用记一行日志）；本作 `GetACP()=932`，未观察到使用。
- `GetStringTypeW` / `LCMapStringW` 的字符分类 / 大小写仍是 ASCII 口径（CP932 假名按 0 处理）；
  启动路径不依赖它们，但 M2 排版前要补 —— 记在 `M1` 父单的"已知边界"。
