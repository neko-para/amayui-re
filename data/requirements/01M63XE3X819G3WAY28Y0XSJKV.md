# 0x194 / 0x215 分诊：取证已齐、参考实现缺 case

- id: REQ-01M63XE3X819G3WAY28Y0XSJKV
- type: req
- status: open
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- order: 6
- tags: [emulator-cxx]

## 要分诊什么

`0x194` / `0x215` 在参考实现里原本**没有派发 case**，但取证已完成（见下）。两条各自二选一：
按取证实现 + 会红的守卫，或登记成"本版不实现"并给出理由（`repo:apps/emulator-cxx/SCOPE.md` + 条目残差）。

## 0x194（字符串相等）—— **已落地**

- 取证（`.lst`，锚 EA）：argc 3、推进 7；读序 **op3 → op2**，两者都按"格式化成字符串"的读法；
  把"两串逐字节相等"归一成 1 / 0 写进 op1。无早退、不记日志。
- spec：`CORE-OPS-TEXT-EQUAL-194`（`60-ops-text.md`），从 `GAP-OPS-0100-01FF` 移出；
  浮点标签的文本形式登记成残差 `RES-OPS-194-FLOAT-TEXT`。
- 实现：`apps/emulator-cxx/src/core/vm_ops_text.cpp` 的 `op_194`（读序、逐字节比较、写 1/0）。
- 守卫：`core_selftest` 新增三条断言（等 / 不等 / 空串相等 + "整数 5 与文本 \"5\" 相等"这条区分性用例）；
  `ctest -R emulator-core-selftest` 绿；整套 24 条 CTest 绿。

## 0x215 —— **仍缺前置**

- 取证：argc 2、推进 5；读 op2，在 `引擎+0x4ED10` 对象的 `+0x408` 映射里查 key；
  **未命中或条目标志位 0 ⇒ −1**，否则取条目 `+4` 的 dword 写进 op1。无分支。
- **前置**：那个"key → dword"映射在参考实现里**没有建模**（`0x4ED10` 那棵对象不存在）；
  它的**写者**也没定位 ⇒ 现在实现出来只能恒返回 −1，等于写了个假实现。
- 下一步：先在 `.lst` 里找写 `[obj+0x408]` 这张映射的指令（`sub_ADC20` 的对应写入口），
  再决定是"建模映射 + 实现 0x215"还是"登记成本版不实现"。
