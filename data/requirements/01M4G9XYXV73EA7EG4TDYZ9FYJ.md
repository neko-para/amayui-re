# 前沿取证 · sub_40C210（被 7 个已登记函数调用）

- id: REQ-01M4G9XYXV73EA7EG4TDYZ9FYJ
- type: req
- status: done
- parent: REQ-01M4G9XYQG149FB3VG5Y6FW3YY
- done_reason: 取证结论也**推翻了这条前沿项**：sub_40C210 不是引擎函数，是 MSVC std::basic_string::assign(const char*, size_type)（含自重叠分支与 _Xlength_error 出口）；它连同 sub_40C120/sub_40B510/sub_40B420 是 STL 的私有实现（0x40B000–0x40C700 段）。台账已落 subject `msvc-stl+0x40C210/basic_string-assign-ptr-count`（10 个锚）。副产物：库代码分类的第二条判据（calls-stl-internal）。
- tags: [engine, coverage]

## 为什么是它

`pnpm tools ledger coverage` 前沿：被 **7 个"已登记"函数**直接调用，而台账里没有 `bin` 锚落在它身上。
调用方包括 `sub_42A420`（文本取值原语）、`sub_42AEA0`（取址原语）、`sub_4328F0`
⇒ 大概率在"值 / 地址的公共助手"那一层。★ 同址附近还有 `sub_40C120`（被 3 个已登记函数调用），一并说清两者关系。

## 要产出的（收口判据）

1. 一条台账观察：它是什么 + 锚 = EA；
2. `sub_40C210` 与 `sub_40C120` 的关系（同一族的两个入口？一个调另一个？）；
3. 守卫草案；
4. callee 里新暴露的未登记函数 ⇒ 登记进后续节点。
