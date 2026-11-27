// 「保留其余」：多条 `ctx.at` 订阅 + 选择性 `ctx.off`（v2）。
//
// 演示什么
//   · v1 = 一次 arm 3 个具名 Trigger；某个触发后要**返回完整的新 armed 集合**来「保留其余」，
//     刚触发的那个不会自动保留（漏掉 = 被消费，重复返回耗尽的 = fail）。
//   · v2 没有 armed 集合：**每条 `ctx.at` 就是一条独立订阅**，默认一直有效。
//     「保留其余」= 什么都不做；「只停一条」= `ctx.off(那一条的 id)`。没有集合替换这回事。
//
// v2 新写法
//   · `ctx.at` 返回订阅 id（`number | undefined`；`no_injection` ⇒ undefined + 宿主一行 error）。
//   · `ctx.off(id)` 幂等（撤过 / Run 已收口 ⇒ false），在 tick handler 里也能调（只减）。
//   · 同一 `(script,pos)` 可以有多条订阅；handler 按注册序串成链，`next()` 继续、不调 = 截断。
//
// 占位值
//   · script / 区间 / global id / 掩码 / HUD 坐标与颜色全是占位；真实 `lo < hi`、`script` 含 .BIN。

import type { PluginModule, RenderCtx, RunCtx } from "@amayui/lift-plugin";

const SHOW_SCRIPT = "TITLE.BIN"; // 占位
const SHOW_LO = 0x00000000; // 占位
const SHOW_HI = 0x00000000;
const HIDE_SCRIPT = "FIELD.BIN"; // 占位
const HIDE_LO = 0x00000000;
const HIDE_HI = 0x00000000;
const FLAG_SCRIPT = "SC0000.BIN"; // 占位
const FLAG_LO = 0x00000000;
const FLAG_HI = 0x00000000;

const GLOBAL_ID = 0; // 占位
const SET_BITS = 0; // 占位

const HUD_X = 0; // 占位
const HUD_Y = 0;
const HUD_W = 0;
const HUD_H = 0;
const HUD_FILL = ""; // 占位：颜色串

/** tick handler 只改模块数据；render 读。 */
const hud = { visible: false };

let showId: number | undefined;
let hideId: number | undefined;
let flagId: number | undefined;

function start(ctx: RunCtx): void {
    showId = ctx.at({ script: SHOW_SCRIPT, lo: SHOW_LO, hi: SHOW_HI }, (tick, next) => {
        hud.visible = true;
        tick.log("debug", "keep-others: show");
        next();
    });

    hideId = ctx.at({ script: HIDE_SCRIPT, lo: HIDE_LO, hi: HIDE_HI }, (tick, next) => {
        hud.visible = false;
        tick.log("debug", "keep-others: hide");
        next();
    });

    flagId = ctx.at({ script: FLAG_SCRIPT, lo: FLAG_LO, hi: FLAG_HI }, (tick, next) => {
        const before = ctx.read.global.get("int", GLOBAL_ID);
        tick.edit.global.set("int", GLOBAL_ID, before | SET_BITS);

        // 只停这一条（v1 的「一次性」）：show / hide 两条订阅不受影响。
        if (flagId !== undefined) ctx.off(flagId);
        tick.log("info", "keep-others: flag written; this subscription removed");
        next();
    });

    if (showId === undefined || hideId === undefined || flagId === undefined) {
        ctx.fail("keep-others", "ctx.at 未装（no_injection）");
    }
}

function render(rctx: RenderCtx): void {
    if (!hud.visible) return;
    rctx.draw.rect(HUD_X, HUD_Y, HUD_W, HUD_H, { fill: HUD_FILL });
}

const plugin = { render, start } satisfies PluginModule;

export default plugin;
