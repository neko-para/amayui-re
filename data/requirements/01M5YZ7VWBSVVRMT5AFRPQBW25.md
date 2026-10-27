# 选项/角色设定的黑边比真机粗：绘制模式 3 的副色趟画到了 4px

- id: REQ-01M5YZ7VWBSVVRMT5AFRPQBW25
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: apps/emulator-cxx/tests/core_selftest.cpp#0x204 mode 3 副色趟与引擎的径向位移不符
- tags: [emulator-cxx font]

# 选项/角色设定的黑边比真机粗：绘制模式 3 的副色趟画到了 4px

## 判据

- 分歧消失 = 纹理 196（选项界面的文字带）里，绘制模式 3 的副色集合**恰好**等于"主色按引擎的 6 个位移
  {(0,1),(1,1),(1,0),(0,-1),(-1,-1),(-1,0)} 平移的并集（去掉主色自身）"（真机黑边 1~2px；修复前斜向到 4px）。
- 守卫：`core_selftest` 的「`0x204` mode 3 副色趟与引擎的径向位移不符」——引擎字形（bake stub ⇒ 1-bit）、
  主色白 / 副色黑、绘制模式 3；**6 个位移硬写在测试里当 oracle**，双向断言。
  变异实测（手工施加、撤销）：±1..±4 ⇒ "无法解释的=157"；单趟 (1,1) ⇒ "该有却没有的=165"；
  角度改成 ×π÷180（8 方向）⇒ "无法解释的=24"。

## 复现

```powershell
# 1) 到选项 → 角色设定（点 y=313 那一行）
# 2) 导出该帧与文字带纹理
.\apps\emulator-cxx\out\emulator-cxx.exe --install "<游戏安装目录>" --skip-logo --play-headless ^
    --input-script <上一步的输入脚本> --anchor-script 21092 --require-script 20940 --tick-ms 16 ^
    --max-steps 400000 --dump-texture 196 --dump-texture-out out\tex196.bmp
```

- 诊断口：`EMULATOR_CXX_TEXT_TRACE=1` 打印每次 `0x204` 的形状（这一步定位到 `mode=3 secondary=000000`）。
- 取证与锚：`repo:docs/02-engine/ops-forensics-2026-10-title-option.md` §40。

## 结论

- 真因**不是字型**：黑边宽度由副色趟的位移决定。引擎（`sub_471180` 模式 3 支 + `sub_46ECC0`）是
  **径向一圈**：半径 = 投影偏移 (1,1)、步长 = 360 ÷ (√(偏移纵+偏移横) × 8) ⇒ 12 趟（扫过约 1.94 圈）、
  位移 = round(sin/cos(角度 × 180 ÷ π) × 偏移分量) ⇒ 收敛成 6 个方向 ⇒ 只外扩 **1px**。
- spec：`CORE-OPS-RENDER-TEXT-204` 的模式 3 由"本版不规定"改成按站点取数；
  `RES-RENDER-TEXT-PROJECTION` 删去该项。
