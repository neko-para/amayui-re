# 🐞TITLE hover：0x12e 命中测试未成篇/未实现

- id: REQ-01M5VBGREC7T3CC7H3QCTV2877
- type: bug
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/input-12e-contract.test.mjs#★ 0x12e 命中查找：TITLE 五框 / 起始下标 / 无命中
- repro: tools/test/input-12e-contract.test.mjs#★ 0x12e 命中查找：TITLE 五框 / 起始下标 / 无命中
- severity: S2
- tags: [hover, input]

## 现象

TITLE 主循环里鼠标移动不产生 hover 高亮。回调链本身是活的：`0xcc` 登记 1 次、`0xcd` 泵 11405 次，`0x109` / `0x2fc` / `0x108` 都在回调里被调用（`apps/emulator-cxx --footprint --max-steps 600000`）。断点是 TITLE 的命中测试 `0x12e`。

## 判据 / 复现

- TITLE 在 `label_0000047c` 里用 `i12e 3f5 1 3f0 3f1 cd 5 69 0` 做五按钮 AABB 命中；命中写 `local 3f5 = 0..4`，未命中写 `-1`。
- 当前 `op_12e` 恒写 `-1` 并报一次 `unmodeled-input-hit-test:0x12e` ⇒ `3f5 == 3f6` 恒成立，`label_00003340` 永不画 hover 叠层。
- 目标守卫：TITLE 同形五框夹具（框 `[0,0x9c,0,0x9c]`、基准数组 `44e/3e0/365/2d9/453` 与 `126/192/1e5/21f/22a`、count=5）在命中第 2 框时返回 `2`，框外返回 `-1`，起始下标 `2` 时从 `3` 起扫。

## 取证锚

- handler `sub_42F230` @ `0x42F230`；逐字语义见台账/规格草稿。
- TITLE 站点：`TITLE.BIN` 的 `i12e`（本地 `.lst` 行 100，非锚）。
- 语义：`start+1` 起扫；`dx=x-planeX[i]`、`dy=y-planeY[i]`；命中判据 `m1+dx-s0>=0 && m3+dy-s2>=0 && s1-dx-m0>=0 && s3-dy-m2>=0`；只写第 1 个操作数。

## 范围

- spec：新增命中测试规则，移出 `GAP-OPS-0100-01FF`。
- emulator-cxx：实现 `op_12e`；删除 stub 诊断；selftest 从“断言 stub”改成命中/未命中/起始下标。
- 不含真机像素 oracle；hover 的最终可见性仍只到“发出正确绘制项”这一层。
