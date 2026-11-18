// emulator-lift · 默认 HUD（`--overlay` 的脚本；随发行分发，运行时读）
//
// ★ 入口约定（`src/ui/script.h`）：脚本是 UTF-8、单文件、strict、无 import；入口必须**显式**
//   赋给 `globalThis.render`。宿主每帧调一次，调用前 builder 已清空 ⇒ 这里只追加 display list。
// ★ 只用宿主注入的两个全局：`ui`（rect / text / measure）与 `game`（只读 getter）。
//   ⛔ 没有 Date / Math.random（沙箱里也不存在）⇒ 同一份读数必然产出同一份 list（可测）。
// ★ 颜色：底 `#101820CC`（80% 不透明 —— 半透明底，画面还看得见）、字 `#EBFFEB`（不透明）。
// ★ 版式（1280×720 参考像素；与 `src/ui/present_ui.h` 的 `kUiFontRefPx` 同一档字号）：
//     四边内边距 PAD=8、字号 SIZE=16、
//     行高 = max(SIZE+4, ui.measure("Ag", SIZE).h + 2)、
//     面板宽 = 最长行 `ui.measure(line, SIZE).w` + 2*PAD、面板高 = 行数*行高 + 2*PAD。
//   第一行文本顶 = PAD；每行 `x = PAD`、`y = PAD + i*lineH`。
// ★ 行序（**拿到才写**，⛔ 不占位 —— 没值的项整行省略）：
//     mouse <x>,<y> · script <payload> · fps <一位小数> · frame <n> ·
//     `worst 3s  wall=<一位小数>ms guest=<一位小数>ms|--> shim=<一位小数>ms`
//   ⛔ **没有** `raster` / `present` 两行（用户 2026-11-18 裁决：开 overlay 时是人在体验，
//     不需要"哪条光栅 / 哪条呈现"这两个开关信息）。
// ★ 改这份文件**不需要重编**：它是运行时读的外置文件（重跑即可，⛔ 不编进 exe）。
const PAD = 8;
const SIZE = 16;
const BG = "#101820CC";  // 80% 不透明（alpha 204）：HUD 底下还看得见画面
const FG = "#EBFFEB";

globalThis.render = function render() {
  const lines = [];
  const mouse = game.mouse;
  if (mouse !== undefined) lines.push("mouse " + mouse.x + "," + mouse.y);
  // ★ 栈顶脚本名：`game.local_frames()` 是 top-first，但 `cur` 那一格**可能不活动**
  //   （列表照给、`top` 全 false，见 README「帧列表」）—— 那时 `frames[0]` 只是"最高活动槽"，
  //   ⛔ 不是栈顶。⇒ 只认 `top === true` 的那一项；没有就回退 `game.script`（当前帧身份）。
  const frames = game.local_frames();
  let top;
  if (frames !== undefined) {
    for (let i = 0; i < frames.length; ++i) {
      if (frames[i].top) {
        top = frames[i].script;
        break;
      }
    }
  }
  const script = top !== undefined ? top : game.script;
  if (script !== undefined) lines.push("script " + script);
  const fps = game.fps;
  if (fps !== undefined) lines.push("fps " + fps.toFixed(1));
  // ★ frame 总是有值（宿主每帧都给）⇒ 这一行**总是**在。
  lines.push("frame " + game.frame);
  const worst = game.worst;
  if (worst !== undefined) {
    // `guest` 没开块计时时**没有 guest_ms 这个键** ⇒ 写 `--`（⛔ 不写 0.0 冒充读数）。
    const guest = worst.guest_ms;
    lines.push("worst 3s  wall=" + worst.wall_ms.toFixed(1) + "ms guest=" +
               (guest === undefined ? "--" : guest.toFixed(1) + "ms") +
               " shim=" + worst.shim_ms.toFixed(1) + "ms");
  }

  const lineH = Math.max(SIZE + 4, ui.measure("Ag", SIZE).h + 2);
  let maxW = 0;
  for (let i = 0; i < lines.length; ++i) {
    const w = ui.measure(lines[i], SIZE).w;
    if (w > maxW) maxW = w;
  }
  const panelW = maxW + 2 * PAD;
  const panelH = lines.length * lineH + 2 * PAD;
  ui.rect({ x: 0, y: 0, w: panelW, h: panelH, fill: BG });
  for (let i = 0; i < lines.length; ++i) {
    ui.text({ x: PAD, y: PAD + i * lineH, text: lines[i], size: SIZE, color: FG });
  }
};
