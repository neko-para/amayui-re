# GetLocaleInfoA 未与 GetLocaleInfoW 统一：A 版仍是常量 shim

- id: REQ-01M6JCC1F886N40CYHJV2KHW1H
- type: req
- status: done
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- verify: apps/emulator-lift/tests/locale_ext.cpp#get-locale-info-a
- tags: [emulator-lift, m1, tech-debt]

## 一句话
`GetLocaleInfoA` 已与 `GetLocaleInfoW` 统一：**同一张 locale 表、同一套错误码**；差异只剩
"TCHAR = 字节 / CP932 编码 / `LOCALE_RETURN_NUMBER` 返回 4"。

## 落地（2026-11-04）
- `apps/emulator-lift/src/shims/kernel32_string.cpp`：
  - `shimGetLocaleInfoA` 复用 `findLocale` + `lookupLocaleInfo`（不再有第二张表）；
  - 文本用 `wideToCp932`（文档：按该 LCID 的 ANSI 代码页翻译；表中 en-US=1252 / ja-JP=932，而本模拟器只有 CP932 一张表、
    en-US 的串全 ASCII ⇒ 等价）；不可编码字符按文档替换为 `?`；
  - `cchData` 按**字节**计（含结尾 0）；`LOCALE_RETURN_NUMBER` 写 DWORD、返回 **4**；
  - `LOCALE_USE_CP_ACP(0x40000000)` 是 A 版专属 flag：接受并清位；
  - 错误码与 W 一致：非法 LCID / 不支持 LCType ⇒ 87；未知高位 flag ⇒ 1008；缓冲不足 ⇒ 122；
- `apps/emulator-lift/src/shims/kernel32_bootstrap.cpp`：删掉旧的常量 `"0409"` shim 与它的注册（一处名字只注册一次）。
- 文件头"偏差 3"已改写（不再是"已知差异、待统一"）。

## 与 W 一致的**故意**口径（两者一起改，别只改一侧）
- `cchData > 0` 但 `lpLCData == NULL` ⇒ `0 + ERROR_INVALID_PARAMETER(87)`（文档把这种情况列在 122 下）；
- `LOCALE_NOUSEROVERRIDE` 仍不接受（W 也不接受）。

## 证据
- `locale-ext` 新用例 `get-locale-info-a`：5 组 `(LCID, LCType)` 的 **A == W 的 CP932 投影**、A 的 `cchData==0` 是字节数、
  数值返回 4 / `cchData==0`⇒4 / cap<4⇒122、非法 LCID/LCType/flag/NULL 缓冲的错误码；整份 `locale-ext` 87 checks 全绿。
- 守卫自检：`pnpm test:mutation --match locale-ext`（把 A 的写字节删掉）⇒ `locale-ext` **红**（CTest 退出码 8）。
