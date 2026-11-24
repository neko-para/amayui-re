// C2a 夹具 · **宿主调用暂停**：`ctx.profile.end` 是宿主能力，它自己的 CPU 不计入用户 JS 的 2 ms。
//
// 判据（红了意味着什么）：`plugin/callback-host-pause`（`tests/plugin_runtime.cpp`）给
// `ctx.profile.end` 接一个**故意烧 CPU 的假宿主**（5 ms 忙等）⇒
//   · 回调**不许**因为"用户 JS 超出 2 ms"失败（`js_cpu_us` 必须是**扣除后**的、远小于 2000）；
//   · 宿主确实被调到了（否则"没超"可能只是根本没调宿主 —— 用例另有断言）。
// 去掉暂停（`script_quickjs.cpp` 的 `HostPause`）⇒ 那 5 ms 落进 `js_cpu_ns` ⇒ `over=js` ⇒ 本夹具的用例红。
//
// ⚠ 本文件里**没有**真忙等：CPU 全在宿主侧（用例注入的假 `profile_end`），这样"超没超"与 JS 快慢无关。
const plugin = {
    start(ctx) {
        // 唯一一件事：调宿主能力（窗口报告 + 剪贴板的生产实现在 `AotHost::endWindowText`）。
        ctx.profile.end();
        return [];
    },
    onTrigger(ctx) {
        void ctx;
        return [];
    },
};

export default plugin;
