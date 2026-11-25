# overlay 版式：左中定位 / 去掉总帧数 / PROFILE 默认隐藏

- id: REQ-01M896H76GE5PDRA8VTX5CJFKT
- type: req
- status: done
- parent: REQ-01M7T0VJPG2S0RZ1HNWW0HH3HY
- verify: apps/emulator-lift/tests/ui_hud_script.cpp#ui-hud-script/settings-entry

# overlay 版式：左中定位 / 去掉总帧数 / PROFILE 默认隐藏 — 已交付

## 已交付（2026-11-25）
- HUD 面板 `x = 0`（贴左）、`y = round((720 - panelH) / 2)`（每帧按行数重算）；面板仍是 `elements[0]`。
- 删掉 `frame <n>` 行；行序 `mouse / script / fps / worst / 状态行 / settings…`（`settings…` 恒末行）。
- `PROFILE` 默认**不画也不挂 key**；按 **F9** 切换显隐。读数 = `game.overlayKeySeq`
  （`runtime/overlay_key.h` 的纯函数 `overlayKeyStep`；present 层每帧 `windows->keyDown(0x78)` 采样；
  无源 ⇒ `undefined`，⛔ 不编 0）。⛔ 不吞键（F9 仍到 guest）。
- `fill.a == 0` 的 Rect = **不画**（只作命中区；`raster.cpp`；之前它会逐像素覆盖擦掉底下的像素）。
- 像素证据按新版式重算：BMP 2764854 B / sha256
  `01468478066F2C2DC728E535569FCFA412C6266DD8954BB951A55DC18FC818E7`（旧 `3E72F989…` 作废）。

## 判据
- `apps/emulator-lift/tests/ui_hud_script.cpp#ui-hud-script/fake-readings`：面板 `x == 0`、
  `2*y + panelH ∈ {720,721}`、`settings…` 末行、按钮 `y >= panel.y + panel.h`。
- `#profile-toggle`：`overlayKeySeq` 0→1 出现 PROFILE、1→2 消失、电平不反复触发、默认不可命中。
- `apps/emulator-lift/tests/present_window.cpp#present-window`（面板左上角 = 纯面板底，用生产
  `hud.js` 的 display list 算几何，不写死魔数）。
- `apps/emulator-lift/tests/aot_overlay.cpp#aot-overlay`：三路（`--overlay` / `--plugin hud` /
  内部回退）BMP + guest dump 逐字节相同，取样点 `(0,360)`。

## 未做
- `--overlay-key=<vk|none>`（固定 F9）；F9 可能与游戏动作冲突（不吞键）。
- 面板居中会随行数跳（fps ≈1 s、worst ≈3 s 各一次），接受。
