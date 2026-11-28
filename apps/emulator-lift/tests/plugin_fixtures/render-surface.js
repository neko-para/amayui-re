// v2 夹具 · `plugin/render-surface`：RenderCtx 的成员表 = `draw/ui/config/game`。
//
// 判据（红了意味着什么）：形状与 d.ts 漂了 —— 多一个（把 RunCtx 的面带进 render）或少一个。
const FORBIDDEN = [
    "edit",
    "input",
    "read",
    "log",
    "profile",
    "params",
    "trigger",
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
        if (keys !== "config,draw,game,ui") bad.push("keys:" + keys);
        for (const key of FORBIDDEN) {
            if (typeof ctx[key] !== "undefined") bad.push("present:" + key);
        }
        // ---- draw ----
        for (const key of ["rect", "text", "measure"]) {
            if (typeof ctx.draw[key] !== "function") bad.push("missing:draw." + key);
        }
        // ---- game（方法 + 只读 getter；无源 ⇒ undefined，⛔ 不编 0 / 空对象）----
        if (typeof ctx.game.mouse !== "function") bad.push("missing:game.mouse");
        if (typeof ctx.game.localFrames !== "function") bad.push("missing:game.localFrames");
        for (const key of ["fps", "script", "worst", "click", "hover", "active", "overlayKeySeq"]) {
            if (typeof ctx.game[key] !== "undefined") bad.push("game." + key + ":" + String(ctx.game[key]));
        }
        // ---- config（`{available, domain(id)}`）----
        if (typeof ctx.config.available !== "boolean") {
            bad.push("config.available:" + typeof ctx.config.available);
        }
        if (typeof ctx.config.domain !== "function") bad.push("missing:config.domain");
        if (bad.length !== 0) throw new Error("render-surface: " + bad.join(","));
        return undefined;
    },
};

export default plugin;
