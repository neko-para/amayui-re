# shape 缺项：OPTION 足迹批暴露的未声明载体 / 成员（T_FadeTimer · T_Scene_42c +8/+c/+10 · T_Command 1200B · 网格/列表项族）

- id: REQ-01M6AKCBTRNBZQNERZGPT8VMPD
- type: req
- status: open
- parent: REQ-01M52F3ASMGWQXX64JSA18MP2N
- tags: [shape]

## 一句话

2026-11-01 的 OPTION 足迹残差收口里，有十几处**机器状态已经定住**（谁写、谁读、领域角色），
但 `data/shape/types.json` **没有对应载体 / 成员** ⇒ `bindings.json` 的 `storage` 只能留空，
`spec` 那几条因此缺"领域状态 ↔ 机器状态"的指针。本单只做 shape 侧的补登记与复核，⛔ 不改 spec 语义。

## 缺口清单（按载体分）

- **`T_FadeTimer`**：计时器对象的成员（睡眠 / 自动消息两个实例的**停表格**、计数、单位、盖章）——
  `CORE-OPS-SLEEP-C8` 与 `CORE-OPS-WAIT` 的计时器段需要它。
- **`T_Scene_42c`（或对应的记录类型）**：`+8 / +c / +10` 三格（读档恢复门 / 零读者标志 / 来源文件号的兄弟格）——
  `CORE-OPS-SCENE-TABLE-FLAGS` 的标志两格身份已定，但类型里没有成员起点。
- **`T_Command` 的记录数组宽度**：`0x3f5c` / `0x440c` / `0x7e994` 等 —— 其中两处是 **1000×20 / 100×12** 的副块，
  `types.json` 里现在的宽度（4 字节）与实际（1200 字节）不一致。
- **网格记录族 / 列表项记录族 / 引擎侧条目表**：三族的记录类型整体缺失（`T_Scene_460` 之外的另一类 740 字节记录、
  列表项 `T_414@0xd0` 的 120 字节元素、按组号索引的 572 字节值表）—— 批 4 的多条合同因此没有 `storage`。
- **`T_Sys3Draw@0x618`**（8 dword 块的第 5 格 = 精灵基色）：成员未声明，且**同格另有掩码用法**，复核时要一并注记。
- **`T_Scene@0xd5b4`**（10 格光源启用标志）与 `T_Effect3D`（三维特效对象）—— `0x32f` / `0x324` 的合同需要它们。

## 判据（做完的样子）

1. 上述载体在 `data/shape/types.json` 里**有类型、有成员起点、有宽度**；同一偏移的多义（如 `+0x420` 的三载体、
   `T_Sys3Draw@0x618` 的掩码用法）在注记里写明。
2. 对应规则的 `bindings.storage` 从空改成 `shape:<类型>@<成员起点>`，且 `pnpm tools spec validate` 的 #6 依旧绿
   （现在留空是"解析不到"，改完必须"解析得到"）。
3. 改完跑 `pnpm tools shape claims --sample` 与相关守卫，确认新成员没有与既有 claims 冲突。

## 不做什么

- ⛔ 不改 spec 正文 / 残差（那是另一条线；本单只补 shape）。
- ⛔ 不为了"让 storage 有东西"编造成员名：没有观察的格仍然留空。
- ⛔ 不手工编辑 `data/spec/**`：本单产出只落 `data/shape/` 与其说明文档。
