// S2 复审 minor 4 夹具 · `ctx.draw.measure` 的 `bold` **必须真的参与度量**（⛔ 静默按非粗体）。
//
// 判据（C++ 侧）：两条 Rect 的 w/h 分别等于宿主 `measureText(..., bold=true/false)` 的读数
// （⛔ 不是把 JS 算出来的数抄进断言）；实现若吞掉 `bold`，第一条会与第二条相同 ⇒ 交叉核对红
// （前提：本机字体的粗体度量与常规不同 —— 缺粗体 face 时会在输出里点名）。
const plugin = {
    render(ctx) {
        const bold = ctx.draw.measure("hello", { size: 18, bold: true });
        ctx.draw.rect(0, 0, bold.width, bold.height, { fill: "#000000" });
        const normal = ctx.draw.measure("hello", { size: 18 });
        ctx.draw.rect(0, 0, normal.width, normal.height, { fill: "#000000" });
    },
};

export default plugin;
