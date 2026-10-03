# 01 · 翻译域

## 1. 现状（旧仓实测）

| 件 | 旧仓位置 | 规模 | 性质 |
|---|---|---|---|
| 日文只读基线 | `data/` | 941 文件 / 82.9 MB | 上行（源语言侧），**只读** |
| 译文 | `src/` | 941 文件 / 91.8 MB | 与 `data/` 同构（370 相同 / 571 不同） |
| 字体 | `res/fonts/` | 含 CN/JP 合并字体（`Amayui-CN_cnjp*.ttf` 等） | 交付素材 |
| 补丁产物 | `patch/` | LFS + 忽略（BIN / AGF 等） | 编译产物 |
| 翻译管线 | `scripts/`（一半） | `translate.js` / `reflow*.js` / `sync-*.js` / 抽取与校验脚本 | 工具（按"重建"处理，不迁移） |

旧仓的行尾实测：`src/` 抽样 300 个里 297 个纯 LF、**3 个混合行尾** —— 这正是新仓 `.gitattributes` 首行
`* text=auto eol=lf` 要钉死的东西（见 `../00-origin/decisions.md` §3）。

## 2. 本轮落点

```text
data/translations/README.md   ← 本域的落点说明（本文件之外没有别的产物）
```

**本轮不搬 941×2 个文件**（§0 的硬约束），只登记指向：见 `corpus/assets.json` 的
`translation/baseline` 与 `translation/translated`。

## 3. 数据模型：**已拍板**（2026-09，用户定案）——patch 叠加层

详见 **[`patch-design.md`](./patch-design.md)**（评估 + 设计 + 支撑观测）。三句话版：

* **入库的只有 patch**（`src` 相对基线的变更叠加层）——它是本域**唯一资产**；
* **`data` = 原版 BIN 的实时视图**，**`src` = 原版 BIN + patch 的实时视图**；两者都**不入库**；
* **旧仓 `install/` 不再被运行时引用**（混杂原始数据与翻译产物、纯派生物）——
  只在**迁移期读一次**，用来把汉化产物**提取成 patch**；`raw-parts/` 则完全不引用。

**已实施**（进度看 `pnpm tools requirements plan`）：

| | 落点 |
|---|---|
| 控制脚本 | `pnpm tools patch`（`describe` / `status` / `baseline` / `extract` / `verify`；模型 `tools/lib/patch.mjs`） |
| 数据（唯一入库物） | `data/translations/patch.json`（说明书 `data/translations/patch.md`；清单 `translation/patch-data`） |
| 判据 | `pnpm tools patch verify` —— 基线 + patch ⇒ **逐字节**相同（旧的 941×2 文本树不再是真源） |

### 3.0 已落定的资源（数据模型的**输入**）

* **字体已入位**：`corpus/assets/fonts/`（分发字体 `Amayui-CN_cnjp{,-Bold}.ttf` + 上游基底 Sarasa 7z）。
* **简→日写法占位字典已入位**：`data/translations/subs-cn-jp.json`（旧仓 `res/subs_cn_jp.json`）。
  它决定"文本层写哪个码位"，与上面字体的 cmap 替换**成对**（见 `../../data/translations/README.md`）。
* **UI 图片已入位**（生效版 + 版本表）：`corpus/assets/ui-images/`；中文 UI 不落在这套数据模型里，
  它是**烘焙产物**（AGF overlay），见 `../02-engine/README.md` §6。

### 3.1 被否决的备选（留档，免得以后重开）

| 备选 | 为什么没选 |
|---|---|
| 保留 `data/` + `src/` 双份文本树 | 同一份信息四处副本（BIN / `data` / `src` / overlay）、无唯一真源；且 `data` 是**可再算**的 |
| 单份文本真源 + 机读标记语法 | 仍是"一整棵树"；而实测 `src` 的信息 **100% 可再算**（见 `patch-design.md` §2.2）⇒ 没必要发明新标记层 |
| 把注释嵌进 BIN（`comment` 0x1A7） | 机制**可行**（实测 25/25 稳定），但会让发行物带创作元数据、并与既有 `▼G####` 标记混在一起；留作后备 |

### 3.2 其它待定

* **BIN 内嵌中日文的前瞻**：编码、切换时机、与 `packages/age-format` 的关系（M2 之后才有容器层结论）。
* **旧式注释层**（页边界 / `// FROM:` / 原文存档块）：节点
  `REQ-01M3XP1ZV3YF5EYHTK2A3KV4CE`。★ 视图与"改过的视图 ⇒ 反解回 patch"**已交付**（`patch view` / `patch edit`）。
* **patch 的物理切分**（单文件 6.74 MB 已可行；并行编辑冲突变痛时再按脚本分片，纯物理切分不改语义）。

## 4. 本轮明确不做

* ❌ 不搬 `data/` / `src/` 任何一个文件（它们现在是**视图**）；❌ 不把视图落库。
