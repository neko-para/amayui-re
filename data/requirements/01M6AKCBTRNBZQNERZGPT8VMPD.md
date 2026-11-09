# shape 缺项：OPTION 足迹批暴露的未声明载体 / 成员（T_FadeTimer · T_Effect3D · 四个副块 · 三族记录类型 · elem14 五格）

- id: REQ-01M6AKCBTRNBZQNERZGPT8VMPD
- type: req
- status: dropped
- parent: REQ-01M52F3ASMGWQXX64JSA18MP2N
- dropped_reason: 引擎逆向 / shape 普查这条线的产物是知识台账与 spec 的机器本体，消费者是「按 spec 重写」的实现线；该实现线已冻结，lift 的真源是拆壳件字节 + .lst（apps/emulator-lift/README.md §1/§2.1），不消费这两层。lift 需要的取证按 M1/M2/M4 的具体单做，不再走常态化普查。需要时从 git 恢复。
- tags: [shape]

## 一句话

2026-11-01 的 OPTION 足迹批里，机器状态（谁写 / 谁读 / 领域角色）已经定住、但 `data/shape/types.json` 没有
载体或成员的，按 `types.json` 逐条实测核过后是 **5 项**。`bindings.json` 的 `storage` 因此只能留空，
spec 那几条缺"领域状态 ↔ 机器状态"的指针。本单只补 shape 侧登记与复核，⛔ 不改 spec 语义。

原稿的 6 条里有 2 条其实已声明、1 条指错类型、1 条把"故意只声明首槽"当成"宽度写错"（见「已核对、不在本单」）。

## 缺口（按类型分）

1. **`T_FadeTimer` 成员表**（现 0 成员 / `size` 28）：七个 dword 全有站点 —— 构造 `0x453a24..0x453a3d` 写满，
   访问器 `0x453a60` / `0x453af0` 读 `0x8` / `0x14` / `0x18`。容器已经是
   `T_Command@0x68ec0 = T_FadeTimer × 32`。⇒ **卡在载体**：三个访问器都没有 `arg` 载体
   （`propagate` 的 `interior` 规则只在 `count === 1` 时换类，内嵌数组的 interior 解不出类型）。
   实测：只补 3 条载体，`T_FadeTimer` 立刻从"0 成员"变成 21 处残差、覆盖 `0x0..0x18` 七个格。
2. **`T_Effect3D`**：类型不存在；`0x32f` / `0x324` 的合同需要它。
3. **引擎侧四个副块**（`T_Command` 上，全部未声明）：`0x3f5c`（活动表）/ `0x440c`（快照）——
   `0x41edfc` 搬 `0x4B0` 字节（`0x12C` 个 dword = 100×12）；`0x4f458` / `0x54278` —— `0x41ee06`
   搬 20000 字节（1000×20）。**字段访问仍然为零**；宽度证据现由 `pnpm tools shape extents` 给出
   （`rep movsd` / `memcpy` 的 `{基址, 字节数, EA}`）—— ⛔ 但 extent **不解成员、不放宽 G4**，
   所以这四项仍要按"成形 + 裁决"补，不是自动生成。
4. **三族记录类型**：网格记录（740 字节，`0x4AC120` 的 `0xB9` dword 副本；与 `T_Scene_460` 不是同一类）·
   `T_414@0xd0` 的 120 字节行命令元素 · 场景侧按组号索引的 572 字节值表（值起于结点 `+0x10`，
   `T_treenode_592` 只有结点外壳）。
5. **`T_Scene_elem14` 的 `+0x4` / `+0x10`**：纹理记录五格（来源文件 / 颜色键 / 恢复重建门 / 零读者标志 /
   空白创建标记）现只声明 `0x0` / `0x8` / `0xc`；缺的两格**必须由 EA 定哪格是哪格**，⛔ 不按顺序猜。

## 已核对、不在本单（原稿误述，记下来防复发）

- `T_Sys3Draw@0x618`：**已声明**（width 4，注记"位掩码"）；缺的只是"同格另有精灵基色用法"的注记。
- `T_Scene@0xd5b4` 的十格光源标志：**已声明**为 10 个独立 dword（`0xd5b4..0xd5d8`）；缺的是"十格数组"的
  成形与语义。
- `T_Scene_42c +8/+c/+10`：`+0x8` 已在；该类型是 std::map 结点，与标志两格无关；两格实际在
  `T_Scene_elem14@0x8/@0xc`，且 `CORE-OPS-SCENE-TABLE-FLAGS` 已绑定。⇒ 并入上面第 5 项。
- `0x7e994` 的"4 字节 vs 1200 字节"：**不是宽度写错** —— G4 要求数组末尾地址在语料里被引用，而该表末尾
  零引用 ⇒ 只声明首槽（`note` 已写明）。改法要等兄弟单的"块搬移 extent"证据；⛔ 不许为它放宽 G4。

## 判据（做完的样子）

1. 上述 5 项在 `data/shape/types.json` 里有类型、成员起点、宽度；同一偏移的多义（`+0x618` 的掩码 / 基色、
   `+0xd5b4` 的十格）在 `note` 写明。
2. `pnpm tools shape claims data/shape/types.json` 与 `... carriers.json` 退出 0；新成员站点全部被解释，
   解释不了的**如实进残差**（⛔ 不为了清零编成员）。
3. 对应规则的 `bindings.storage` 从空改成 `shape:<类型>@<成员起点>`；`pnpm tools spec validate` #6 绿。
4. `T_FadeTimer` 的载体必须有**调用点** witness（G16）：`0x42196e lea ecx,[esi+0x68ec0]` → `0x421974 call`，
   其余访问器同法；⛔ 不许按"函数看起来是计时器的"编载体。

## 不做什么

- ⛔ 不改 spec 正文 / 残差（那是另一条线）。
- ⛔ 不为了"让 storage 有东西"编造成员名：没有观察的格仍然留空。
- ⛔ 不手工编辑 `data/spec/**`：本单产出只落 `data/shape/` 与其说明文档。
- ⛔ 不加模型规则 / 不改 `census --debt` 的判据 —— 那是兄弟单
  `REQ-01M6BPB07YQ73NYVMYNFPQ1X26`（shape 度量修复：零成员类型 · 未解析基址按来路分组 · 块搬移 extent ·
  interior 进内嵌数组）。

## 怎么查（按需补齐的入口已落地）

```bash
pnpm tools shape debt                         # 哪些类型还是零成员 / 尾部缺口
pnpm tools shape need T_FadeTimer             # ★ 载体候选（调用点 EA）+ 未声明偏移 + 可粘贴草稿（⛔ 不落盘）
pnpm tools shape need T_FadeTimer@0x14        # 只看某一格（已声明 ⇒ 直接告诉你落在哪个成员里）
pnpm tools shape extents sub_41ED80           # 块搬移的宽度证据（1200B / 20000B …）
```

★ 草稿粘贴后 `claims` 会判 G1/G2/G4/G8/G16；还漏的偏移用 `claims --by-type` 看残差，再回 `need`。
