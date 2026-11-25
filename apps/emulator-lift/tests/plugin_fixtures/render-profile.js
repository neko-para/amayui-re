// S2 夹具 · `plugin/render-profile`：`RenderCtx.profile` 在 **render** 里可用。
//
// 判据（红了意味着什么）：
//   ① `ctx.profile.begin(label)` 在 render 里**能调到宿主**（调用计数由 C++ 侧判）；
//      `begin` 与 RunCtx 同形：**返回 `undefined`**，失败 = TypeError；
//   ② `ctx.profile.end()` 返回与旧 UiScript `host.profileEnd()` **同形**的 `{ok,text,error}`
//      （S2 复审 I3：HUD 要判报告文本里的 `INVALID`）—— ⛔ 不是 undefined、⛔ 失败不抛；
//   ③ 宿主失败 ⇒ `end()` 回 `{ok:false, error:<why>, text:""}`（结果对象，不是异常）。
// ★ mode 由 `--plugin-param mode=` 给（ok / fail-begin / fail-end）：C++ 侧按画出来的文本判。
const plugin = {
    render(ctx) {
        const mode = ctx.params.mode === undefined ? "ok" : ctx.params.mode;
        const draw = (text) => ctx.draw.text(0, 0, text, { size: 16 });
        if (mode === "ok") {
            const begin = ctx.profile.begin("overlay");
            if (begin !== undefined) {
                draw("begin-not-undefined:" + String(begin));
                return;
            }
            const end = ctx.profile.end();
            const shape = end === null || end === undefined
                ? "missing"
                : typeof end.ok + "/" + typeof end.text + "/" + typeof end.error;
            if (shape !== "boolean/string/string") {
                draw("end-shape:" + shape);
                return;
            }
            if (end.ok !== true || end.error !== "") {
                draw("end-not-ok:" + end.error);
                return;
            }
            draw("ok:" + end.text);
            return;
        }
        if (mode === "fail-begin") {
            try {
                ctx.profile.begin("overlay");
            } catch (err) {
                draw("begin-threw: " + err.message);
                return;
            }
            draw("begin-no-throw");
            return;
        }
        if (mode === "fail-end") {
            // ★ 失败**不抛**（结果对象）；只有"不在 render 里 / 没有宿主桥"才是 TypeError。
            const end = ctx.profile.end();
            if (end === null || end === undefined) {
                draw("end-missing");
                return;
            }
            draw("end-result:" + String(end.ok) + ":" + end.error);
            return;
        }
        throw new Error("unknown mode: " + mode);
    },
};

export default plugin;
