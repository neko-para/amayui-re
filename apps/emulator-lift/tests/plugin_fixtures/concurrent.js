// C2a 夹具 · `plugin/concurrent-arm`
//
// 判据（红了意味着什么）：
//   ① 一次 arm 三个具名门；**一读点最多触发一个**（注册顺序）⇒ 同拍两个都在电平内时，
//      只有排在前面的那个 fire（后面的晚一个读点才 fire）；
//   ② 返回值是**完整替换**：没出现在返回数组里的 trigger 全部消失 ⇒ 这里每一步只把"还没到的"
//      放回去（⛔ 不是"清空后自动重来"）；
//   ③ `[]` = done。
// ★ `name` 是插件侧状态名；`ev.name` 是分派键 —— 链不靠 step 计数器。
// ★ `afterPosition` 的第一个参数是**脚本名**（宿主算出的 `now.script`）；三段都在 "SCENE1" 上，
//   只靠位置区间区分 ⇒ 用例只需要改读数里的 pos。
const SCRIPT = "SCENE1";

const plugin = {
    start(ctx) {
        return [
            ctx.trigger.afterPosition(SCRIPT, 0x10, 0x20, { name: "hud:show" }),
            ctx.trigger.afterPosition(SCRIPT, 0x30, 0x40, { name: "hud:hide" }),
            ctx.trigger.afterPosition(SCRIPT, 0x50, 0x60, { name: "edit:flag" }),
        ];
    },
    onTrigger(ctx, ev) {
        ctx.state.fired = (ctx.state.fired || 0) + 1;
        ctx.state.last = ev.name;
        switch (ev.name) {
            case "hud:show":
                return [
                    ctx.trigger.afterPosition(SCRIPT, 0x30, 0x40, { name: "hud:hide" }),
                    ctx.trigger.afterPosition(SCRIPT, 0x50, 0x60, { name: "edit:flag" }),
                ];
            case "hud:hide":
                return [ctx.trigger.afterPosition(SCRIPT, 0x50, 0x60, { name: "edit:flag" })];
            case "edit:flag":
                return [];
            default:
                ctx.fail(ev.name, `unknown name: ${ev.name}`);
                return [];
        }
    },
};

export default plugin;
