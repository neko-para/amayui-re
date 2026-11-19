// C2a 夹具 · `plugin/finish-quit` 的 **finish** 那一半（`quit` 那一半在 e2e 里）
//
// 判据（D-C2-9 / R3）：`ctx.finish(5, reason)` **只结束 Run**（readout `run=finished exit=5`），
// ⛔ **不改进程 exit**（宿主侧用例断言 readout 的 `exit=5`，进程退出码仍按 run 自己的结局走）。
const plugin = {
    start(ctx) {
        ctx.finish(5, "old-exit5");
        // finish 之后返回什么都不会被 arm（Run 已收口）；这里显式 `[]` 让意图无从误解。
        return [];
    },
    onTrigger(ctx) {
        return [];
    },
};

export default plugin;
