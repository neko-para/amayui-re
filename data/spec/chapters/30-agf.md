## AGF 图像

AGF 是 AGE 的图像容器（当前语料全部是 `ACGF` 变体）：一个 24 字节头 + 可压缩的 meta 区 + 可压缩的 body 区，后面可跟一段可选的 `ACIF` alpha 数据。

本版把 AGF 作为协议描述；实现细节（`age-format`）不是来源。AGF 的引擎读取/解码段已定位（错误串 `AGFヘッダの読み込み時...`），但**字段级核验尚未完成**，见文末状态表。

### 容器头

```text
[0,4)     "ACGF"
[4,8)     version（语料恒为 2）
[8,12)    unknown0（语料为 0）
[12,16)   metaUnpackedSize
[16,20)   metaUnpackedSize（重复）
[20,24)   metaPackedSize
[24, ...) meta 数据
```

- 头 24 字节，全部小端。
- `meta` 区长度由 `metaPackedSize` 给出；`metaUnpackedSize == metaPackedSize` 时原样存放，否则按 AGF 的 LZSS 解压。
- `meta` 之后是 12 字节 body 头：

```text
[0]  unknown
[4]  bodyUnpackedSize
[8]  bodyPackedSize
```

- body 数据同样：相等即原样，否则 LZSS 解压。

### AGF 的 LZSS

AGF 的 meta / body 与 ALF 的目录区用**同一个**解压例程、**同一组参数**（窗口 4096、写位置从 4078 开始、最短 3、最长 18、flag 低位先、位置 12 位在前）；参数只写在「ALF 归档」的「ALF 的 LZSS」，本节不重复。

★ 与 ALF **不同**的只是容器：AGF 的 meta 直接解到图像对象里（不校验长度），body 解到按 body 头长度分配的缓冲；ALF 的压缩块在 268 字节头之后。⇒ "参数相同"不等于"装载路径可以互借"。

### 可选 ACIF / alpha

body 之后可能跟一段 `ACIF`：

```text
"ACIF" + 24 字节前缀 + u32 alphaUnpackedSize + u32 alphaPackedSize + alpha 数据
```

- `ACIF` 在 body 结束后 2048 字节窗口内查找；找不到就没有 alpha。
- alpha 数据同样按"相等即原样，否则 LZSS"处理。
- **引擎不校验 alpha 的字节数**：ACIF 头只校验宽高与图像宽高相等；使用端按"宽 × 高"逐行拷贝 ⇒ alpha 偏短是越界读（未定义行为）。实现应把它记为诊断，不得静默补 0。

### 像素排布

meta 里取宽高与 bpp（实现口径）：

- `width = u32 @ meta+20`，`height = u32 @ meta+24`，`bpp = i16 @ meta+30`；
- 若宽或高为 0，退回 `u32 @ meta+0` / `u32 @ meta+4`。

像素：

- 8bpp：调色板索引；调色板是一个固定 1024 字节的槽，条目数来自 meta 的一个字段（为 0 时按 256 项），每项 4 字节；
- 24bpp：BGR 三字节；
- 32bpp：BGRA 四字节（alpha 可被 ACIF 覆盖）；
- 行按 **4 字节对齐**：8bpp 行字节 `(w+3)&~3`，24bpp `(w*3+3)&~3`，32bpp `w*4`；
- 盘上是 **bottom-up**：解码到 top-down 时要反转行序；通道序为 BGR(A)。

### 核验状态（部分待核验）

| 部分 | 状态 | 待核验什么 |
|---|---|---|
| `ACGF` 头 / 版本 / 段长度 | **部分** | 24 字节头还没逐字段对上读取点 |
| meta / body 的 LZSS | **已核验** | 与 ALF 同一例程（参数见「ALF 归档」） |
| `ACIF` alpha | **部分** | 压缩 / 未压缩的选择路径还没逐条对上 |
| 宽高 / bpp / 调色板 / 行序 | **部分** | 像素读取与 blit 路径未逐条核验 |
| "压得小才压"规则 | **部分** | 重打包不要求逐字节相同 |

**已建立关联**：AGF 读取路径的入口与错误串已定位；**没有**把每个字段与具体读取指令逐条对上，所以整章不标"已核验"。

### 与 ALF / ASM 的边界

- ALF 是归档容器，负责"文件从哪里取"；AGF 是图像容器，负责"像素怎样排"；ASM 是脚本协议，负责"指令怎样编码"。三者互不替代。
