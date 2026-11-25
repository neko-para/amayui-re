// @ts-check
// emulator-lift · 默认 HUD（`--overlay` 的 HUD；随发行分发，运行时读）
//
// ★ S2 起本文件是**插件模块**：`export default { render(ctx) }`（`--overlay` = `--plugin hud` 预设；
//   契约见 `scripts/plugins/plugin.d.ts` 的 `RenderCtx` / `docs/03-emulator/plugin-runtime.md` §4.6）。
//   ★ **仍保留一句旧入口** `globalThis.render`：**内部 hud 回退**走的是旧 UiScript
//     loader（`--overlay` + 无 render 的 `--plugin`；`--ui` 已从 CLI 面删除）。两个入口共用**同一份绘制体**（`drawHud`），
//     只换一个薄适配器 ⇒ 不可能"旧版画一套、插件版画另一套"。
// ★ 入口约定（旧 loader，`src/ui/script.h`）：脚本是 UTF-8、strict；入口必须**显式**赋给
//   `globalThis.render`。宿主每帧调一次，调用前 builder 已清空 ⇒ 这里只追加 display list。
//   ★ 本文件是 **module**（`import`）：module 的顶层声明**不会**挂到 `globalThis` ⇒ 旧入口**仍然**必须
//     显式写 `globalThis.render = …`（下面）。
// ★ 两个入口的能力面（同一份读数，逐条同口径）：
//     · 插件：`ctx.draw.*` / `ctx.game.*` / `ctx.profile.*`（**没有** `config`：`RenderCtx` 是 7 成员）；
//     · 内部 hud 回退（classic）：全局 `ui.*` / `game.*` / `host.profileBegin|End` / `host.config`。
//   ⛔ 没有 Date / Math.random（沙箱里也不存在）⇒ 同一份读数必然产出同一份 list（可测）。
// ★ 发行件：同目录必须有 `kit.js` 与 `settings-page.js`（`import` 的相对解析以本文件所在目录为根）；
//   缺任一与缺 hud 同属**加载期硬失败**（`--overlay` / `--plugin hud` exit 2，错误里点名路径）。
// ★ 颜色：底 `#101820CC`（80% 不透明 —— 半透明底，画面还看得见）、字 `#EBFFEB`（不透明）。
// ★ 版式（1280×720 参考像素；与 `src/ui/present_ui.h` 的 `kUiFontRefPx` 同一档字号）：
//     四边内边距 PAD=8、字号 SIZE=16、
//     行高 = max(SIZE+4, measure("Ag", SIZE).h + 2)、
//     面板宽 = 最长行 `measure(line, SIZE).w` + 2*PAD、面板高 = 行数*行高 + 2*PAD。
//   ★ D4（2026-11-25 父级裁决）：面板**贴左、垂直居中** —— `x = 0`、
//     `y = round((720 - panelH) / 2)`（面板高随行数变 ⇒ **每帧**重算）。
//   第一行文本顶 = panelY + PAD；每行 `x = PAD`、`y = panelY + PAD + i*lineH`。
//   面板是 **elements[0]**；命中区（`settings…` 那一行 / PROFILE 按钮）一律画在面板**之后**。
// ★ 行序（**拿到才写**，⛔ 不占位 —— 没值的项整行省略；`settings…` **恒在最后一行**）：
//     mouse <x>,<y> · script <payload> · fps <一位小数> ·
//     `worst 3s  wall=<一位小数>ms guest=<一位小数>ms|--> shim=<一位小数>ms` ·
//     状态行（PROFILE 交互 / 设置入口）· `settings…`
//   ⛔ **没有** `frame <n>` 行（总帧数无诊断价值；2026-11-25 D4 删除）。
//   ⛔ **没有** `raster` / `present` 两行（用户 2026-11-18 裁决：开 overlay 时是人在体验，
//     不需要"哪条光栅 / 哪条呈现"这两个开关信息）。
// ★ 设置入口：最后一行 `settings…`（16px / FG）+ 一条**透明命中 Rect**（`fill:"#00000000"`、
//   `key:"settings"`）刚好盖住那一行。点击 ⇒ 打开二级页（`settings-page.js` 的 `drawPage`）；
//   打开期间**不画 HUD 面板**，只画页面（`settings…` 行与 PROFILE 都不画；页面自己的 BACK 关闭）。
//   ★ `h.config` 不可用（`undefined` / `available !== true`）⇒ **不打开**，状态行
//     `settings: 没有 appdata`。
//   ★ 桥可用但 `settings.schema` 里**一个可画项都没有**（空表 / 全是坏形状）⇒ **也不打开**，
//     状态行 `settings: 没有注册设置`（⛔ 不打开一个空白页、⛔ 不造死控件）。
//     两条都是"同一个脚本实例内改回来才清"。
//   ★ 设置页的状态行是**模块级**（`settingsPage.status`，由 `drawPage` 每帧写回）：
//     成功点（`applied:true`）清空；失败（`settings: 只读` / `settings: <错误>`）留到下一次操作
//     或下一次成功；**关闭页面不清**（状态行留在 HUD 上）。⛔ 不每帧新建对象 —— 那样失败回执
//     只在自己那一帧可见（≈16ms，人看不见），只读 appdata 下点开关就是"零反馈"。
// ★ PROFILE（`REQ-01M7QF01A00F9946DX0GPH23JQ` 切片 3）：第一击 `profile.begin`（状态行「开始」），
//   第二击 `profile.end`（结束时**内部**写系统剪贴板）；四态 = 开始 / 已复制 / 失败：<error> /
//   INVALID（已复制）。★ 副作用在 render **开头**（`game.click` 自驱）：状态行要参与面板尺寸，
//   不能等按钮画完才知道结果 ⇒ `Button()` 只画外观、**不传 `on`**。
//   ★ S2 复审 I3：插件侧 `ctx.profile.end()` 回传 `{ok,text,error}`（与旧 `host.profileEnd()` 同形）
//     ⇒ `--overlay` / `--plugin hud` 也走四态；报告文本**不**再额外写 stdout。
//   ★ D4：PROFILE **默认隐藏**（不画、也不挂 `key` ⇒ 不可命中）。按键 = 固定 **F9**，读数 =
//     `h.overlayKeySeq`（无源 ⇒ `undefined`）。模块级记住上一次的值，**变了才切换**显隐
//     （⛔ 当电平用 —— 电平每帧都触发）。显示时按钮画在面板**下方**
//     （`y = panelY + panelH + PAD`），`key:"profile"`，尺寸 / 主题沿用 `kit.Button`。
//   ★ F9 **不吞键**（它仍到 guest）：与游戏的按键动作可能同时触发，冲突风险记在
//     `docs/03-emulator/overlay.md`（R10 / 版式段）。
// ★ 改这份文件**不需要重编**：它是运行时读的外置文件（重跑即可，⛔ 不编进 exe）。
import { Button } from './kit.js';
import { canOpen, drawPage, hasSettings } from './settings-page.js';

const PAD = 8;
const SIZE = 16;
const BG = "#101820CC";  // 80% 不透明（alpha 204）：HUD 底下还看得见画面
const FG = "#EBFFEB";
const BUTTON_W = 112;
const BUTTON_H = 28;
const PROFILE_LABEL = "overlay";  // `profile.begin(label)` 的 label
/** 面板最后一行（设置入口）的文案。 */
const SETTINGS_LINE = "settings…";
/** 设置入口的命中键（`ui.rect({key})`；透明 Rect 刚好盖住那一行）。 */
const SETTINGS_KEY = "settings";
/** `h.config` 不可用时留在状态行上的那句话（D4；同一个 session 内改回来才清）。 */
const NO_APPDATA = "settings: 没有 appdata";
/** 桥可用但 `settings.schema` 空表时留在状态行上的那句话（⛔ 不打开空白页；改回来才清）。 */
const NO_SETTINGS = "settings: 没有注册设置";

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
 *   overlayKeySeq?: number,
 *   config?: any,
 *   profileBegin: (label: string) => { ok: boolean, error: string },
 *   profileEnd: () => { ok: boolean, text: string, error: string },
 * }} HudHost
 */

// ★ 模块级状态（一个脚本实例 = 一份；渲染失败 / 关闭叠层时随实例一起丢弃）。
//   `status` 一直留到下一次点击（⛔ 不按帧清：`frame` 是唯一时钟，按帧清会让"已复制"只闪一帧）。
const profile = { running: false, status: "" };
/** 设置二级页是否打开（D4）。 */
let settingsOpen = false;
/** 上一次看到的 `overlayKeySeq`（`undefined` = 还没有可用的源）。 */
/** @type {number | undefined} */
let lastKeySeq;
/**
 * 设置状态行（**模块级**，`settings-page.js` 的 `drawPage` 每帧把 `status` 写回这个对象）。
 *
 * ★ ⛔ 不每帧新建（`const page = { status: "" }`）：那样 `applied:false` / `set` 抛错的回执
 *   只在自己那一帧可见（≈16ms，人看不见）—— 只读 appdata 下点开关就是"零反馈"。
 * 规则：成功点（`applied:true`）清空；失败保留到下一次操作或下一次成功；关闭页面**不清**
 *   （状态行留在 HUD 上）。两条"打不开"的原因（`NO_APPDATA` / `NO_SETTINGS`）也放这里。
 */
const settingsPage = { status: "" };
/** PROFILE 按钮是否可见（默认隐藏；F9 的读数**变了**才切换）。 */
let profileVisible = false;

// ---------------------------------------------------------------------------------------
// 内部 hud 回退 loader 的适配器：**只有它**知道全局 `ui` / `game` / `host`。
//   （插件 loader 也会装这些全局，但插件路径**不走这里** —— 走 ctx，契约才是真的。）
// ★ `config` / `overlayKeySeq` 是 **getter**：每帧现读（⛔ 构造时冻住 —— `host.config` 的桥
//   可能这一帧才接上 / 这一帧断了；`overlayKeySeq` 也可能这一帧还没有源）。
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
    get overlayKeySeq() { return game.overlayKeySeq; },
    get config() { return host.config; },
    profileBegin: (label) => host.profileBegin(label),
    profileEnd: () => host.profileEnd(),
  };
}

// ---------------------------------------------------------------------------------------
// 插件适配器：`ctx` → 同一份绘制面（`draw` / `game` / `profile`）。
//   ★ 形状差异全部**只在**这里：measure 的 `{width,height}` ↔ `{w,h}`；`mouse()` 方法；
//     profile 的 `void` + TypeError ↔ `{ok,error}` 结果对象。
//   ★ 失败文本剥掉 `ctx.profile.*: ` 前缀 ⇒ 与旧 `host.profile*` 的 `error` 字段同形（HUD 文案不变）。
//   ★ D2 收窄：`RenderCtx` **没有** `config`（7 成员）⇒ 插件档 `h.config` 恒 `undefined`
//     （预设模式 `--plugin hud` 没有插件注册 schema ⇒ 设置页不可用是诚实的）。
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
    get overlayKeySeq() { return ctx.game.overlayKeySeq; },
    config: undefined,
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
      //   `{ok,text,error}` ⇒ 四态（含 `INVALID（已复制）`）与旧入口完全一致。
      return ctx.profile.end();
    },
  };
}

/**
 * F9 的显隐切换：读数是**单调按下沿计数** ⇒ 只有"值变了"才切一次（⛔ 当电平用）。
 *
 * ★ 没有源（`undefined`）⇒ 忘掉基线：下一次拿到值时只建立基线、不切换
 *   （中断期间按过几次无从得知，⛔ 不猜）。
 *
 * @param {HudHost} h 宿主适配器。
 */
function toggleProfileOnKey(h) {
  const seq = h.overlayKeySeq;
  if (seq === undefined) {
    lastKeySeq = undefined;
    return;
  }
  if (lastKeySeq !== undefined && seq !== lastKeySeq) profileVisible = !profileVisible;
  lastKeySeq = seq;
}

/**
 * 画设置二级页（**只**画页面；打开期间不画 HUD 面板）。
 *
 * @param {HudHost} h 宿主适配器（`canOpen(h)` 已为真）。
 * @returns {boolean} true = 这一帧已经画了页面；false = 页面已关闭（调用方继续画 HUD）。
 */
function drawSettingsPage(h) {
  // ★ 页面状态由本文件持有（`settings-page.js` 无模块级状态，只把 `status` 写回这个对象）。
  //   ⛔ 不每帧新建：失败回执要留到下一次操作 / 下一次成功（见 `settingsPage` 的注释）。
  const r = drawPage(h, settingsPage);
  if (r.closed === true) {
    settingsOpen = false;
    return false;
  }
  return true;
}

/**
 * 绘制体（两个入口共用）：`h` 是上面两种适配器之一。
 * @param {HudHost} h
 */
function drawHud(h) {
  // ① 读数 / 门（先于任何 draw）：F9 的显隐切换、设置入口、页面开关。
  toggleProfileOnKey(h);

  const click = h.click;
  // `h.config` 每帧现读：`available` 可能从 false 变 true（同一个 session 内）⇒ 才清那句状态。
  const configReady = canOpen(h);
  // ★ 桥可用 ≠ 有东西可画：`settings.schema` 空表 ⇒ 打开就是**空白页**（⛔ 不造死控件）。
  const settingsReady = configReady && hasSettings(h);
  if (configReady && settingsPage.status === NO_APPDATA) settingsPage.status = "";
  if (settingsReady && settingsPage.status === NO_SETTINGS) settingsPage.status = "";

  // 页开着 ⇒ 只画页面（HUD 面板 / `settings…` / PROFILE 都不画）。
  if (settingsOpen) {
    if (!settingsReady) {
      // 桥在这一帧断了 / 注册表空了（例如 appdata 被撤 / 插件全退）：回到 HUD + 一行原因。
      settingsOpen = false;
      settingsPage.status = configReady ? NO_SETTINGS : NO_APPDATA;
    } else if (drawSettingsPage(h)) {
      return;
    }
  }

  // 设置入口：最后一行那一块透明 Rect 的命中键。config 不可用 / 注册表空 ⇒ 不打开、留一句状态。
  // ★ 打开**不**清状态：失败回执要留到下一次操作（页面里那一行也会显示它）。
  if (click !== undefined && click.key === SETTINGS_KEY) {
    if (!configReady) {
      settingsPage.status = NO_APPDATA;
    } else if (!settingsReady) {
      settingsPage.status = NO_SETTINGS;
    } else {
      settingsOpen = true;
      if (drawSettingsPage(h)) return;  // 同一帧就换绘制面（点开不延迟一帧）
    }
  }

  // ② PROFILE 的状态机（副作用**先于任何 draw**：状态行要参与面板尺寸）。
  if (profileVisible && click !== undefined && click.key === "profile") {
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

  // ③ 读数行（D4 的行序；`settings…` 恒在最后一行）。
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
  const worst = h.worst;
  if (worst !== undefined) {
    // `guest` 没开块计时时**没有 guest_ms 这个键** ⇒ 写 `--`（⛔ 不写 0.0 冒充读数）。
    const guest = worst.guest_ms;
    lines.push("worst 3s  wall=" + worst.wall_ms.toFixed(1) + "ms guest=" +
               (guest === undefined ? "--" : guest.toFixed(1) + "ms") +
               " shim=" + worst.shim_ms.toFixed(1) + "ms");
  }
  // ★ 状态行参与面板尺寸（它是一行 HUD 文本）。PROFILE 的状态优先（它是一次点击的直接回执）；
  //   设置那句在它空着时才显示。
  if (profile.status !== "") {
    lines.push(profile.status);
  } else if (settingsPage.status !== "") {
    lines.push(settingsPage.status);
  }
  // ★ `settings…` 恒在最后一行（D4）—— 设置入口的可点击文案。
  lines.push(SETTINGS_LINE);

  // ④ 面板 + 文本（面板是 elements[0]；贴左、垂直居中）。
  const lineH = Math.max(SIZE + 4, h.measure("Ag", SIZE).h + 2);
  let maxW = 0;
  for (let i = 0; i < lines.length; ++i) {
    const w = h.measure(lines[i], SIZE).w;
    if (w > maxW) maxW = w;
  }
  const panelW = maxW + 2 * PAD;
  const panelH = lines.length * lineH + 2 * PAD;
  const panelY = Math.round((720 - panelH) / 2);
  h.rect({ x: 0, y: panelY, w: panelW, h: panelH, fill: BG });
  for (let i = 0; i < lines.length; ++i) {
    h.text({ x: PAD, y: panelY + PAD + i * lineH, text: lines[i], size: SIZE, color: FG });
  }
  // ⑤ 设置入口：透明命中 Rect 刚好盖住**最后一行**（画在面板之后 ⇒ 不遮文本、可命中）。
  const settingsY = panelY + PAD + (lines.length - 1) * lineH;
  h.rect({ x: 0, y: settingsY, w: panelW, h: lineH, fill: "#00000000", key: SETTINGS_KEY });

  // ⑥ PROFILE：默认不画（也不挂 key）；F9 打开后画在面板**下方**。只画外观 —— 副作用已在 ② 做完。
  if (profileVisible) {
    Button({ key: "profile", rect: { x: PAD, y: panelY + panelH + PAD, w: BUTTON_W, h: BUTTON_H }, label: "PROFILE" }, h);
  }
}

// ---- 内部 hud 回退入口（旧 UiScript loader；S3 删的是 CLI `--ui`，这条内部路径保留）----
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
