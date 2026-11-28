// v2 夹具 · `plugin/finish-lands-input`（F3 裁决的 finish 半 = 对照）
//
// 判据：`ctx.finish(code)` 是**正常收口** ⇒ **不** `releaseOwner`：同一个入口里提交的
// `tick.input.click` 仍要在**下一个读点**落地。
// ★ P2b：guest 写只走 Tick ⇒ 在 at handler 里提交 click 再 finish（RunCtx 没有 input）。
const PLACE = { script: "FIELD.BIN", pos: 0x100 };

const plugin = {
    start(ctx) {
        ctx.at(PLACE, (tick) => {
            tick.input.click(640, 323);
            ctx.finish(0, "old-exit0");
        });
    },
};

export default plugin;
