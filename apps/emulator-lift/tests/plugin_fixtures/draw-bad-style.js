// C2b 夹具 · `plugin/ui-draw` 的**反证**（D-C2-3）：不支持的 style 字段 ⇒ TypeError。
//
// 判据（红了意味着什么）：`stroke` / `width` / `alpha` / `align` 又被**静默忽略**了 ——
//   d.ts 声明了它们，而 `Element`（`ui/element.h`：只有 fill/color/bold/z）没有表示。
//   静默忽略会让"声明与实现"长期说谎，而且**没有症状**（少一圈描边没人知道是谁吞的）。
//
// 参数 `bad` 由 C++ 侧替换源码里的占位符（RenderCtx 没有 `params`）；`render` 必须**抛**。
let badField = "__BAD__";

const plugin = {
    render(ctx) {
        switch (badField) {
            case "rect.stroke":
                ctx.draw.rect(0, 0, 10, 10, { stroke: "#FFFFFF" });
                break;
            case "rect.width":
                ctx.draw.rect(0, 0, 10, 10, { width: 2 });
                break;
            case "rect.alpha":
                ctx.draw.rect(0, 0, 10, 10, { alpha: 0.5 });
                break;
            case "text.align":
                ctx.draw.text(0, 0, "x", { align: "center" });
                break;
            case "text.stroke":
                ctx.draw.text(0, 0, "x", { stroke: "#FFFFFF" });
                break;
            case "text.width":
                ctx.draw.text(0, 0, "x", { width: 1 });
                break;
            case "text.alpha":
                ctx.draw.text(0, 0, "x", { alpha: 0.5 });
                break;
            case "text.key":
                // ★ S2：`key` 只对 rect 有意义（命中区）；text 上 ⇒ TypeError。
                ctx.draw.text(0, 0, "x", { key: "k" });
                break;
            case "measure.align":
                ctx.draw.measure("x", { align: "left" });
                break;
            default:
                // 参数没给 / 拼错 ⇒ 也抛（⛔ 不许"什么都没画"静默通过）。
                throw new Error("unknown bad: " + badField);
        }
    },
};

export default plugin;
