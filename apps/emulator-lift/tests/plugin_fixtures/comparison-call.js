// v2 夹具 · `plugin/js-comparison-unchanged`：`.js` **不许**带 `JS_EVAL_FLAG_TYPESCRIPT`。
const f = (x) => x + 10;
const a = 20;
const b = 5;
const r = f < a > (b);

export default {
    start(ctx) {
        if (r !== false) {
            ctx.fail("js-comparison", `f<a>(b) 不是比较语义：r=${String(r)}`);
        }
    },
};
