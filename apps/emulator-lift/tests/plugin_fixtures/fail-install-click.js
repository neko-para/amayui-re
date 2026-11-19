// C2a 夹具 · `plugin/fail-install-releases-input`（F3-R1）
//
// 判据：**runner 侧**的失败（`install` 拒了 `onTrigger` 返回的集合）也是**终态** ⇒ 同一个回调里
// 已经 staged 的 input 必须被丢掉，⛔ 不许在下一读点落地。
//
// ★ 为什么这一条和 `fail-release.js` 不同：这条**不调** `ctx.fail` —— 失败发生在 JS 返回**之后**
//   （`TriggerRunner::invokeOnTrigger` 的 `install`）。⇒ 归因只能由 `PluginHost::syncRunnerFailure`
//   兜（`onTrigger` 的归因位当时还看不到）；旧实现会把这次 click 留到下一拍落地。
let once;

const plugin = {
    start(ctx) {
        once = ctx.trigger.atReadPoint("any", 1, { name: "a" });
        return once;
    },
    onTrigger(ctx) {
        ctx.input.click(700, 500);
        return once; // 已耗尽的 Once 对象 ⇒ `install` 判 exhausted ⇒ Run fail
    },
};

export default plugin;
