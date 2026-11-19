// C2a 夹具 · `ctx.quit()`（第二条停机源；readout 归因 `plugin: quit`）
//
// 判据（D-C2-9）：`ctx.quit(reason)` ⇒ 宿主 `stop_requested` → 下一个块边界以 `Stopped` 收口，
// `AotRunOptions::stop_detail` 点名 `plugin: quit`；Run 同时收口（返回 `[]`）。
const plugin = {
    start(ctx) {
        ctx.quit("scripted-quit");
        return [];
    },
    onTrigger(ctx) {
        return [];
    },
};

export default plugin;
