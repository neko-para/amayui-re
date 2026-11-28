// 为什么这个例子没有 dispatch（v2）：**dispatch 属控制层，不在插件 API 里**。
//
// 演示什么
//   · v1 的「UI 按钮按下时 dispatch 一个子 Run」在 v2 被拆成两件事：
//       ① 子流程的**内容** = 同一个 Run 里的 `ctx.task` / `ctx.at` / `ctx.once`（组合，不新开 Run）；
//       ② 「换一个 Run」= 当前 Run 结束（`finish` / `quit` / `fail`，都只能在 Run 回调里），
//          由控制层（emu-ctl / UI 宿主）dispatch 下一个。
//   · 插件里 ⛔ 没有 `ctx.run.dispatch`，也 ⛔ 不允许嵌套 Run；`RenderCtx.run` 声明保留、运行期不安装。
//
// v2 新写法
//   · `ctx.task(async (t) => …)`：把一段「子配方」内联进当前 Run，共享同一个 ctx / 生命周期。
//   · `ctx.at(where, handler)`：进度用订阅表达；要停就 `ctx.off(id)`（只减，不是 abort 一个 Run）。
//   · `render(rctx)` 只读模块数据 + `rctx.draw` / `rctx.ui`；**tick / task 不碰 UI**。
//     按钮的点击在 render 里发生 —— 它能做的最多是用存下来的 RunCtx 调 `ctx.off`，
//     ⛔ 不能 dispatch、不能 `ctx.task`、不能 `finish`（那些门 = Run 回调）。
//
// 占位值
//   · 坐标 / 颜色 / 文案 / 脚本位置全是占位；`script` 真实写法必须是 marker 文件名（含 .BIN）。

import type { PluginModule, RenderCtx, RunCtx } from "@amayui/lift-plugin";

const CHILD_POINT = { script: "SAVE.BIN", pos: 0x00000000 } as const; // 占位：子流程等的指令点
const WATCH_RANGE = { script: "FIELD.BIN", lo: 0x00000000, hi: 0x00000000 } as const; // 占位：进度区间

const CHILD_TIMEOUT_MS = 30_000; // 占位：once 的失败截止

const BUTTON_X = 0; // 占位：按钮矩形
const BUTTON_Y = 0;
const BUTTON_W = 0;
const BUTTON_H = 0;
const FONT_SIZE = 0; // 占位
const BUTTON_FILL = ""; // 占位：颜色串
const BUTTON_FILL_HOVER = ""; // 占位
const BUTTON_STROKE = ""; // 占位
const TEXT_FILL = ""; // 占位

const HOVER_ID = "dispatch:hover"; // render 写的 UI 状态键

type Phase = "idle" | "waiting" | "done" | "failed";

/** 模块级变量 = per-plugin 状态（render / task / at handler 共享）——v2 不依赖 ctx.state。 */
const child: { phase: Phase; hits: number } = { phase: "idle", hits: 0 };

/** start 存下来的 RunCtx：render 里只有 `off` 这一个「只减」操作允许跨入口用。 */
let runCtx: RunCtx | undefined;
let watchId: number | undefined;
let mouseWasDown = false;

function inButton(x: number, y: number): boolean {
    return x >= BUTTON_X && x < BUTTON_X + BUTTON_W && y >= BUTTON_Y && y < BUTTON_Y + BUTTON_H;
}

function start(ctx: RunCtx): void {
    runCtx = ctx;
    child.phase = "waiting";

    // 「子流程」= 同一 Run 内的组合，不是第二个 Run。
    ctx.task(async (t) => {
        try {
            const tick = await t.once(CHILD_POINT, { timeoutMs: CHILD_TIMEOUT_MS });
            child.phase = "done";
            log.info("info", "composed child: reached " + tick.now.script + " @" + String(tick.now.pos));
        } catch (err) {
            child.phase = "failed";
            // 想让 Run 以这条原因失败：重新抛出即可（task 拒绝 ⇒ 宿主 fail Run）。
            throw err;
        }
    });

    // 进度观察：一条 at 订阅。按钮要「停」的是它，而不是 dispatch 一个子 Run。
    watchId = ctx.at(WATCH_RANGE, (tick, next) => {
        child.hits += 1;
        next(); // 不调 = 截断后面同点的 at handler（本文件没有下一条，写在这里是示范链语义）
    });
    if (watchId === undefined) ctx.fail("dispatch", "ctx.at 未装（no_injection）");

    log.info("info", "dispatch: composed in one Run; no nested Run / no dispatch");
}

function render(rctx: RenderCtx): void {
    // 两个「没有值」的口都要判：这一帧没有鼠标读数 ⇒ `mouse()` undefined；没有按键源 ⇒ `buttons` 缺键。
    const mouse = rctx.game.mouse();
    const down = mouse?.buttons?.left ?? false;
    const hover = mouse !== undefined && inButton(mouse.x, mouse.y);
    const pressed = down && !mouseWasDown && hover;
    mouseWasDown = down;

    if (pressed) {
        // ⛔ 这里不是 dispatch。按钮只能撤订阅；「换一个 Run」是控制层的事。
        log.info("dispatch: button pressed — dispatch 属控制层，插件只做 ctx.off");
        if (runCtx !== undefined && watchId !== undefined) {
            runCtx.off(watchId);
            watchId = undefined;
        }
    }

    rctx.ui.set(HOVER_ID, hover);
    rctx.draw.rect(BUTTON_X, BUTTON_Y, BUTTON_W, BUTTON_H, {
        fill: hover ? BUTTON_FILL_HOVER : BUTTON_FILL,
        stroke: BUTTON_STROKE,
    });
    // 状态文字只读模块数据（task / at 写它，render 读它）。
    rctx.draw.text(BUTTON_X, BUTTON_Y, child.phase + " hits=" + String(child.hits), {
        size: FONT_SIZE,
        fill: TEXT_FILL,
    });
}

const plugin = { render, start } satisfies PluginModule;

export default plugin;
