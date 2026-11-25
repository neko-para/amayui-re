// C2b 夹具 · `plugin/ui-draw`：`ctx.draw.{rect,text,measure}` 把本帧 display list 写成宿主可读的元素。
//
// 判据（红了意味着什么）：draw 的**形状 / 映射**漂了 ——
//   ① 元素字段（x/y/w/h/fill/字号/颜色）；S2 加 `style.key`（命中区）与 `style.bold`；
//   ② z 序 = 调用顺序（宿主只能看到构造序：z 恒 0、同 z 按 seq）；
//   ③ `measure` 返回 **`{width,height}`**（⛔ 不是 overlay 的 `{w,h}`），且尺寸与宿主自己的度量一致；
//   ④ `RenderCtx.ui` 可写（同一帧后续读得到 —— 同一个数据面）。
const plugin = {
    render(ctx) {
        // ① 调用顺序 = 绘制序：先 rect、再 text、最后用 measure 的量再画一个 rect。
        //   ★ S2：`style.key`（命中区）/ `style.bold`（粗体）是 hud 迁移需要的两个字段。
        ctx.draw.rect(10, 20, 100, 40, { fill: "#102030CC", key: "hit10" });
        ctx.draw.text(12, 24, "hello", { size: 20, fill: "#EBFFEB", bold: true });

        // ③ measure 的键集必须**恰好**是 width/height（⛔ 不是 w/h）。
        const m = ctx.draw.measure("hello", { size: 18 });
        const keys = Object.keys(m).sort().join(",");
        if (keys !== "height,width") throw new Error("measure keys: " + keys);
        if (!(m.width > 0) || !(m.height > 0)) {
            throw new Error("measure size: " + m.width + "x" + m.height);
        }
        // 把量出来的尺寸画成第三个元素 ⇒ C++ 侧能拿**自己的** measureText 读数比对（不是自证）。
        ctx.draw.rect(0, 0, m.width, m.height, { fill: "#000000" });

        // ④ ctx.ui 可写：show 之后**本帧**就能 visible / get（同一个数据面；不是"下一次才可见"）。
        ctx.ui.show("hover", { key: "hud" });
        if (!ctx.ui.visible("hover")) throw new Error("ui.visible:false-after-show");
        const hover = ctx.ui.get("hover");
        if (!hover || hover.key !== "hud") throw new Error("ui.get:lost-data");
    },
};

export default plugin;
