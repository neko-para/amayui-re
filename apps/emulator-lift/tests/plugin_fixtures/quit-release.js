// C2a 夹具 · `plugin/quit-releases-input`（F3 裁决的 quit 半）
//
// 判据：`ctx.quit(...)` 也是**终态异常** ⇒ 同一次回调里已提交的 input 必须被立刻丢掉
// （`releaseOwner` 在 `atPoint` 的第 ⑥ 步之前），⛔ 不许在宿主停机之前再落地一次点击。
const plugin = {
    start(ctx) {
        return ctx.trigger.atReadPoint("any", 1, { name: "bye" });
    },
    onTrigger(ctx) {
        ctx.input.click(300, 400);
        ctx.quit("scripted-quit");
        return [];
    },
};

export default plugin;
