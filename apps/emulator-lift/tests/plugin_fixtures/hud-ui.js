// C2b 夹具 · `plugin/ui-state`：`onTrigger` **只改** `ctx.ui`；下一次 `render` 才按状态画。
//
// 判据（红了意味着什么）：
//   ① **RunCtx 没有 draw**：回调里 `typeof ctx.draw` 不是 "undefined" ⇒ fail（⛔ 不是"画不出来"，
//      而是这一层就不该存在）—— C2b 只把 draw 装在 RenderCtx 上；
//   ② 回调之后宿主手里的 display list 仍是空的（UI 状态 ≠ display list；onTrigger 不画）；
//   ③ 下一次 render 读到 `ctx.ui.visible/get` 并按状态画；hide 之后下一次 render 又为空。
const plugin = {
    render(ctx) {
        const hud = ctx.ui.get("hud");
        if (!ctx.ui.visible("hud")) return;
        ctx.draw.rect(0, 0, 120, 24, { fill: "#101820CC" });
        ctx.draw.text(4, 4, "mode " + hud.mode, { size: 16 });
    },
    start(ctx) {
        return [
            ctx.trigger.afterScript("TITLE", { name: "hud:show" }),
            ctx.trigger.afterScript("FIELD", { name: "hud:hide" }),
        ];
    },
    onTrigger(ctx, ev) {
        if (typeof ctx.draw !== "undefined") {
            ctx.fail(ev.name, "RunCtx has draw");
            return [];
        }
        if (ev.name === "hud:show") {
            ctx.ui.show("hud", { mode: "title" });
            // 返回新的 armed 集合（只留 hide 那道门；`[]` = done 由 hide 那一侧给）。
            return [ctx.trigger.afterScript("FIELD", { name: "hud:hide" })];
        }
        if (ev.name === "hud:hide") {
            ctx.ui.hide("hud");
            return [];
        }
        ctx.fail(ev.name, "unknown name: " + ev.name);
        return [];
    },
};

export default plugin;
