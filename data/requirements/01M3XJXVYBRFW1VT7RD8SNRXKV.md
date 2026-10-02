# 翻译 patch 方案（唯一入库物 = 变更叠加层；data / src 均为实时视图）

- id: REQ-01M3XJXVYBRFW1VT7RD8SNRXKV
- type: req
- status: doing
- parent: REQ-01M3SVH2V332E4CJYN0BHH0YK8
- order: 10
- tags: [translation, patch]

## 范围
把译文从"一整棵文本树"改成 **patch（叠加层）**：入库的只有 patch，`data` 与 `src` 都是**实时计算出来的视图**。

## 口径（用户已定）
* **唯一资产 = patch**（入库的只有 `src` 相对基线的变更）。
* `data` = **原版 BIN** 实时计算的视图；`src` = **原版 BIN + patch** 实时计算的视图。
* patch **用 JSON + 控制脚本**（数据不透明、语义只在控制脚本的自描述里）；后续工具要能**直接分析**它。
* **对 BIN 而言 patch 存中文**（真源）：BIN 里的日文写法是构建时按
  `canEncodeCp932 优先 → 简→日字典 → 全角空格` 派生出来的（见 `docs/01-translation/patch-design.md` §2.5）。
* 必须提供必要工具：**查询 `data` 视图 · 查询 `src` 视图 · 编辑 patch**。

## 判据
1. **锚**：基线**指令序号**（非字节偏移、非 label、非字符串槽——三者都是派生量）＋ 内容摘要（`sha8`）；
   带 `baseSha`（基线指纹）与 `subsSha`（字典指纹）；任一变了即**显式报冲突**，不静默改写。
2. **往返 = 逐字节**：`原版 BIN + patch ⇒ 汇编` 与产物 BIN **逐字节相同**；
   `resultSha` 是这条判据在旧仓消失之后**唯一还能机械复核的证人**（由 `extract` 写、`verify` 只读）。
3. **唯一写入口**是控制脚本：生成 `data` / `src` 视图、从改过的视图反解回 patch、校验与基线的匹配；
   缺基线时报错而非静默跳过。
4. **落点**：基线与产物**走同一套解析层**（`散装优先 → SYS4INI.BIN/APPEND0N.AAI → *.ALF`），只是**根不同**：
   基线根 = `gameInstall/`（清单 `roots.gameInstall`）；产物根 = 旧仓 `install/` **仅迁移期读一次**。
   `raw-parts/`（ALF 的解包派生）与旧仓 `patch/BIN`、`gameInstall/_extracted/` **完全不引用**。

## 已交付（数据模型 + 迁移 + 视图 + 编辑回路）
* 工具：`pnpm tools patch`（模型 `tools/lib/patch.mjs` · 基线解析层 `tools/lib/bin-source.mjs` ·
  中文↔BIN 映射 `tools/lib/cn-jp.mjs` · 说明书 `tools/patch.md`）；
* 数据：`data/translations/patch.json`（说明书 `data/translations/patch.md`；清单条目 `translation/patch-data`）；
* 实测：**223/223 个官方脚本「基线 + patch ⇒ 逐字节相同」通过**（`pnpm tools patch verify`，8.6 s）；
* 视图：`pnpm tools patch view` 生成**同构的 `data` / `src` 两份文本**（`src` 显示 patch 里的**中文**，
  因为 BIN 里只是占位写法），落 `dist/views/`（生成物，不入库）；实测 223 个脚本 / 446 个文件 / 20 s；
* 编辑回路：`pnpm tools patch edit` 把**改过的 `src` 视图**反解回 patch（走同一条对齐路径、锚仍在基线行序上）；
  实测往返：改一处中文 ⇒ 反解 ⇒ 重新生成视图与改过的那份**逐字节相同**；改回原样 ⇒ 操作集与 `resultSha` 都回到原值；
* 基建契约：`tools/test/patch.test.mjs`（20 条：写路径 / 守卫能红 / 差分与重放的可再校验性 / 视图换字符串 / 中文落码）。

## 子节点
* `REQ-01M3XP1ZV3YF5EYHTK2A3KV4CE` **翻译视图重建**：机械视图与编辑回路已交付；
  剩下**旧式注释层**（页边界 / `// FROM:` / 原文存档块 —— 实测全部**可再算**，不进 patch）。

## 非目标
* 不引用旧仓 `install/`（**运行时**）、`raw-parts/`、`patch/BIN`（overlay 都是产物）；
* 不改基线语义（"基线"永远是原版 BIN；`data` 只是它的视图）；
* 真正的"文案更新"是另一张单（`翻译更新`）的业务，本节点只做基建。
