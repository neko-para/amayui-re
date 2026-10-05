# 翻译 patch 方案（唯一入库物 = 变更叠加层；data / src 均为实时视图）

- id: REQ-01M3XJXVYBRFW1VT7RD8SNRXKV
- type: req
- status: done
- parent: REQ-01M3SVH2V332E4CJYN0BHH0YK8
- order: 10
- verify: tools/test/patch.test.mjs#★ 发布物：data/translations/patch.json 处于规范形态
- tags: [translation, patch]

## 范围
把译文从"一整棵文本树"改成 **patch（叠加层）**：入库的只有 patch，`data` 与 `src` 都是**实时计算出来的视图**。

## 口径（用户已定）
* **唯一资产 = patch**（入库的只有 `src` 相对基线的变更）。
* `data` = **原版 BIN** 实时计算的视图；`src` = **原版 BIN + patch** 实时计算的视图。
* patch **用 JSON + 控制脚本**（数据不透明、语义只在控制脚本的自描述里）；后续工具要能**直接分析**它。
* **对 BIN 而言 patch 存中文**（真源）：BIN 里的日文写法是构建时按
  `canEncodeCp932 优先 → 简→日字典 → 全角空格` 派生出来的（见 `docs/01-translation/patch-design.md` §2.5）。
* **没有变更的脚本不进 patch**（"没改"不记录；空条目只会让 diff 变大）。
* 必须提供必要工具：**查询 `data` 视图 · 查询 `src` 视图 · 编辑 patch**。

## 判据（全部已达成）
1. **锚**：基线**指令序号**（非字节偏移、非 label、非字符串槽——三者都是派生量）＋ 内容摘要（`sha8`）；
   带 `baseSha`（基线指纹）与 `subsSha`（字典指纹）；任一变了即**显式报冲突**，不静默改写。
2. **往返 = 逐字节**：`原版 BIN + patch ⇒ 汇编` 与产物 BIN **逐字节相同**；
   `resultSha` 是这条判据的机械证人（`extract` 时 = 旧仓当年那份产物；**编辑之后** = 本工具新产物的 sha）。
3. **唯一写入口**是控制脚本：生成 `data` / `src` 视图、把编辑清单**逐条应用**成 patch
   （`find --edits` 生成清单 → `set` **逐条应用**；或 `edit` 反解已改好的视图文件）、校验与基线的匹配；
   缺基线时报错而非静默跳过。★ "改文案"不拆成"改视图文件 + 另跑同步"两步，也不做"无条件全库替换"：
   前者隔着"视图可能是旧的"这个**静默**窗口，后者没人逐条看过（见 `REQ-01M43YPP863JQW6JJQD6GK6BCQ`）。
4. **落点**：基线与产物**走同一套解析层**（`散装优先 → SYS4INI.BIN/APPEND0N.AAI → *.ALF`），只是**根不同**：
   基线根 = `gameInstall/`（清单 `roots.gameInstall`）；产物根 = 旧仓 `install/` **仅迁移期读一次**。
   `raw-parts/`（ALF 的解包派生）与旧仓 `patch/BIN`、`gameInstall/_extracted/` **完全不引用**。

## ★ 范围（这里犯过一次错，已修，必须记住）
**范围 = 基线根里全部能反汇编的 AGE 脚本（941 支），由"产物 ≠ 基线"定，不按名字筛。**

曾经我拿旧仓 `scripts/annotate-speaker.js` 的正则 `/^(SC|SP)/ || /^\$\d+\$(SC|SP)/`（223 支）
当范围，还把它叫成「官方集 = 翻译管线处理过的脚本」。**两件事都不成立**：那个正则只表示
"旧管线给这批做过**页 / 说话人标注**"；而旧仓 `src/` 覆盖 **941** 个文本，被它挡在门外的 719 支里
**有 247 支产物与基线不同（= 有译文）**。⇒ patch 当时只覆盖 207 支，**漏掉 247 支真译文**；
口径改对后是 **453 支**有条目（487 支无变更不进 patch；另有 **1 支是旧仓的坏文件**，迁移时跳过、不继承）。

★ 一句话纪律：**"没有 `// FROM:`" 只说明没做过页标注，不说明没有译文。**
标注口径在代码里的名字是 `SPEAKER_FILTER` / `annotated`（**只是标签**，不是范围）。
★ 同一类错在**视图层**又犯过一次（视图只按标注集生成 ⇒ 全库检索少报）：
见 🐞 `REQ-01M43Y83BPG8E93NP0ESPB7N7S`。

## 交付物
* 工具：`pnpm tools patch`（`describe` / `status` / `baseline` / `extract` / `verify` / `view` / `find` / `set` / `edit`）；
  模型 `tools/lib/patch.mjs` · 基线解析层 `tools/lib/bin-source.mjs` · 中文↔BIN 映射 `tools/lib/cn-jp.mjs` ·
  说明书 `tools/patch.md`；
* 数据：`data/translations/patch.json`（说明书 `data/translations/patch.md`；清单条目 `translation/patch-data`）；
* 视图落点：**持久侧只放依赖不可变输入的东西** —— `dist/index/base.json`（**基线索引**：名单 + 逐支指纹 +
  **codec 指纹** + 基线指纹）+ `dist/views/data/*.txt`（base 文本）；`dist/views/src/*.txt` 只按需物化（整篇改），
  草稿账本 `dist/views/manifest.json` 记它的来源。★ 全部不入库。

## 实测（可由命令复核，不手抄）
* **941 支全覆盖**：patch 里 **453 支**逐字节复验通过 ＋ **487 支无变更**经「与基线逐字节相同」复核；
  另有 **1 支是旧仓的坏文件**（`$1$IMINIT.BIN`：产物是 64 B 空壳、与游戏自带的 `$4$IMINIT.BIN` 逐字节相同，
  而它的文本树一行中文都没有）⇒ **迁移时用 `--skip` 跳过、不继承**，构建保留游戏原版。
  ★ 这**不进数据模型**（`patch.json` 里就是没有那一条）：跳过是命令行参数、理由写在文档里；
  记录与证据见 `REQ-01M3YF5120V9WXB7NF36GVP52G`。
* **规范形态**：脚本名按字典序、条目内按基线行序、一行一个 op；重跑提取**必得同字节**；
* **编辑往返**：改一处中文 ⇒ 反解 ⇒ 重新生成 `src` ⇒ 与改过的那份**逐字节相同**；改回原样 ⇒ 操作集与 `resultSha` 都回到原值；
* 基建契约：`tools/test/patch.test.mjs`（条数跑 `pnpm test` 看，不手抄）。

## 相关节点
* 兄弟节点 `REQ-01M3XP1ZV3YF5EYHTK2A3KV4CE` **翻译视图：旧式注释层**：视图 / 检索 / 直改链已随本节点交付；
  它还挂着**注释层**（页边界 / `// FROM:` / 原文存档块）。★ 注意：**注释层的范围 ≠ patch 的范围**（见上）。
* 子节点 `REQ-01M43YPP863JQW6JJQD6GK6BCQ` **按锚直改 op**（`find --edits` 生成清单 + `set` 逐条应用）。
* 子节点 `REQ-01M45AGK4JAGCX4KMZA6KRGBR1` **基线索引 + merge on read**（持久侧只依赖不可变的东西）。

## 非目标
* 不引用旧仓 `install/`（**运行时**）、`raw-parts/`、`patch/BIN`（overlay 都是产物）；
* 不改基线语义（"基线"永远是原版 BIN；`data` 只是它的视图）；
* 真正的"文案更新"是另一张单（`翻译更新`）的业务，本节点只做基建。
