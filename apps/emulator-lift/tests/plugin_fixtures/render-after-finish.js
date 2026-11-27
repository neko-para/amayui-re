// S2-2 夹具 · **Run 收口之后** present 仍会每帧调 render ⇒ 不许再泵 / 再 settle / 跑用户 JS
//
// 判据（`plugin/render-after-finish`，红了意味着什么）：
//   · `start` 里起一个 `ctx.task`，它 `await ctx.once(..., {timeoutMs})`，并 catch 后 log 一行；
//   · 用例从 C++ 侧调 `PluginHost::finish(5)`（= `ctx.finish` 的同一条宿主入口）⇒
//     `releaseRunHooks()` 会撤掉 once 的临时 hook（S2-4 起 ⇒ reject ⇒ 那条 catch 的 continuation
//     已经在 job 队列里）；
//   · 然后调 `PluginScript::render()`：**必须**直接早退（不泵、不 settle、不跑用户 JS）⇒
//     `logLines()` 不涨（泵若跑，catch 的 log 会涨；render 若跑，"render-ran" 也会涨）。
const SCRIPT = "FIELD.BIN";
const POS = 0x0100;

const plugin = {
    start(ctx) {
        ctx.task(async (tctx) => {
            try {
                await ctx.once({ script: SCRIPT, pos: POS }, { timeoutMs: 60000 });
                tctx.log("info", "render-after-finish: unexpected resolve");
            } catch (e) {
                tctx.log("info", "render-after-finish: task resumed (should-not-run)");
            }
        });
        ctx.log("info", "render-after-finish: armed");
        return null;
    },
    render(ctx) {
        // ★ 这一行是"render 没被早退挡住"的证据（`PluginHost::log` 对连续相同消息去重，但本用例只调一次）。
        ctx.log("info", "render-after-finish: render-ran");
        return undefined;
    },
};

export default plugin;
