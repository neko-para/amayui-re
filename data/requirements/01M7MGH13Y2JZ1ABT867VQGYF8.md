# 闭包漏扫未对齐立即数（D26）：0x004F80E0 是手工补的，下次 census 会抹掉

- id: REQ-01M7MGH13Y2JZ1ABT867VQGYF8
- type: req
- status: open
- parent: REQ-01M7CZQZ5RR0WV8CQE2ZEKJHHQ
- tags: [emulator-lift, aot, closure]

## 现象

`aot/closure.txt` 是**生成物**（`closure_build.cpp` 的 D14/D15/D17/D21/D26 扫出来的根）。
`0x004F80E0`（`EnumSystemLocalesA` 的回调 EA，`LanguageEnumProc`）只以 `push $0x4F80E0` 的
**未对齐立即数**出现在 `0x4F822B`（客侧 CRT 的 `___get_qualified_locale` 的"只有语言、没有国家"分支）
⇒ **D26 只扫 4 字节对齐 dword，抓不到它**。修好 `bt` 内存形（BUG-27）后第一次走到它就
`stop: decode-error @ eip=004F80E0`。

本轮是**手工**往 `closure.txt` 补了一行（`# why: indirect-entry…`）。⚠ 下一次跑 census 会把它抹掉，
而症状是"只有走那条 CRT 分支才炸"（默认路径看不见）。

## 要做什么（二选一或都做）

1. **守卫（便宜、先做）**：一条 pure CTest/`tools/` 用例断言 `closure.txt` 里 `byEa(0x004F80E0)` 非空
   （或更一般：`EnumSystemLocalesA` 的回调 EA 在根集合里）⇒ 手工行被抹掉时**当场红**，
   ⛔ 不要断言"当前行数"。
2. **生成器（根治）**：让 D15/D26 的扫描也认**未对齐立即数**的 `push imm32` / `mov r32, imm32`
   形态（按指令解码而不是按 4 字节对齐 dword 扫），并把 `0x4F822B` 这个站点当证据写进
   `closure_build.cpp` 的头注。

## 判据

- 删掉手工行后 `--closure-gate` 或新守卫必须红；补回后绿。
- ⛔ 不放宽其它五条闭包判据；⛔ 不把 `0x004F80E0` 写成"下一跳"式的特判。
