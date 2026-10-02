# corpus/assets/samples/ —— 三套 AGE 格式的样本（**只登记外部路径，不入库**）

> ⚠ **本目录不存在**（有意为之）：样本是**原始游戏文件**，用户口径明确 **ALF / AAI 这类很大的原始游戏文件不入库**。
> 因此样本**只在清单里登记来源与 sha256**，盘上一个字节都不放：
> `corpus/assets.json` 的 `assets/samples-alf` / `assets/samples-agf` / `assets/samples-asm`（`storage: external-only`）。
> **怎么查**：`pnpm tools corpus list` 看三条条目的 `origin` 与 `storage`；`pnpm tools corpus describe` 是清单的 schema 真源。
> **怎么改**：用 `pnpm tools corpus set <id> '<patch>' --write`（唯一写入口是 `tools/corpus.mjs`，**不要手改** `assets.json`）。

## 它是什么

三套 AGE 容器格式的**真实样本**，专供"解包 → 重打包逐字节相同"这类守卫使用。
选样口径**不是"随便挑几个"**，而是"取满足覆盖的最小真实件"：

| 条目 | 格式 | 选样口径 |
|---|---|---|
| `assets/samples-alf` | ALF 归档（索引 `*.AAI` + 数据体 `*.ALF`） | 取**最小的归档家族** `APPEND02`：索引 5.8 KB + 数据体 49.4 MB。索引与数据体**必须成对**，否则验不了载荷搬运 |
| `assets/samples-agf` | AGF 图像（ACGF） | 必须覆盖两条分支：`MI042.AGF` 的 meta 是**压缩**形态（1080→159），`MI040.AGF` 是**原样**形态（压不小才原样）；`SO002.AGF` 尺寸更小 |
| `assets/samples-asm` | ASM 脚本（`.BIN`） | 小件（284/440 B）验证极短脚本，中等件（21/52 KB）验证指令流与字符串 |

## 非显然的口径

* **为什么不留副本**：样本是原始游戏文件（ALF 家族 49.4 MB）；把游戏文件搬进仓库既不合口径、也换不来额外保障 ——
  守卫要的是"**读的时候确认它没被换过**"，那由 `origin[].sha256` 提供（清单 #4 会对不在库的文件**现算现比**）。
* **测试在文件缺席时跳过**：`gameInstall` 是每台机器不同的绝对路径（写在清单的 `roots` 里）。
  fresh clone 上样本不在 ⇒ 相关用例**跳过并打印原因**，而不是红。
  ⇒ 判据的**实测证据**（哪台机器、哪个 HEAD、哪几个文件逐字节相同）记在需求节点 `REQ-01M3TCP0B0CTCSCJXYKM2ESKHG` 的凭据段。
* **ALF 的索引与数据体是两个文件**：`.AAI`/`SYS?INI.BIN` 只有目录区（LZSS 压缩），`.ALF` 只有裸载荷字节。
* **样本不是夹具**：不要为了让测试通过而改动它们（截断、重排、改偏移）。真要换样本，在条目 `note` 里写清为什么换。
* **`8bpp` 的调色板在 meta 里**，不在 body 里；`ACIF` 的 alpha 数据**也会被压缩**
  （旧仓注释只说 meta/body 会压；实测按"压得小就压、否则原样"）。

## 怎么查

```bash
pnpm tools corpus list                 # 三条样本条目（storage 一律 external-only，dest 为 null）
pnpm tools corpus validate             # #4：对不入库的文件现算 sha256 并与清单比对
pnpm tools age-format verify           # 对样本跑「解包 → 重打包逐字节相同」（M2 的判据）
```

## 怎么改

* **换/加样本**：先确认新件仍满足上表的**覆盖口径**，再用 `pnpm tools corpus set` 改 `origin`（含新 sha256）。
* **不要**给样本登记 `dest`：`external-only` 与 `dest` 必须自洽（守卫 #2），给了 `dest` 就会被判红。
* **不要**给样本加"期望输出"之类的旁挂文件：判据是**逐字节相同**，记录期望值等于把结论抄第二遍。
