// v2 夹具 · `plugin/finish-quit` 的 **finish** 那一半（`quit` 那一半在 e2e 里）
//
// 判据（D-C2-9 / R3）：`ctx.finish(5, reason)` **只结束 Run**（readout `run=finished exit=5`），
// ⛔ **不改进程 exit**。
const plugin = {
    start(ctx) {
        ctx.finish(5, "old-exit5");
    },
};

export default plugin;
