# spec 文档与需求树治理二批：planned 对齐 / 键名复述 / 路径标记

- id: REQ-01M5AKDRRHKWEXQV1M7KTBG2KE
- type: req
- status: open
- parent: REQ-01M5851Z51V1MTSWH414M7WN5Q
- order: 65
- tags: [engine spec docs infra]

## 背景

一批（`REQ-01M5AJWYV…`）已修掉 doc 层失效指针。以下属于**基建 / 口径**，转二批：

1. **planned ↔ 机制对齐（数据层）**：`data/spec/audit.json` 的 `planned[]` 还是旧域 id（8 个），
   与 `data/spec/core/README.md` §2.1 的映射尚未落到 JSON。等 `data/spec/chapters/core/` 落地时同批改
   `planned[]` 与 `outOfScope[].chapter`，并加不变量"每个机制都有需求子单"。
2. **键名复述**：文档仍在复述 `bindings[].rule` 等键名，与 `AGENTS.md` §10「JSON 不复述 schema」冲突且无守卫
   （漂移实例：草案里曾写 `audit.residual`，该键不存在）。→ 二选一：改成"字段见 `pnpm tools spec describe`"，
   或加一条一致性守卫；⛔ 不许两处都写又不钉。
3. **需求树路径写法**：需求单大量引用 `scripts/…` / `app/amayui-emulator` / `install/` 等**旧仓**路径，
   也有本仓 `chapters/…`（应为 `data/spec/chapters/…`），且没有"旧仓 / 本仓"标记 ⇒ 无法机械判定。
   → 约定前缀（`old:` / `repo:`）+ 只校验 `repo:` 的守卫（本仓路径必须存在）。
4. **需求单前提会漂**：开单时写"文件:行 + 现查命令"，不写"当前状态"式断言（一批里就有一条按不存在的前提写的）。
5. **分发边界**：`chapters/README.md` 是分发物；已删掉指向 `data/spec/core/README.md` 与仓内路径的句子。
   若以后要分发机制地图，先把它纳入 `spec.json` 的分发清单 —— 这是产品取舍，等用户裁决。
6. **机制表的机器形态（暂缓）**：机制表现在是散文表（无机制 / dependsOn 的机器字段）。
   先简单处理：等工具真的需要读它（例如按机制生成报告）时再落 JSON，届时同批落 `planned[]` 映射。

## 判据

* 1 落地后 `spec validate` 仍绿；`planned[]` 每个元素都能在机制表找到归属（`input` / `presentation` 明确标 core 外）。
* 2 二选一执行完，并有一条守卫（或明确记录"不改，理由 X"）。
* 3 守卫能区分 `repo:`（本仓路径必须存在）与旧仓引用（不校验存在性）；现有可疑引用有清单可查。
