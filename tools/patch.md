# tools/patch.md — `tools/patch.mjs`（翻译 patch 控制脚本）的**落点说明**

> 它**不是**工具台账（那个要 append-only 文本 + `--describe`）；这里只写"它动哪片数据、为什么这么动"。
> 命令与自描述：**`pnpm tools patch describe`** / `status` / `baseline` / `extract` / `verify`。
> 设计与支撑观测：`../docs/01-translation/patch-design.md`。

## 它动哪片数据

| | 落点 | 读写 |
|---|---|---|
| **产物（唯一入库物）** | `data/translations/patch.json` | **唯一写入口 = `extract`**；`verify` 只读 |
| 生成物（**不入库**） | `dist/views/{data,src}/<脚本>.txt`（`--view`） | 可无限重算；真源只有基线 + patch |
| 输入（只读） | `data/translations/subs-cn-jp.json`（简→日写法字典） | 只读；指纹写进 patch 的 `subsSha` |
| 输入（只读） | `gameInstall/`（清单 `roots.gameInstall`） | 只读：基线由「散装优先 → ALF」实时解析 |
| 输入（只读，**仅提取期**） | 旧仓 `install/` | 只读一次，用来把汉化产物提取成 patch |
| 不引用 | 旧仓 `raw-parts/`、`patch/BIN`、`gameInstall/_extracted/` | ❌ 都是派生物 / overlay |

★ **绝对路径只写在 `corpus/assets.json` 的 `roots` 里**：本工具按清单解析根，不硬编码旧仓路径
（换机器 / 旧仓搬家都只改登记一处）——与 `tools/opcodes.mjs` 同一口径。

## 为什么这么动

* **为什么 patch 是唯一资产**：`data`（原版）与 `src`（原版 + patch）都是**实时视图**。
  实测 `src` 里的信息 **100% 可再算**（页来源、输入原文、原文存档块、页边界都能从基线 BIN 重算），
  所以"没有文本树会丢东西"这个顾虑不成立；真正要小心的是**工具算错**。
* **为什么锚"行序 + 摘要"**：插入一条指令会改掉后续所有字节偏移，而 `label_XXXXXXXX` 本身就是绝对偏移
  ⇒ 偏移与 label 都是**派生量**，当锚就会一插就错位。
* **为什么 patch 里**不记** label 地址**：地址随偏移漂；patch 只保留"这里是个 label 引用"这一结构事实，
  重建时由汇编器按符号重算。
* **为什么存中文**：见 `../data/translations/patch.md` §非显然的口径 第 1 条。
* **为什么判据是"逐字节"**：它比"指令流逐条相同"更强，而且能一次性抓出
  `comment` 被当注释滤掉、label 定义没跟着搬这类**不报错的错**。
* **为什么 `verify` 不许写**：`resultSha` 若能在 verify 里回写，判据就退化成自证循环。

## 怎么改

```bash
pnpm tools patch describe         # ★ 先看：字段 / 不变量 / 操作
pnpm tools patch status           # 当前规模 + 字典指纹是否一致
pnpm tools patch extract          # dry-run：提取并**逐条自证**，报告会写什么
pnpm tools patch extract --write  # 落盘（写后回读复验，不绿回滚）
pnpm tools patch verify           # 判据：基线 + patch ⇒ 逐字节相同
pnpm tools patch view             # 生成 data / src 视图 → dist/views/（生成物，不入库）
pnpm tools patch edit             # ★ 改过 src 视图之后，反解回 patch（dry-run；--write 落盘）
pnpm test                         # 基建契约：tools/test/patch.test.mjs
```

* ❌ **不要手改** `patch.json`（唯一写入口是 `extract`；`verify` 只读）；
* ❌ **不要**把 `--extract` 的部分结果写盘（`--name` / `--limit` 只能是 dry-run —— 否则会删掉其余条目）；
* ❌ **不要**在 patch 里存 BIN 里的日文写法（那是派生物，反推回中文不可逆）；
* ✅ 基线换了 ⇒ 让 `verify` **报冲突**，不要放宽检查。
