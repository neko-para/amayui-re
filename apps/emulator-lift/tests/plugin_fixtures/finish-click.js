// v2 夹具 · `plugin/finish-lands-input`（F3 裁决的 finish 半 = 对照）
//
// 判据：`ctx.finish(code)` 是**正常收口** ⇒ **不** `releaseOwner`：同一次回调里提交的
// `ctx.input.click` 仍要在**下一个读点**落地。
const plugin = {
    start(ctx) {
        ctx.input.click(640, 323);
        ctx.finish(0, "old-exit0");
    },
};

export default plugin;
