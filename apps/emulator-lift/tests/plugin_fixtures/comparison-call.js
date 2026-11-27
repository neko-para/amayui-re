// S1b 夹具 · `plugin/js-comparison-unchanged`：`.js` **不许**带 `JS_EVAL_FLAG_TYPESCRIPT`。
//
// 判据（红了意味着什么）：`.js` 被误加了 TS flag ⇒ `f < a > (b)` 被读成泛型调用 `f<a>(b)`
//   ⇒ `f(5)` = 15；比较语义下 `(f < a) > b` = false。两者在 `r` 上可判。
// ★ 这是上游自认的"flags-on 唯一会改变合法 JS 含义"的写法；本仓的 `.js` 必须仍是比较
//   （本切片只按扩展名加 flag ⇒ `.js` 永远 flags=0）。
const f = (x) => x + 10;
const a = 20;
const b = 5;
const r = f < a > (b);

export default {
    start(ctx) {
        if (r !== false) {
            ctx.fail("js-comparison", `f<a>(b) 不是比较语义：r=${String(r)}`);
        }
        return [];
    },
    onTrigger() {
        return [];
    },
};
