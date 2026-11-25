// @ts-check
// emulator-lift · 默认 HUD（`--overlay` 的 HUD；随发行分发，运行时读）
//
// ★ S2 起本文件是**插件模块**：`export default { render(ctx) }`（`--overlay` = `--plugin hud` 预设；
//   契约见 `scripts/plugins/plugin.d.ts` 的 `RenderCtx` / `docs/03-emulator/plugin-runtime.md` §4.6）。
//   ★ **仍保留一句旧入口** `globalThis.render`：`--ui <path>`（deprecated，S3 删）走的是旧 UiScript
//     loader（`globalThis.render` + 全局 `ui/game/host`）。两个入口共用**同一份绘制体**（`drawHud`），
//     只换一个薄适配器 ⇒ 不可能"旧版画一套、插件版画另一套"。
// ★ 入口约定（旧 loader，`src/ui/script.h`）：脚本是 UTF-8、strict；入口必须**显式**赋给
//   `globalThis.render`。宿主每帧调一次，调用前 builder 已清空 ⇒ 这里只追加 display list。
//   ★ 本文件是 **module**（`import`）：module 的顶层声明**不会**挂到 `globalThis` ⇒ 旧入口**仍然**必须
//     显式写 `globalThis.render = …`（下面）。
// ★ 两个入口的能力面（同一份读数，逐条同口径）：
//     · 插件：`ctx.draw.*` / `ctx.game.*` / `ctx.profile.*`；
//     · 旧 `--ui`：全局 `ui.*` / `game.*` / `host.profileBegin|End`。
//   ⛔ 没有 Date / Math.random（沙箱里也不存在）⇒ 同一份读数必然产出同一份 list（可测）。
// ★ 发行件：同目录必须有 `kit.js`（`import` 的相对解析以本文件所在目录为根）；缺 kit 与缺 hud
//   同属**加载期硬失败**（`--overlay` / `--plugin hud` exit 2，错误里点名路径）。
// ★ 颜色：底 `#101820CC`（80% 不透明 —— 半透明底，画面还看得见）、字 `#EBFFEB`（不透明）。
// ★ 版式（1280×720 参考像素；与 `src/ui/present_ui.h` 的 `kUiFontRefPx` 同一档字号）：
//     四边内边距 PAD=8、字号 SIZE=16、
//     行高 = max(SIZE+4, measure("Ag", SIZE).h + 2)、
//     面板宽 = 最长行 `measure(line, SIZE).w` + 2*PAD、面板高 = 行数*行高 + 2*PAD。
//   第一行文本顶 = PAD；每行 `x = PAD`、`y = PAD + i*lineH`。
//   面板是 **elements[0]**；PROFILE 按钮画在面板**下方**（`ui-hud-script` 的结构判据）。
// ★ 行序（**拿到才写**，⛔ 不占位 —— 没值的项整行省略）：
//     mouse <x>,<y> · script <payload> · fps <一位小数> · frame <n> ·
//     `worst 3s  wall=<一位小数>ms guest=<一位小数>ms|--> shim=<一位小数>ms` ·
//     状态行（只在有过一次 PROFILE 交互之后；参与面板尺寸）
//   ⛔ **没有** `raster` / `present` 两行（用户 2026-11-18 裁决：开 overlay 时是人在体验，
//     不需要"哪条光栅 / 哪条呈现"这两个开关信息）。
// ★ PROFILE（`REQ-01M7QF01A00F9946DX0GPH23JQ` 切片 3）：第一击 `profile.begin`（状态行「开始」），
//   第二击 `profile.end`（结束时**内部**写系统剪贴板）；四态 = 开始 / 已复制 / 失败：<error> /
//   INVALID（已复制）。★ 副作用在 render **开头**（`game.click` 自驱）：状态行要参与面板尺寸，
//   不能等按钮画完才知道结果 ⇒ `Button()` 只画外观、**不传 `on`**。
//   ★ S2 复审 I3：插件侧 `ctx.profile.end()` 回传 `{ok,text,error}`（与旧 `host.profileEnd()` 同形）
//     ⇒ `--overlay` / `--plugin hud` 也走四态；报告文本**不**再额外写 stdout。
// ★ 改这份文件**不需要重编**：它是运行时读的外置文件（重跑即可，⛔ 不编进 exe）。
import { Button } from './kit.js';

const PAD = 8;
const SIZE = 16;
const BG = "#101820CC";  // 80% 不透明（alpha 204）：HUD 底下还看得见画面
const FG = "#EBFFEB";
const BUTTON_W = 112;
const BUTTON_H = 28;
const PROFILE_LABEL = "overlay";  // `profile.begin(label)` 的 label

// 两个入口共用的**薄适配器**形状（绘制体只认它；⛔ 不直接摸全局 / ctx）。
/** @typedef {{ x: number, y: number, w: number, h: number, fill: string, key?: string }} HudRect */
/** @typedef {{ x: number, y: number, text: string, size: number, color: string, bold?: boolean }} HudText */
/** @typedef {{ size: number, fill: string, bold?: boolean }} HudTextStyle */
/**
 * @typedef {{
 *   measure: (text: string, size: number) => { w: number, h: number },
 *   rect: (o: HudRect) => void,
 *   text: (o: HudText) => void,
 *   mouse: () => any,
 *   frames: () => any,
 *   frame: number,
 *   script?: string,
 *   fps?: number,
 *   worst?: any,
 *   click?: any,
 *   hover?: string,
 *   active?: string,
 *   profileBegin: (label: string) => { ok: boolean, error: string },
 *   profileEnd: () => { ok: boolean, text: string, error: string },
 * }} HudHost
 */

// ★ 模块级状态（一个脚本实例 = 一份；渲染失败 / 关闭叠层时随实例一起丢弃）。
//   `status` 一直留到下一次点击（⛔ 不按帧清：`frame` 是唯一时钟，按帧清会让"已复制"只闪一帧）。
const profile = { running: false, status: "" };

// ---------------------------------------------------------------------------------------
// 旧 `--ui` loader 的适配器：**只有它**知道全局 `ui` / `game` / `host`。
//   （插件 loader 也会装这些全局，但插件路径**不走这里** —— 走 ctx，契约才是真的。）
// ---------------------------------------------------------------------------------------
/** @returns {HudHost} */
function classicHost() {
  return {
    measure: (text, size) => ui.measure(text, size),
    rect: (o) => ui.rect(o),
    text: (o) => ui.text(o),
    mouse: () => game.mouse,
    frames: () => game.local_frames(),
    get frame() { return game.frame; },
    get script() { return game.script; },
    get fps() { return game.fps; },
    get worst() { return game.worst; },
    get click() { return game.click; },
    get hover() { return game.hover; },
    get active() { return game.active; },
    profileBegin: (label) => host.profileBegin(label),
    profileEnd: () => host.profileEnd(),
  };
}

// ---------------------------------------------------------------------------------------
// 插件适配器：`ctx` → 同一份绘制面（`draw` / `game` / `profile`）。
//   ★ 形状差异全部**只在**这里：measure 的 `{width,height}` ↔ `{w,h}`；`mouse()` 方法；
//     profile 的 `void` + TypeError ↔ `{ok,error}` 结果对象。
//   ★ 失败文本剥掉 `ctx.profile.*: ` 前缀 ⇒ 与旧 `host.profile*` 的 `error` 字段同形（HUD 文案不变）。
// ---------------------------------------------------------------------------------------
/**
 * @param {any} ctx
 * @returns {HudHost}
 */
function pluginHost(ctx) {
  /** @param {any} e */
  const errorText = (e) => {
    let text = e !== null && e !== undefined && typeof e.message === "string" ? e.message : String(e);
    // 剥掉 `ctx.profile.<name>: ` 前缀（⛔ 不用正则：沙箱里没有 RegExp）。
    if (text.indexOf("ctx.profile.") === 0) {
      const colon = text.indexOf(": ");
      if (colon >= 0) text = text.slice(colon + 2);
    }
    return text;
  };
  return {
    measure: (text, size) => {
      const m = ctx.draw.measure(text, { size });
      return { w: m.width, h: m.height };
    },
    rect: (o) => {
      /** @type {{ fill: string, key?: string }} */
      const style = { fill: o.fill };
      if (o.key !== undefined) style.key = o.key;
      ctx.draw.rect(o.x, o.y, o.w, o.h, style);
    },
    text: (o) => {
      /** @type {HudTextStyle} */
      const style = { size: o.size, fill: o.color };
      if (o.bold !== undefined) style.bold = o.bold;
      ctx.draw.text(o.x, o.y, o.text, style);
    },
    mouse: () => ctx.game.mouse(),
    frames: () => ctx.game.localFrames(),
    get frame() { return ctx.game.frame; },
    get script() { return ctx.game.script; },
    get fps() { return ctx.game.fps; },
    get worst() { return ctx.game.worst; },
    get click() { return ctx.game.click; },
    get hover() { return ctx.game.hover; },
    get active() { return ctx.game.active; },
    profileBegin: (label) => {
      try {
        ctx.profile.begin(label);
        return { ok: true, error: "" };
      } catch (e) {
        return { ok: false, error: errorText(e) };
      }
    },
    profileEnd: () => {
      // ★ S2 复审 I3：插件的 `ctx.profile.end()` 返回与旧 `host.profileEnd()` 同形的
      //   `{ok,text,error}` ⇒ 四态（含 `INVALID（已复制）`）与 `--ui` 完全一致。
      return ctx.profile.end();
    },
  };
}

/**
 * 绘制体（两个入口共用）：`h` 是上面两种适配器之一。
 * @param {HudHost} h
 */
function drawHud(h) {
  // ① 副作用**先于任何 draw**：状态行要参与面板尺寸，不能等按钮画完才知道结果。
  const click = h.click;
  if (click !== undefined && click.key === "profile") {
    if (profile.running) {
      const r = h.profileEnd();
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
      const r = h.profileBegin(PROFILE_LABEL);
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
  const mouse = h.mouse();
  if (mouse !== undefined) lines.push("mouse " + mouse.x + "," + mouse.y);
  // ★ 栈顶脚本名：`frames()` 是 top-first，但 `cur` 那一格**可能不活动**
  //   （列表照给、`top` 全 false，见 README「帧列表」）—— 那时 `frames[0]` 只是"最高活动槽"，
  //   ⛔ 不是栈顶。⇒ 只认 `top === true` 的那一项；没有就回退 `h.script`（当前帧身份）。
  const frames = h.frames();
  let top;
  if (frames !== undefined) {
    for (let i = 0; i < frames.length; ++i) {
      if (frames[i].top) {
        top = frames[i].script;
        break;
      }
    }
  }
  const script = top !== undefined ? top : h.script;
  if (script !== undefined) lines.push("script " + script);
  const fps = h.fps;
  if (fps !== undefined) lines.push("fps " + fps.toFixed(1));
  // ★ frame 总是有值（宿主每帧都给）⇒ 这一行**总是**在。
  lines.push("frame " + h.frame);
  const worst = h.worst;
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
  const lineH = Math.max(SIZE + 4, h.measure("Ag", SIZE).h + 2);
  let maxW = 0;
  for (let i = 0; i < lines.length; ++i) {
    const w = h.measure(lines[i], SIZE).w;
    if (w > maxW) maxW = w;
  }
  const panelW = maxW + 2 * PAD;
  const panelH = lines.length * lineH + 2 * PAD;
  h.rect({ x: 0, y: 0, w: panelW, h: panelH, fill: BG });
  for (let i = 0; i < lines.length; ++i) {
    h.text({ x: PAD, y: PAD + i * lineH, text: lines[i], size: SIZE, color: FG });
  }

  // ④ 按钮：面板**下方**（面板仍是 elements[0]）。只画外观 —— 副作用已经在 ① 做完。
  Button({ key: "profile", rect: { x: PAD, y: panelH + PAD, w: BUTTON_W, h: BUTTON_H }, label: "PROFILE" }, h);
}

// ---- 旧入口（`--ui` 的 UiScript loader；S3 随 `--ui` 一起删）----
globalThis.render = function render() {
  drawHud(classicHost());
};

// ---- 新入口（`--plugin hud` / `--overlay` 预设）----
export default {
  /** @param {any} ctx */
  render(ctx) {
    drawHud(pluginHost(ctx));
  },
};
