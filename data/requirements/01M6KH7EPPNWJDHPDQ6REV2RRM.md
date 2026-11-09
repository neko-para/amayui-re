# GetStringTypeW 的非 ASCII CT_CTYPE1 分类（CP932 假名 / 全角 / CJK）+ CT_CTYPE2/3

- id: REQ-01M6KH7EPPNWJDHPDQ6REV2RRM
- type: req
- status: done
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- verify: apps/emulator-lift/tests/locale_ext.cpp#get-string-type-w-ctype1
- tags: [emulator-lift, m2, locale]

## 一句话
`GetStringTypeW(CT_CTYPE1)` 现在只保证 **ASCII** 的位值（`REQ-01M6K69JNGE7EP8ZNRDAXNSYZZ` 已收口：
ASCII 位值按 MSDN、`dwInfoType` 不再被丢弃）；**非 ASCII 一律给 0 位 + 一次日志** —— 对
`iswalpha(L'あ')` / `iswdigit(L'０')` 这类调用结果仍然是错的（只是不再"假装按 ASCII 猜"）。

## 剩余
1. 一张**字符类表**（或等价的可再校验数据）给出 BMP 里 CP932 相关区段的 CT_CTYPE1 位：
   - U+3000–303F（日文标点 / 全角空格 U+3000 是 SPACE）
   - U+3040–309F（平假名）、U+30A0–30FF（片假名）
   - U+4E00–9FFF（CJK 统一表意）
   - U+FF01–FF5E（全角 ASCII 变体）、U+FF61–FF9F（半角片假名）
2. `CT_CTYPE2`（双向类）与 `CT_CTYPE3`（语言学类别）：现在**明确失败** `ERROR_INVALID_PARAMETER(87)`。
   真机上这两个值合法 —— 要做就得先定"这些位从哪来"，⛔ 不许沿用 CT_CTYPE1 的位。
3. 收口判据要能机械判定：每个区段至少一个定点，且"非 ASCII 不再一律 0"有断言
   （现在 `locale-ext#get-string-type-w-ctype1` 断言的是 0，**那条要跟着改**）。

## 边界
- ⛔ 不许用 `w < 0x80` 或宿主 `iswalpha` 猜：要么有表，要么明确失败（当前选择后者）。
- 表若从宿主 libc / ICU 派生，必须写清"派生来源 + 可复现命令"，否则它就是第二份无锚真源（AGENTS.md §6）。
- 本单与 `REQ-01M6K69K28AT5VNKZAJXT6PZJ4` 的收口单**同级**（那两单只做 ASCII，已 done）。
