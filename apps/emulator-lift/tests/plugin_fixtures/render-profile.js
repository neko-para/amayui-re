// S2 夹具 · `plugin/render-profile`：`RenderCtx.profile` 在 **render** 里可用，且与 RunCtx 同形。
//
// 判据（红了意味着什么）：
//   ① `ctx.profile.begin(label)` / `end()` 在 render 里**能调到宿主**（调用计数由 C++ 侧判）；
//   ② 两者都返回 `undefined`（与 RunCtx 的 `ctx.profile` 同形 —— ⛔ 不是"结果对象"）；
//   ③ 宿主失败 ⇒ **TypeError**（`ctx.profile.begin: <why>`；⛔ 静默 no-op）。
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
            if (end !== undefined) {
                draw("end-not-undefined:" + String(end));
                return;
            }
            draw("ok");
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
            try {
                ctx.profile.end();
            } catch (err) {
                draw("end-threw: " + err.message);
                return;
            }
            draw("end-no-throw");
            return;
        }
        throw new Error("unknown mode: " + mode);
    },
};

export default plugin;
