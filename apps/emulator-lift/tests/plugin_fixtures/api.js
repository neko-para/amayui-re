// C2a 夹具 · `plugin/api-surface`
//
// 判据（红了意味着什么）：`RunCtx` 的形状与 d.ts 不一致 —— 缺一个成员（插件写不出来），
// 或者**多**一个本切片不该有的成员（`draw` / `run` / `dispatch` / `sample` / `screenshot` /
// `read.frameHash`：声明了却不存在的东西会让"声明与实现"长期说谎）。
//
// ★ 这些断言写在**插件自己**里：宿主侧读不到 JS 对象，而 `ctx.fail` 会把名字带进 readout。
// ★ D-C2-6：`ctx.read.frameHash` **不安装**（⛔ 不返回 0）⇒ 这里断言它 `undefined`。
const REQUIRED = [
    "edit",
    "input",
    "read",
    "trigger",
    "ui",
    "profile",
    "state",
    "params",
    "log",
    "fail",
    "finish",
    "quit",
    "now",
];

const FORBIDDEN = ["draw", "run", "dispatch", "sample", "screenshot"];

const plugin = {
    start(ctx) {
        const bad = [];
        for (const key of REQUIRED) {
            if (!(key in ctx)) bad.push(`missing:${key}`);
        }
        for (const key of FORBIDDEN) {
            if (typeof ctx[key] !== "undefined") bad.push(`present:${key}`);
        }
        if (typeof ctx.read.frameHash !== "undefined") bad.push("present:read.frameHash");

        // ---- EngineRead 的形状（C2a 只有 global.get）----
        if (typeof ctx.read.global.get !== "function") bad.push("missing:read.global.get");

        // ---- 回执形状：Edit（立即）与 Input（只入队）----
        const edit = ctx.edit.global.set("int", 0xb1b6, 0);
        if (!("accepted" in edit) || !("queued" in edit) || !("applied" in edit)) {
            bad.push("edit.receipt.shape");
        }
        if (edit.accepted !== true || edit.applied !== true || edit.queued !== false) {
            bad.push(`edit.receipt.values:accepted=${edit.accepted},applied=${edit.applied},queued=${edit.queued}`);
        }
        const click = ctx.input.click(10, 20);
        if (!("id" in click) || typeof click.id !== "number" || click.id <= 0) {
            bad.push("input.receipt.id");
        }
        const status = ctx.input.status(click.id);
        if (status !== "queued") bad.push(`input.status:${status}`);

        // ---- ctx.now 的形状（D-C2-7：本回调内有效）----
        if (typeof ctx.now !== "object" || ctx.now === null) {
            bad.push("now:not-object");
        } else {
            if (typeof ctx.now.readIndex !== "number") bad.push("now.readIndex");
            if (typeof ctx.now.posValid !== "boolean") bad.push("now.posValid");
        }

        // ---- ctx.params（只读；宿主给了什么就有什么）----
        if (ctx.params.probe !== "api-surface") bad.push(`params.probe:${String(ctx.params.probe)}`);

        if (bad.length !== 0) {
            ctx.fail("api-surface", bad.join(","));
        }
        return [];
    },
    // ⛔ 成对：start 存在就必须有 onTrigger（合同 §4.6）；本夹具的用例只用 start。
    onTrigger() {
        return [];
    },
};

export default plugin;
