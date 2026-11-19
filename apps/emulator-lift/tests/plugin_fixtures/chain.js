// C2a 夹具 · `control-script/start-on-trigger-chain`
//
// 合同：`docs/03-emulator/plugin-runtime.md` §4.6 757–795（`loadSave(slot)` 的形态：靠**具名**
//       trigger 接线，⛔ 不用 step 计数器 / ⛔ 不用 ctx.state 记步）。
// 父级裁决：`.tmp/plugin-runtime/c2-decisions.md` D-C2-8（rowY 只用实测表；7–9 没有实测 ⇒ 抛）。
//
// ★ 数值全部是实测表（`docs/03-emulator/scripts/SAVE.md`）：行距 ≈59.5，⛔ 不用公式猜。
// ★ 两道确认的坐标（640,323 / 690,323）来自 `save04-fault.plan` —— 它们是"配方在跑、没报错停住"
//   的形态，⛔ **不是**"语义已证"，所以链到第一道确认就 `return []`（done），不写"加载成功"。
export function loadSave(slot) {
    const rowY = { 0: 91, 1: 150, 2: 209, 3: 269, 4: 328, 5: 388, 6: 447 }[slot];
    if (rowY === undefined) {
        throw new Error(`slot ${slot} has no measured row (only 0-6 measured)`);
    }
    return {
        start(ctx) {
            ctx.input.click(1070, 480);
            return ctx.trigger.afterPosition("SAVE", 0x1104, 0x11c4, { name: "load:save" });
        },
        onTrigger(ctx, ev) {
            switch (ev.name) {
                case "load:save":
                    ctx.input.move(300, rowY);
                    return ctx.trigger.atReadPoint("any", 1, { name: "load:row" });
                case "load:row":
                    ctx.input.click(300, rowY);
                    return ctx.trigger.afterPosition("SBUNKI", 0x09f8, 0x0b18, { name: "load:confirm1" });
                case "load:confirm1":
                    ctx.input.click(640, 323);
                    return [];
                default:
                    ctx.fail(ev.name, `unknown name: ${ev.name}`);
                    return [];
            }
        },
    };
}

const plugin = {
    start: (ctx) => loadSave(Number(ctx.params.slot)).start(ctx),
    onTrigger: (ctx, ev) => loadSave(Number(ctx.params.slot)).onTrigger(ctx, ev),
};

export default plugin;
