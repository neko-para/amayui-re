// C2b 夹具 · `plugin/render-readings`：`RenderCtx` 的读数面。
//
// 判据（红了意味着什么）：
//   ① `ctx.read.frames()` / `ctx.game.localFrames()` 的**字段映射**（`index` ↔ 宿主槽号、
//      `script`、只有 top 帧才有 `pos`），以及"没有源 ⇒ **undefined**"（⛔ 不编空数组）；
//   ② `ctx.read.scriptPos()` 从 **top 帧**取（没有 top 帧 ⇒ undefined；位置读不到 ⇒ `valid:false`
//      且**缺 `pos` 键** —— ⛔ 不编 0）；
//   ③ `ctx.game.mouse()` 是**方法**且三键电平**没有源**时不安装 `buttons`（⛔ 不编 false）；
//      有源时三位是**真实电平**（`probe: "mouse-buttons"`；C2b-2）。
//
// 参数 `probe` 点名一个场景；任何一处不符就 `throw`（宿主侧判据 = render 返回 true）。
const plugin = {
    render(ctx) {
        const probe = ctx.params.probe;

        if (probe === "mouse") {
            const m = ctx.game.mouse();
            if (!m) throw new Error("mouse:undefined");
            if (m.x !== 812 || m.y !== 622) throw new Error("mouse:" + m.x + "," + m.y);
            // ★ 三键电平没有源 ⇒ **不安装**这个键（d.ts 声明留着，C2b-2 / C4 再接）。
            if ("buttons" in m) throw new Error("mouse:buttons-present");
            return;
        }

        if (probe === "mouse-buttons") {
            // ★ C2b-2：宿主**有**读数源时安装 `buttons`，且三位是真实电平（不是编的 false）。
            const m = ctx.game.mouse();
            if (!m) throw new Error("mouse:undefined");
            if (!m.buttons) throw new Error("mouse:buttons-missing");
            if (m.buttons.left !== true || m.buttons.right !== false || m.buttons.middle !== false) {
                throw new Error(
                    "mouse:buttons=" + m.buttons.left + "," + m.buttons.right + "," + m.buttons.middle
                );
            }
            return;
        }

        if (probe === "frames") {
            const frames = ctx.read.frames();
            if (!Array.isArray(frames)) throw new Error("frames:not-array");
            if (frames.length !== 2) throw new Error("frames:len=" + frames.length);
            const top = frames[0];
            if (top.index !== 2 || top.script !== "SC0000") {
                throw new Error("frames[0]:" + top.index + "," + top.script);
            }
            if (top.pos !== 0x1104) throw new Error("frames[0].pos:" + top.pos);
            // `hash` / `presented` 在 runtime 里没有源 ⇒ 不安装（⛔ 不编 0 / false）。
            if ("hash" in top || "presented" in top) throw new Error("frames[0]:extra-keys");
            const below = frames[1];
            if (below.index !== 1 || below.script !== "SC0009") {
                throw new Error("frames[1]:" + below.index + "," + below.script);
            }
            // 非 top 帧没有执行点 ⇒ 没有 `pos` 键。
            if ("pos" in below) throw new Error("frames[1].pos-present");
            const local = ctx.game.localFrames();
            if (!Array.isArray(local) || local.length !== 2) throw new Error("localFrames");
            if (local[0].index !== 2 || local[0].script !== "SC0000") {
                throw new Error("localFrames[0]:" + local[0].index + "," + local[0].script);
            }
            return;
        }

        if (probe === "pos") {
            const p = ctx.read.scriptPos();
            if (!p) throw new Error("scriptPos:undefined");
            if (p.script !== "SC0000" || p.pos !== 0x1104 || p.valid !== true) {
                throw new Error("scriptPos:" + p.script + "," + p.pos + "," + p.valid);
            }
            return;
        }

        if (probe === "pos-invalid") {
            // 有 top 帧、只是这一拍位置读不到 ⇒ `valid:false` 且**缺 `pos`**（⛔ 不编 0）。
            const p = ctx.read.scriptPos();
            if (!p) throw new Error("scriptPos:undefined");
            if (p.valid !== false) throw new Error("scriptPos.valid:" + p.valid);
            if ("pos" in p) throw new Error("scriptPos.pos-present");
            return;
        }

        if (probe === "no-source") {
            // 没有帧源（`readings.frames == nullptr`）/ 宿主报 err ⇒ 三个口都 undefined。
            if (typeof ctx.read.frames() !== "undefined") throw new Error("frames:present");
            if (typeof ctx.read.scriptPos() !== "undefined") throw new Error("scriptPos:present");
            if (typeof ctx.game.localFrames() !== "undefined") throw new Error("localFrames:present");
            if (typeof ctx.game.mouse() !== "undefined") throw new Error("mouse:present");
            return;
        }

        if (probe === "no-top") {
            // `cur` 不活动（`top` 全 false）⇒ 没有执行点 ⇒ scriptPos undefined（⛔ 不拿 frames[0] 顶替）。
            if (typeof ctx.read.scriptPos() !== "undefined") throw new Error("scriptPos:present");
            return;
        }

        throw new Error("unknown probe: " + probe);
    },
};

export default plugin;
