# AOT 最小输入闭环：投递层 + 11 读点 + AOT --input-plan 控制面 + 门 {Always,Presents}

- id: REQ-01M7G7EMX3JR272D8FVM7JFY9D
- type: req
- status: done
- parent: REQ-01M7G7EJF062NKQ0D40RF3BZ4Z
- done_reason: 投递层（11 读点 + input::deliver*）今天由 tools/test/input-plan-shape.test.mjs 的 ①② 守住；--input-plan 控制面与门 {Always,Presents} 随 plan 整体退役（2026-11-27 用户裁决），原 CTest input-plan-read-points 已删，不再单设守卫。
- tags: [emulator-lift, input, aot]

## 判据

（待写：怎么算做完，要可核对）

## 范围 / 非目标

（待写）
