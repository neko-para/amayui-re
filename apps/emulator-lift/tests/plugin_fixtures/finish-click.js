// C2a 夹具 · `plugin/finish-lands-input`（F3 裁决的 finish 半 = 对照）
//
// 判据：`ctx.finish(code)` 是**正常收口** ⇒ **不** `releaseOwner`：同一次回调里提交的
// `ctx.input.click` 仍要在**下一个读点**落地（与 `scripts/plugins/lib/save.js` 终态的 `click + return []` 同义）。
// ★ 为什么是"下一个读点"而不是"本读点第 ⑥ 步"：回调里的 `ctx.input.*` 一律进 `staged_`，
//   由**下一读点**的 `beginReadPoint`（第 ② 步）提升 ⇒ 本读点的 ⑥ 只看得见**上一拍**提交的。
const plugin = {
    start(ctx) {
        return ctx.trigger.atReadPoint("any", 1, { name: "fin" });
    },
    onTrigger(ctx) {
        ctx.input.click(640, 323);
        ctx.finish(0, "old-exit0");
        return [];
    },
};

export default plugin;
