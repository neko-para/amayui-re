// C2a 夹具 · `plugin/at-read-point-file-read`（F5-R2）
//
// 判据：`atReadPoint("fileRead", 1, …)` **能解析**（运行期词表里有 `InputPoint::FileRead`）、能 arm，
// 且只在**下一个 `ReadFile` 投递点**上 fire（别的读点不 fire）；回调里 `ev.point` / `ctx.now.point`
// 都必须是 `"fileRead"`（与 `pluginPointName()` 的输出侧同一个名字 —— 一边能给、一边抛 TypeError
// 就是"声明与实现漂移"的另一种症状）。
const plugin = {
    start(ctx) {
        return ctx.trigger.atReadPoint("fileRead", 1, { name: "fr" });
    },
    onTrigger(ctx, ev) {
        if (ev.name !== "fr") {
            ctx.fail(ev.name, `unknown name: ${ev.name}`);
            return [];
        }
        if (ev.point !== "fileRead") {
            ctx.fail(ev.name, `ev.point=${String(ev.point)}（期望 fileRead）`);
            return [];
        }
        if (ctx.now.point !== "fileRead") {
            ctx.fail(ev.name, `ctx.now.point=${String(ctx.now.point)}（期望 fileRead）`);
            return [];
        }
        return []; // done
    },
};

export default plugin;
