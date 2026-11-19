// C2a 夹具 · `plugin/concurrent-arm` 的**反面**：同一返回数组里重复 `name` ⇒ Run fail 并点名。
// ★ 为什么不让 JS 自己发现：d.ts 明文"同一 armed 集合内唯一（运行期查重）"⇒ 判据在宿主
//   （`TriggerRunner::install`），这里只负责造出那个形状。
const SCRIPT = "SCENE1";

const plugin = {
    start(ctx) {
        return [
            ctx.trigger.afterPosition(SCRIPT, 0x10, 0x20, { name: "dup" }),
            ctx.trigger.afterPosition(SCRIPT, 0x30, 0x40, { name: "dup" }),
        ];
    },
    onTrigger(ctx) {
        return [];
    },
};

export default plugin;
