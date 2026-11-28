// v2 夹具 · `plugin/ui-state`：`RenderCtx.ui` 的状态变化只在 render 里做；下一次 render 才画。
//
// ★ 判据：RunCtx 没有 draw/ui（typeof undefined）；render 按模块状态调 `ctx.ui.show/hide` 并画 2 个元素。
// ★ 为什么用 render 计数而不是 `ctx.delay`：render 边界本身会推进 async 泵，用 delay 分步会让
//   "下一次 render"提前看到下一步。这里判的是 render 的**成员面**，不是 async 时序。
let started = false;
let renders = 0;

const plugin = {
    render(ctx) {
        if (!started) return;
        renders += 1;
        if (renders === 1) ctx.ui.show("hud", { mode: "title" });
        if (renders >= 2) ctx.ui.hide("hud");
        const hud = ctx.ui.get("hud");
        if (!ctx.ui.visible("hud")) return;
        ctx.draw.rect(0, 0, 120, 24, { fill: "#101820CC" });
        ctx.draw.text(4, 4, "mode " + hud.mode, { size: 16 });
    },
    start(ctx) {
        if (typeof ctx.draw !== "undefined") ctx.fail("hud-ui", "RunCtx has draw");
        if (typeof ctx.ui !== "undefined") ctx.fail("hud-ui", "RunCtx has ui");
        started = true;
    },
};

export default plugin;
