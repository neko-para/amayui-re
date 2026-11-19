// @ts-check
// emulator-lift · 默认 HUD（`--overlay` 的脚本；随发行分发，运行时读）
//
// ★ 入口约定（`src/ui/script.h`）：脚本是 UTF-8、strict；入口必须**显式**赋给 `globalThis.render`。
//   宿主每帧调一次，调用前 builder 已清空 ⇒ 这里只追加 display list。
//   ★ 本文件是 **module**（`import`）：module 的顶层声明**不会**挂到 `globalThis` ⇒ 入口**仍然**必须
//     显式写 `globalThis.render = …`（下面那一行）。
// ★ 只用宿主的三个全局：`ui`（rect / text / measure）、`game`（只读读数）、`host`（宿主能力）。
//   ⛔ 没有 Date / Math.random（沙箱里也不存在）⇒ 同一份读数必然产出同一份 list（可测）。
// ★ 发行件：同目录必须有 `kit.js`（`import` 的相对解析以本文件所在目录为根）；缺 kit 与缺 hud
//   同属**加载期硬失败**（`--overlay` exit 2，错误里点名路径）。
// ★ 颜色：底 `#101820CC`（80% 不透明 —— 半透明底，画面还看得见）、字 `#EBFFEB`（不透明）。
// ★ 版式（1280×720 参考像素；与 `src/ui/present_ui.h` 的 `kUiFontRefPx` 同一档字号）：
//     四边内边距 PAD=8、字号 SIZE=16、
//     行高 = max(SIZE+4, ui.measure("Ag", SIZE).h + 2)、
//     面板宽 = 最长行 `ui.measure(line, SIZE).w` + 2*PAD、面板高 = 行数*行高 + 2*PAD。
//   第一行文本顶 = PAD；每行 `x = PAD`、`y = PAD + i*lineH`。
//   面板是 **elements[0]**；PROFILE 按钮画在面板**下方**（`ui-hud-script` 的结构判据）。
// ★ 行序（**拿到才写**，⛔ 不占位 —— 没值的项整行省略）：
//     mouse <x>,<y> · script <payload> · fps <一位小数> · frame <n> ·
//     `worst 3s  wall=<一位小数>ms guest=<一位小数>ms|--> shim=<一位小数>ms` ·
//     状态行（只在有过一次 PROFILE 交互之后；参与面板尺寸）
//   ⛔ **没有** `raster` / `present` 两行（用户 2026-11-18 裁决：开 overlay 时是人在体验，
//     不需要"哪条光栅 / 哪条呈现"这两个开关信息）。
// ★ PROFILE（`REQ-01M7QF01A00F9946DX0GPH23JQ` 切片 3）：第一击 `host.profileBegin`（状态行「开始」），
//   第二击 `host.profileEnd`（结束时**内部**写系统剪贴板）；四态 = 开始 / 已复制 / 失败：<error> /
//   INVALID（已复制）。★ 副作用在 render **开头**（`game.click` 自驱）：状态行要参与面板尺寸，
//   不能等按钮画完才知道结果 ⇒ `Button()` 只画外观、**不传 `on`**。
// ★ 改这份文件**不需要重编**：它是运行时读的外置文件（重跑即可，⛔ 不编进 exe）。
import { Button } from './kit.js';

const PAD = 8;
const SIZE = 16;
const BG = "#101820CC";  // 80% 不透明（alpha 204）：HUD 底下还看得见画面
const FG = "#EBFFEB";
const BUTTON_W = 112;
const BUTTON_H = 28;
const PROFILE_LABEL = "overlay";  // `host.profileBegin(label)` 的 label

// ★ 模块级状态（一个 UiScript 实例 = 一份；渲染失败 / 关闭叠层时随实例一起丢弃）。
//   `status` 一直留到下一次点击（⛔ 不按帧清：`game.frame` 是唯一时钟，按帧清会让"已复制"只闪一帧）。
const profile = { running: false, status: "" };

globalThis.render = function render() {
  // ① 副作用**先于任何 draw**：状态行要参与面板尺寸，不能等按钮画完才知道结果。
  const click = game.click;
  if (click !== undefined && click.key === "profile") {
    if (profile.running) {
      const r = host.profileEnd();
      profile.running = false;
      if (!r.ok || r.error !== "") {
        // 结束失败 / 剪贴板失败（`ok:true` + `error` 非空 = 窗口关了、文本在，但没进剪贴板）。
        profile.status = "profile: 失败：" + r.error;
      } else if (r.text.indexOf("INVALID") >= 0) {
        // INVALID 的口径 = C++ 报告文本里的同一个 token（⛔ 不在 JS 侧另立判据）。
        profile.status = "profile: INVALID（已复制）";
      } else {
        profile.status = "profile: 已复制";
      }
    } else {
      const r = host.profileBegin(PROFILE_LABEL);
      if (r.ok) {
        profile.running = true;
        profile.status = "profile: 开始";
      } else {
        profile.status = "profile: 失败：" + r.error;
      }
    }
  }

  // ② 读数行（现有行序 / 逐字文案不动）。
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
  // ★ 状态行参与面板尺寸（它是一行 HUD 文本）；idle（没交互过）时**不出这一行**。
  if (profile.status !== "") lines.push(profile.status);

  // ③ 面板 + 文本（面板是 elements[0]）。
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

  // ④ 按钮：面板**下方**（面板仍是 elements[0]）。只画外观 —— 副作用已经在 ① 做完。
  Button({ key: "profile", rect: { x: PAD, y: panelH + PAD, w: BUTTON_W, h: BUTTON_H }, label: "PROFILE" });
};
