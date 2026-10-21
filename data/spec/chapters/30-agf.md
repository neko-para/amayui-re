## AGF 图像

AGF 是 AGE 的图像容器（当前语料全部是 `ACGF` 变体）：一个 24 字节头 + 可压缩的 meta 段 + 可压缩的 body 段；version 2 的文件在 body 段之后紧跟一段 `ACIF` alpha 段。**段** = 容器中由长度界定的连续字节区（ALF 的 TOC 也是段）。

本版把 AGF 作为协议描述；实现细节（`age-format`）不是来源。AGF 的引擎读取/解码段已定位；字段级核验结果见文末状态表。

### 容器头

```text
[0,4)     "ACGF"
[4,8)     version（1 / 2）
[8,12)    unknown0（必须为 0）
[12,16)   metaUnpackedSize
[16,20)   metaUnpackedSize（重复）
[20,24)   metaPackedSize
[24, ...) meta 数据
```

- 头 24 字节，全部小端。
- 本版覆盖 version 为 `2` 的文件（语料全部是 version 2）；引擎也接受 version `1`，但语料没有 version 1 样本。version 不在 `{1,2}` 或 `unknown0` 非 0 时，引擎按“不是图像文件”拒绝。
- 引擎**不按 4 字节签名匹配** `ACGF`：它只把前两字节为 `BM` 的文件分流到 BMP 读取路径，其余文件按上面的 version / `unknown0` 判定后进入 AGF 路径。本版只覆盖 `ACGF` 签名；其它签名在语料里没有样本。
- `meta` 段长度由 `metaPackedSize` 给出；`metaUnpackedSize == metaPackedSize` 时原样存放，否则按 AGF 的 LZSS 解压。
- `meta` 段之后是 12 字节 body 头：

```text
[0]  unknown
[4]  bodyUnpackedSize
[8]  bodyPackedSize
```

- body 数据同样：相等即原样，否则 LZSS 解压。
- body 数据先按 `bodyPackedSize` 读入，解压目标是按 **`bodyUnpackedSize`** 分配的缓冲。解压例程只以“压缩数据读完”为终点，不检查实际写出的字节数：写出多于 `bodyUnpackedSize` 即越界（未定义行为）；写出少于它时，缓冲尾部保持分配时未清零的内容。实现必须把长度不符记为诊断，不得静默截断或补 0。

### AGF 的 LZSS

AGF 的 meta / body 与 ALF 的目录区、ACIF 的 alpha 用**同一个**解压例程、**同一组参数**（窗口 4096、写位置从 4078 开始、最短 3、最长 18、flag 低位先、位置 12 位）；参数只写在「ALF 归档」的「ALF 的 LZSS」，本节不重复。

★ 与 ALF **不同**的只是容器：AGF 的 meta 直接解到图像对象里（不校验长度），body 解到按 `bodyUnpackedSize` 分配的缓冲；ALF 的压缩块在 268 字节头之后。⇒ "参数相同"不等于"装载路径可以互借"。

本版只规定读取侧：两个长度相等即原样，否则 LZSS。写侧如何选择压缩或原样不在本版。

### ACIF / alpha（version 2）

version 2 的文件在 body 数据之后**紧接**（中间没有填充）一段 `ACIF`；缺这段或它的头不合法时读取失败。version 1 的文件不读 alpha。

```text
[0,4)     "ACIF"
[4,8)     version（1 / 2；其余版本拒绝）
[8,12)    0（非 0 拒绝）
[12,16)   alphaUnpackedSize
[16,20)   width
[20,24)   height
[24,28)   alphaUnpackedSize（重复）
[28,32)   alphaUnpackedSize（重复）
[32,36)   alphaPackedSize
[36, ...) alpha 数据
```

- 引擎把 ACIF 头的 `width` / `height` 与 meta 的 `width` / `height` 比较，不相等即拒绝。
- alpha 数据同样按"相等即原样，否则 LZSS"处理。
- **引擎不校验 alpha 的字节数**：使用端按"宽 × 高"逐行拷贝 ⇒ alpha 偏短是越界读（未定义行为）。实现必须把它记为诊断，不得静默补 0。

### 像素排布

meta 里的宽、高与 bpp：

- `width = u32 @ meta+20`，`height = u32 @ meta+24`，`bpp = u16 @ meta+30`；宽、高同时被 ACIF 头校验使用。
- 宽或高为 0 时本版不规定。

像素：

- 8bpp：调色板索引。调色板是 meta 内从 `+56` 起的一个 **1024 字节槽**（256 项 × 4 字节，每项前 3 字节按 B、G、R 解释），初值全 0，meta 覆盖该槽；引擎按 `2^bpp` 项取用（8bpp ⇒ 256 项）。
- 24bpp：BGR 三字节；
- 32bpp：BGRA 四字节（alpha 可被 ACIF 覆盖）；
- body 的行**不填充**：每行字节 = `bpp × width / 8`（8bpp：`width`；24bpp：`3 × width`；32bpp：`4 × width`）；
- 盘上是 **bottom-up**：解码到 top-down 时要反转行序；通道序为 BGR(A)。

### 核验状态

| 部分 | 状态 | 待核验什么 |
|---|---|---|
| `ACGF` 头 / 版本 / meta 与 body 段长度 | **已核验** | — |
| meta / body 的 LZSS | **已核验** | 与 ALF 同一例程（参数见「ALF 归档」） |
| `ACIF` alpha（version 2） | **已核验** | — |
| 宽高 / bpp / 调色板 / 行序 / 通道 | **已核验** | — |
| version 1 的 AGF | **无样本** | 找一个 version 1 的文件，确认它在没有 ACIF 时能读 |

**已建立关联**：AGF 读取入口、段长度、ACIF 校验与像素转换路径都已在引擎侧逐条对上。

### 与 ALF / ASM 的边界

- ALF 是归档容器，负责"文件从哪里取"；AGF 是图像容器，负责"像素怎样排"；ASM 是脚本协议，负责"指令怎样编码"。三者互不替代。
