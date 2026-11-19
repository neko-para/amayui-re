// C2a 夹具 · `start` 与 `onTrigger` **不成对** ⇒ 装载必须失败（合同 §4.6：两者必修成对）
const plugin = {
    start(ctx) {
        void ctx;
        return [];
    },
};

export default plugin;
