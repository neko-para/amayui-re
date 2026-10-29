# 文本记录的生产链：0x6e/0x196/0x6f 追加 72 字节记录 + 0x82 的下标空间（实现缺）

- id: REQ-01M630YH3RVEJJEGYKZJW2VQE7
- type: req
- status: done
- parent: REQ-01M5SZ3M7GCR0M23RSYBD7GEA5
- verify: apps/emulator-cxx/CMakeLists.txt#emulator-core-selftest
- done_reason: 0x6e/0x196/0x6f 的 72 字节记录追加已按 sub_46BE30/sub_45F090 的实参链落地（文本门 < 0 ⇒ 整批不追加；折行先提交前缀再结束行；0x6f 追加空串记录），0x82 的记录下标空间随之与引擎一致；守卫 emulator-core-selftest 的 check_text_layer_surface 钉住 0x196 产出一条记录且 0x82 不早退。记录标志位 2 的置位者/0x82 选支另开 REQ-01M633QTR9VXK1CA1AYJCJPNN8；振り仮名记录仍留残差。
- tags: [text, emulator-cxx]

## 已落地（本轮）

- **条件**：`0x6e` / `0x196` 把 `Engine+0x5EC7C`（文本门，`0x1bb` 写；正常 0、抑制 0x80000000）作为第 5 实参交给排版入口
  `sub_46BE30`；`sub_4691A0` 只在 `a5 >= 0` 时追加 72 字节记录 ⇒ **抑制态整批不追加**。
- **顺序**：越界时先追加当前段前缀（`0x46C225`），再调 `sub_46AF90` 结束该行（它再追加一条**空串记录**，标志 `|= 8`），
  当前字符落到新行行首；正常收尾追加最后一段（`0x46CAB7`）。
- **字段**：`sub_45F090` 写 44 字节定长区 + 28 字节字符串 = 72；记录槽号 = 层号，四个位置格取自 `&v100`，
  标志 = 第 3 实参（文本门值；行结束 `|= 8`）。
- **实现**：`Machine::append_text_span_record`（`apps/emulator-cxx/src/core/vm_ops_text.cpp`）在
  `draw_text_layer_text` 的折行 / 收尾与 `end_text_layer_line` 里追加；`TextSpanRecord` 增 `x/y/x1/y1/text`，
  getter `text_span_text` / `text_span_slot`。
- **守卫**：`core_selftest.cpp` 的 `check_text_layer_surface` 新增：`0x196("AB")` 后 `text_span_count()==1`、
  text=="AB"、slot==9；接着 `0x82(op2=0)` 必须使 `text_span_draw_requests_` +1（旧行为：记录为空 ⇒ 早退）；
  `0x6f` 后多一条空串记录且 flags bit3 置起。CTest：`emulator-core-selftest`。

## 仍留残差

- **标志位 2 的来源**：按实参链正常正文记录 `flags=0`（没有 `|2`），与规则正文「位 2 恒置起」冲突 ⇒ 已另开
  `REQ-01M633QTR9VXK1CA1AYJCJPNN8`（spike）跟踪；本实现按实参走，不置 2。
- **振り仮名记录**：`0x196` 的振り仮名是否另追加记录（`sub_465A20` / 每渲染行）未实现，仍留残差。
- **0x82 的绘制**：本条只记「一次绘制请求」，不落像素（与既有 `RES-OPS-TEXT-SPAN-MEASURE` 一致）。
