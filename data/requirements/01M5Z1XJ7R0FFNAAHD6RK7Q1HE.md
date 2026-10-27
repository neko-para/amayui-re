# 基线偏低复核：缓存 ascent 用渲染字体量（比引擎合同低 1~2px）

- id: REQ-01M5Z1XJ7R0FFNAAHD6RK7Q1HE
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: apps/emulator-cxx/tests/core_selftest.cpp#0x204 宿主基线补偿没生效
- tags: [emulator-cxx font]

# 基线偏低：固定用 Amayui 量 + 基线补偿 2px（2026-10-28 定案）

## 判据

- 分歧消失 = 墨迹顶与真机（= GDI 用引擎字面名的同一帧）的差 **≤1px**：
  实测补偿后 **20px 一致（0px）**、**28px 低 1px**。
- 守卫：`core_selftest` 的「`0x204` 宿主基线补偿没生效」——同一夹具画两次（默认 / `BASELINE_LIFT=0`），
  断言关掉补偿后墨迹顶**正好低 2px**。变异（`lift = 2` → 0）⇒ 当场红（`默认墨迹顶=13、lift=0 的墨迹顶=13`）。

## 复现

```powershell
# 默认档 vs 真机档（GDI + 引擎字面名）：同一输入脚本、同一帧，比同一标签的墨迹顶
.\apps\emulator-cxx\out\emulator-cxx.exe --install "<安装目录>" --skip-logo --play-headless ^
    --input-script <输入脚本> --anchor-script 21092 --require-script 20940 --tick-ms 16 ^
    --max-steps 400000 --bmp out\ft --bmp-stride 300 --bmp-limit 4
$env:EMULATOR_CXX_GLYPH_GDI='1'; $env:EMULATOR_CXX_GLYPH_GDI_FACE='engine'
# 再跑一遍，比同一标签的墨迹顶（实测 28px：108 vs 107）
```

- 诊断口：`EMULATOR_CXX_TEXT_TRACE=1` 打印 `ascent= / cached= / baseline= / lift=`。
- 数值与取证：`repo:docs/02-engine/ops-forensics-2026-10-title-option.md` §42。

## 结论

- 本质：宿主替换的 Amayui 自身 ascent 比引擎的参考字体大（FT 量到 20px 20 / 28px 28，参考字体 17 / 24），
  而引擎的缓存 ascent 恒按参考字体量 ⇒ 真机刚好"居中"；我们量自己的字体就整体偏低。
- 处置（用户口径）：①**固定用 Amayui 量**（不看参考字体，量度来源只有一处）；
  ②画字前基线再**抬 2px**（`EMULATOR_CXX_BASELINE_LIFT`，0 = 关）。宿主视觉策略，⛔ 不是引擎语义。
- 残差：28px 仍低 1px（要 0px 需 lift=3，但那让 20px 高 1px）。
