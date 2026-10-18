# 批 S2ab · STL 容器布局识别（vector/map）—— 提案已验收，但合并被 propagate/calls 回归挡住

- id: REQ-01M5700CD0HASNQBGFB2BPBNYN
- type: req
- status: done
- parent: REQ-01M54KEF7X560CMPMGABG2W3CW
- done_reason: Tier A+B 全部合并：vector 19 例（T_stdvector / T_stdvector_proxy / 12 字节版备而未用、T_stdvector_T313c0）+ std::map 5 实例（Scene+0x408/418/428/438/448，T_stdmap_n760/n116/n80/n592 + 结点类型）+ 布局类型 T_stdstring24/T_stdlist；载体重定向 2 条（sub_40C780/sub_45D240 → T_stdvector*）、孤儿类型删 3 个（T_vec16/T_77960/T_5d860）。门禁：claims exit 0（8411 = 8400 + 11）、propagate 冲突 0、calls 2911 对/不一致 0、test 407/407、mutation 118/118、requirements 5/5
- tags: [engine, shape]

## 判据

（待写：怎么算做完，要可核对）

## 范围 / 非目标

（待写）
