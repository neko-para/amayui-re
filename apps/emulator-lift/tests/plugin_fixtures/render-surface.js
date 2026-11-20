// C2b 夹具 · `plugin/render-surface`：`RenderCtx` 的成员表 = d.ts 的 `RenderCtx` **减 `run`**（C5）。
//
// 判据（红了意味着什么）：形状与 d.ts 漂了 ——
//   · **多**一个成员（把 RunCtx 的写面带进 render：edit/input，或 Run 语境的 state/now/…）；
//   · **少**一个成员（插件写不出来：ui/draw/read/game/params/log）；
//   · `read.frameHash` 又"有值"了（D-C2-6：C2 **不安装** ⇒ 必须是 undefined）。
// ★ `do-nothing` 地成功也算过 —— 但这个夹具**只在形状对时**不抛；宿主侧判据是 render 返回 true。
const FORBIDDEN = [
    "edit",
    "input",
    "trigger",
    "profile",
    "state",
    "now",
    "fail",
    "finish",
    "quit",
    "run",
    "dispatch",
    "sample",
    "screenshot",
];

const plugin = {
    render(ctx) {
        const bad = [];
        // ★ 精确成员表：多一个 / 少一个都红（`Object.keys` 是这个判据的唯一来路）。
        const keys = Object.keys(ctx).sort().join(",");
        if (keys !== "draw,game,log,params,read,ui") bad.push("keys:" + keys);
        for (const key of FORBIDDEN) {
            if (typeof ctx[key] !== "undefined") bad.push("present:" + key);
        }
        // ---- draw ----
        for (const key of ["rect", "text", "measure"]) {
            if (typeof ctx.draw[key] !== "function") bad.push("missing:draw." + key);
        }
        // ---- read（global.get + C2b 的 frames/scriptPos；⛔ frameHash）----
        if (typeof ctx.read.global.get !== "function") bad.push("missing:read.global.get");
        if (typeof ctx.read.frames !== "function") bad.push("missing:read.frames");
        if (typeof ctx.read.scriptPos !== "function") bad.push("missing:read.scriptPos");
        if (typeof ctx.read.frameHash !== "undefined") bad.push("present:read.frameHash");
        // ---- game（方法，⛔ 不是 overlay 的 getter）----
        if (typeof ctx.game.mouse !== "function") bad.push("missing:game.mouse");
        if (typeof ctx.game.localFrames !== "function") bad.push("missing:game.localFrames");
        // ---- ui / params / log ----
        for (const key of ["show", "hide", "set", "visible", "get"]) {
            if (typeof ctx.ui[key] !== "function") bad.push("missing:ui." + key);
        }
        if (typeof ctx.log !== "function") bad.push("missing:log");
        if (ctx.params.probe !== "render-surface") bad.push("params.probe:" + String(ctx.params.probe));
        if (bad.length !== 0) throw new Error(bad.join(","));
    },
};

export default plugin;
