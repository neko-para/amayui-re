// HUD：位置 → 模块数据 → render 画（v2 的分层）。
//
// 演示什么
//   · v1 = 两个具名 Trigger（afterPosition）在 onTrigger 里 `ctx.ui.show/hide`，render 每帧 `ctx.draw`；
//   · v2 = **tick 不碰 UI**：`ctx.at` 的 handler 只改模块数据（要不要显示、画什么），
//     `render(rctx)` 每帧读这份数据 + `rctx.ui` / `rctx.draw`。
//   · 为什么这样分：tick handler 的入口不打开 `in_callback_`，RunCtx 的 `ui` / `trigger` / `fail`
//     在那里都是 TypeError；能做的只有 `tick.log` / `tick.edit` / `tick.input` / `tick.patch`
//     和存下来的 RunCtx 的只读查询 + `ctx.off`。UI 状态天然属于 render。
//
// v2 新写法
//   · `ctx.at({script,lo,hi}, handler)`：区间 `[lo,hi)`；一条订阅每次命中都进 handler（没有 mode）。
//   · 模块级数据对象 = per-plugin 状态（v2 不依赖 ctx.state）。
//   · `render(rctx)` 的鼠标 / hover 用 `rctx.game.mouse()`（这一帧没有读数 ⇒ `undefined`，⛔ 不编 0）。
//
// 占位值
//   · 坐标 / 字号 / 颜色 / 文案全是占位；`lo` / `hi` 是半开区间，真实值必须 `lo < hi`；
//     `script` 必须是 marker 文件名（含 .BIN）。本文件两个区间都留 0，只为展示形状。

import type { PluginModule, RenderCtx, RunCtx } from "@amayui/lift-plugin";

const HUD_HOVER_ID = "hud:hover"; // render 写的 UI 状态键（模块数据是本文件的主数据面）

const ENTER_SCRIPT = "TITLE.BIN"; // 占位：进屏脚本
const ENTER_LO = 0x00000000; // 占位：进屏停点区间 [lo,hi)
const ENTER_HI = 0x00000000;
const LEAVE_SCRIPT = "FIELD.BIN"; // 占位：离屏的另一个正向位置
const LEAVE_LO = 0x00000000;
const LEAVE_HI = 0x00000000;

const HUD_ANCHOR_X = 0; // 占位
const HUD_ANCHOR_Y = 0;
const HUD_LINE_H = 0;
const HUD_PAD = 0;
const HUD_MIN_W = 0;
const HUD_FONT_SIZE = 0;
const HUD_FILL = ""; // 占位：颜色串
const HUD_STROKE = "";
const HUD_TEXT_FILL = "";
const HUD_HOVER_FILL = "";

/** tick handler 只改这份数据；render 每帧读它。 */
interface HudData {
    visible: boolean;
    title: string;
    lines: string[];
}

const hud: HudData = { visible: false, title: "", lines: [] }; // 占位：文本由本地化提供

let enterId: number | undefined;
let leaveId: number | undefined;

function start(ctx: RunCtx): void {
    enterId = ctx.at({ script: ENTER_SCRIPT, lo: ENTER_LO, hi: ENTER_HI }, (tick, next) => {
        hud.visible = true;
        hud.title = ""; // 占位：标题
        hud.lines = []; // 占位：正文行
        tick.log("debug", "hud: enter @" + String(tick.now.pos));
        next(); // 继续同点的后续 at handler（没有就只是示范链语义）
    });

    leaveId = ctx.at({ script: LEAVE_SCRIPT, lo: LEAVE_LO, hi: LEAVE_HI }, (tick, next) => {
        hud.visible = false;
        tick.log("debug", "hud: leave @" + String(tick.now.pos));
        next();
    });

    if (enterId === undefined || leaveId === undefined) {
        ctx.fail("hud", "ctx.at 未装（no_injection）");
    }
    ctx.log("info", "hud: subscriptions installed");
}

function render(rctx: RenderCtx): void {
    if (!hud.visible) return;

    const style = { size: HUD_FONT_SIZE, fill: HUD_TEXT_FILL };
    const measuredTitle = rctx.draw.measure(hud.title, style);
    const width = Math.max(HUD_MIN_W, measuredTitle.width + HUD_PAD * 2);
    const height = measuredTitle.height + HUD_PAD * 2 + hud.lines.length * HUD_LINE_H;

    // 这一帧没有鼠标读数 ⇒ `undefined`；没有按键源 ⇒ `buttons` 缺键。两者都按「不在 HUD 上」处理。
    const mouse = rctx.game.mouse();
    const hover =
        mouse !== undefined &&
        mouse.x >= HUD_ANCHOR_X &&
        mouse.x < HUD_ANCHOR_X + width &&
        mouse.y >= HUD_ANCHOR_Y &&
        mouse.y < HUD_ANCHOR_Y + height;

    rctx.ui.set(HUD_HOVER_ID, hover);

    // 后调用在上：先底框、再标题、再正文。
    rctx.draw.rect(HUD_ANCHOR_X, HUD_ANCHOR_Y, width, height, {
        fill: hover ? HUD_HOVER_FILL : HUD_FILL,
        stroke: HUD_STROKE,
    });
    rctx.draw.text(HUD_ANCHOR_X + HUD_PAD, HUD_ANCHOR_Y + HUD_PAD, hud.title, style);
    hud.lines.forEach((line, i) => {
        rctx.draw.text(
            HUD_ANCHOR_X + HUD_PAD,
            HUD_ANCHOR_Y + HUD_PAD + measuredTitle.height + i * HUD_LINE_H,
            line,
            style,
        );
    });
}

const plugin = { render, start } satisfies PluginModule;

export default plugin;
