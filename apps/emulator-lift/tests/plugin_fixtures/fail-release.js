// C2a 夹具 · `plugin/fail-releases-input`（F3 裁决的 fail 半）
//
// 判据：`ctx.fail(...)` 是**终态异常** ⇒ 同一个回调里**已经提交**的 input（这里是一次 click）必须被
// 立刻丢掉（`releaseOwner` 在 `atPoint` 的第 ⑥ 步之前），⛔ 不许在下一次读点落地。
// ★ 反证：修 F3 之前，`stagedDepth()==1`，下一读点光标会走到 (200,100)。
const plugin = {
    start(ctx) {
        return ctx.trigger.atReadPoint("any", 1, { name: "boom" });
    },
    onTrigger(ctx) {
        ctx.input.click(200, 100);
        ctx.fail("boom", "stop-here");
        return [];
    },
};

export default plugin;
