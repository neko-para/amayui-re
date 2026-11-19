// C2a 夹具 · `plugin/initial-state`
//
// 判据：`initialState` 在 `start` **之前**注入（`ctx.state.n === 1`）；`ctx.state` 是 **per-Run**
// 的容器（同一次 Run 的第二个回调看得到上一个回调写进去的值）。
// ★ 两个 Run 不共享由宿主侧用例表达（两次 load、各自的 Host），⛔ 不在 JS 里造第二个 Run。
const plugin = {
    initialState: { n: 1 },
    start(ctx) {
        if (ctx.state.n !== 1) {
            ctx.fail("state", `initialState 没有在 start 之前注入：n=${String(ctx.state.n)}`);
            return [];
        }
        ctx.state.n = 2;
        return ctx.trigger.atReadPoint("any", 1, { name: "again" });
    },
    onTrigger(ctx, ev) {
        if (ev.name !== "again") {
            ctx.fail(ev.name, "unexpected");
            return [];
        }
        if (ctx.state.n !== 2) {
            ctx.fail("state", `per-Run 状态丢了：n=${String(ctx.state.n)}`);
            return [];
        }
        return [];
    },
};

export default plugin;
