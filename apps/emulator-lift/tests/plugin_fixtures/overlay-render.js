// C2b-2 夹具 · `aot-overlay` 的 `plugin-render-window` 子用例：**窗口档**的插件 render。
//
// 判据（红了意味着什么）：
//   ① 插件真的被装进**已有**的叠层 host（`adoptOverlayScript`）⇒ 每帧 `render` 被调；
//   ② 它的 display list 真的上屏：整屏 rect 的 fill 是**不透明** `#3366CC`（alpha=FF）⇒
//      窗口 BMP 的画布原点 (0,0) 逐位等于该值（⛔ 不套混合公式、⛔ 不读 hud.js 的底色）；
//   ③ `ctx.draw.measure` 有宿主字体（没有 ⇒ TypeError ⇒ render false ⇒ 像素仍是 guest 黑）。
//
// ★ 整屏 rect 用 **1280×720 参考系**（叠层坐标的唯一口径；宿主按窗口缩放）⇒ 与 guest 后缓冲
//   分辨率无关：任何后缓冲尺寸下画布都被填满。
const FILL = "#3366CC";

const plugin = {
    render(ctx) {
        const m = ctx.draw.measure("W", { size: 16 });
        if (!(m.width > 0) || !(m.height > 0)) {
            throw new Error("measure: " + m.width + "x" + m.height);
        }
        ctx.draw.rect(0, 0, 1280, 720, { fill: FILL });
    },
};

export default plugin;
