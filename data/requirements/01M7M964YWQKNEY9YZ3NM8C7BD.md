# 客侧 CRT 的 _mbstrlen 按字节返回（local 9c=6 而非 2）⇒ 文本度量偏宽

- id: REQ-01M7M964YWQKNEY9YZ3NM8C7BD
- type: req
- status: open
- parent: REQ-01M6ZB7M1EEF5WMWEYX6K5556V
- tags: [emulator-lift, encoding, text]

## 现象（2026-11-17 / mac / SBUNKI 的运行时取样）

`local 9c = 6`（"はい" 2 字符 / 6 字节）——`i2c6`（`sub_430940` → 客侧 CRT 的 `__mbstrlen`）
返回的是**字节数**，不是字符数 ⇒ 按钮宽度算成 334（字符口径是 247）。
`GetACP` 已经返回 932（`src/shims/kernel32_bootstrap.cpp:159`），所以不是 ACP 常量的问题；
更可能是客侧 CRT 的 `_setlocale(0,"japanese")` 没成功（locale 数据/`LCMapString`/`CompareString`
等 API 面返回默认值），于是 `_mbstrlen` 退回单字节语义。

## 为什么要单独立

- 它直接改**文本度量**（按钮/文本框宽度、换行），而这类偏差是"画面看起来只是稍微不对"的静默面；
- 与 `REQ-01M6ZB7M1EEF5WMWEYX6K5556V`（CP932 编码后端）同一块：决定"系统接口替代自带表"的落点。

## 判据（待补）

1. 先定 `_setlocale` 在客侧 CRT 里走哪些 Win32 API（EA 锚）——用 `disasm --at` 读 `sub_430940` 一族；
2. 再判是我们的哪个 shim 返回了"非日语 locale"（`GetLocaleInfoA` / `GetUserDefaultLCID` /
   `EnumSystemLocalesA` / `LCMapStringA` / `CompareStringA` / `GetCPInfo`）；
3. 守卫形状：给客侧 `_mbstrlen` 一条**夹具**（同一串在 CP932 下的字符数），⛔ 不断言当前运行时数字。
