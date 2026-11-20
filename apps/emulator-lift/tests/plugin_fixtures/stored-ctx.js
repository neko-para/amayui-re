// C2b 夹具 · `plugin/render-stored-runctx-rejected`（复审 F2）：把 **RunCtx 存下来**在 render 里用
// `fail`/`finish`/`quit`/`profile.*` ⇒ 必须 **TypeError**。
//
// 判据（红了意味着什么）：render 期 `call_host` 非空（`ctx.read`/`ctx.log` 要用），而那 5 个出口
//   只判"call_host 非空" ⇒ 插件在 `onTrigger` 里 `saved = ctx`，下一帧 `saved.fail(...)` 就能绕过
//   `RenderCtx` 的成员表（`fail/finish/quit/profile` 都不在它上面）。
//   本夹具把**被接受**的 op 收集起来自己抛（`accepted:<op,…>`）⇒ 宿主侧 render 返回 false ⇒ 红。
//   ★ 主机的 profile 桥是**接了的**（否则 `profile.*` 会因为"没接线"抛 TypeError 而假通过）；
//     C++ 侧同时断言 host_ops 的 begin/end 计数仍为 0。
const OPS = ["fail", "finish", "quit", "profile.begin", "profile.end"];

let saved = null;

const plugin = {
    render(ctx) {
        // ★ 必须真的在 start 之后：`saved === null` 时 `saved.fail` 也会抛 TypeError（假通过）。
        if (saved === null) throw new Error("render before start: RunCtx not stored");
        const accepted = [];
        for (const op of OPS) {
            try {
                if (op === "fail") saved.fail("x", "from-render");
                else if (op === "finish") saved.finish(0, "from-render");
                else if (op === "quit") saved.quit("from-render");
                else if (op === "profile.begin") saved.profile.begin("x");
                else if (op === "profile.end") saved.profile.end();
                else throw new Error("unknown op: " + op);
            } catch (e) {
                // 唯一正确的形状：非回调期 ⇒ TypeError（⛔ 不是"接受"、也不是别的错）。
                if (!e || e.name !== "TypeError") {
                    throw new Error("wrong error kind for " + op + ": " + String(e));
                }
                continue;
            }
            accepted.push(op);
        }
        if (accepted.length !== 0) throw new Error("accepted:" + accepted.join(","));
        // ★ 同一次 render 里，读数面必须仍然可用（F2 的修法不能把 render 的 read/log 一起关掉）。
        if (typeof ctx.read.global.get !== "function") throw new Error("read lost");
        if (typeof ctx.log !== "function") throw new Error("log lost");
    },
    start(ctx) {
        saved = ctx;  // ★ 存下来 —— 这就是旁路的入口
        return [ctx.trigger.afterScript("TITLE", { name: "hold" })];
    },
    onTrigger() {
        return [];
    },
};

export default plugin;
