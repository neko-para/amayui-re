# 🐞S3 win32：CTest fault-report 钉 LF，Windows 子进程 stderr 是 CRLF（全量 ctest 唯一红）

- id: REQ-01M7QQ8A7C3N0R18Y8GYH2XE8G
- type: bug
- status: done
- parent: REQ-01M6JFZFN15MBPG7Y89WNZACFW
- repro: apps/emulator-lift/tests/fault_report.cpp#default-fault-text-unchanged
- severity: S3
- done_reason: 测试捕获侧归一化 CRLF 后修好：ctest -R ^fault-report$ 由红转 1/1 Passed；全量 ctest 116/116（0 skip）。产品输出未改。
- tags: [emulator-lift, win32, test]

## 现象

win32 全量 `ctest` 的唯一红：

```
fault_report.cpp:527: CHECK failed:
  safe.stderr_text.find("access     : READ 0x20000000  size=4\n") != std::string::npos
fault-report: 180 checks, 1 failures
```

## 机制

断言钉的是 LF，但 Windows 上子进程 stderr 走 CRT **文本模式**（继承的文件句柄一样）⇒ 每个 `\n` 被翻成
`\r\n`（实测该行行尾 `0D 0A`，捕获文本里 5 处 CR）。POSIX 无此翻译 ⇒ mac 绿。
断言由 `3180972`（2026-11-18，fault EA / `LIFT_FAULT_DUMP` / `LIFT_WATCH`）引入，此后没在 win32 跑过全量 ctest。

## 落地

`tests/fault_report.cpp` 新增 `appendChildStderr()`，两处捕获点（Windows 临时文件 / POSIX pipe）都走它：
**逐字节剥掉 `\r`**。⛔ 不动断言、⛔ 不动产品侧输出字节。
取舍说明：实现是"剥所有 `\r`"而不是严格的"`\r\n`→`\n`"（差 1 字节前瞻）；对本文件无影响 —— 180 条断言
里没有任何一条依赖裸 `\r`，且剥 `\r` 只会让比较回到"子进程想说的那行文本"。

## 读数（2026-11-18 / win32 / clang-cl 22.1.2 / RelWithDebInfo）

- 修前：`ctest --test-dir out -R '^fault-report$' --output-on-failure` = 红（1 failure / 180 checks）。
- 修后：同命令 **1/1 Passed**（父级在最终二进制上复跑，0.51 s）。
- 全量：`ctest --test-dir out --output-on-failure` = **116/116 passed · 0 failed · 0 skip · 188.00 s · exit 0**
  （日志 `.tmp/qjs-018/ctest-full3.log`；父级复跑 `ui-script-quickjs` / `fault-report` 两条均 Passed）。
