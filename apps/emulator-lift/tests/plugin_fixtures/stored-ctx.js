// v2 夹具 · `plugin/render-stored-runctx-rejected`：把 **RunCtx 存下来**在 render 里用
// `fail`/`finish`/`quit`/`at`/`delay` ⇒ 必须 **TypeError**。
//
// 判据（红了意味着什么）：render 期 `call_host` 非空（config/draw 要用），而那些出口只判
//   "call_host 非空" ⇒ 插件在 `start` 里 `saved = ctx`，下一帧 `saved.fail(…)` 就能绕过
//   RenderCtx 的成员表。本夹具把**被接受**的 op 收集起来自己抛 ⇒ 宿主侧 render 返回 false ⇒ 红。
// ★ `ctx.off` 故意不在那张表里（只减操作）⇒ render 里调它必须被接受（返回 false，幂等）。
const OPS = ["fail", "finish", "quit", "at", "delay"];

let saved = null;
let sub = null;

const plugin = {
    render(ctx) {
        if (saved === null || sub === null) throw new Error("render before start: RunCtx not stored");
        const accepted = [];
        for (const op of OPS) {
            try {
                if (op === "fail") saved.fail("x", "from-render");
                else if (op === "finish") saved.finish(0, "from-render");
                else if (op === "quit") saved.quit("from-render");
                else if (op === "at") saved.at({ script: "FIELD.BIN", pos: 0x100 }, () => {});
                else if (op === "delay") saved.delay(1);
                else throw new Error("unknown op: " + op);
            } catch (e) {
                if (!e || e.name !== "TypeError") {
                    throw new Error("wrong error kind for " + op + ": " + String(e));
                }
                continue;
            }
            accepted.push(op);
        }
        if (accepted.length !== 0) throw new Error("accepted:" + accepted.join(","));
        // ★ 正向断言：`off` 在 render 里可用（只减、不写 guest、不让 Run 更长）。
        let offResult;
        try {
            offResult = saved.off(999999);
        } catch (e) {
            throw new Error("off rejected in render: " + String(e));
        }
        if (offResult !== false) throw new Error("off(999999) returned " + String(offResult));
        // RenderCtx 的四成员必须都在（v2 最终形态；⛔ read/log/profile/params）。
        for (const key of ["draw", "ui", "config", "game"]) {
            if (typeof ctx[key] === "undefined") throw new Error("RenderCtx missing " + key);
        }
    },
    start(ctx) {
        saved = ctx;  // ★ 存下来 —— 这就是旁路的入口
        sub = ctx.at({ script: "FIELD.BIN", pos: 0x100 }, () => {});
        if (typeof sub !== "number") throw new Error("start: ctx.at failed");
    },
};

export default plugin;
