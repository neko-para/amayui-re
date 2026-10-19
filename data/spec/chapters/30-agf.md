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

AGF 使用与 ALF **不同参数**的 LZSS：

- 窗口 `4096` 字节，窗口初始位置 `4078`；
- 最长匹配 `18` 字节，**最短匹配 3**；
- 每个 flag 字节管 8 个单位；匹配长度 = `(hi & 0x0F) + 3`，位置 = `lo | ((hi & 0xF0) << 4)`；
- 压缩器按"压得比原文小就存压缩结果，否则原样"选择每段形态。这是**可复现规则**，不是猜测。

★ 不能拿 ALF 的 LZSS 替代：两者参数不同，字节层不互逆。

### 可选 ACIF / alpha

body 之后可能跟一段 `ACIF`：

```text
"ACIF" + 24 字节前缀 + u32 alphaUnpackedSize + u32 alphaPackedSize + alpha 数据
```

- `ACIF` 在 body 结束后 2048 字节窗口内查找；找不到就没有 alpha。
- alpha 数据同样按"相等即原样，否则 LZSS"处理。
- alpha 的长度应等于 `width × height`；不等时实现不得把它当成完整 alpha 使用。

### 像素排布

meta 里取宽高与 bpp（实现口径）：

- `width = u32 @ meta+20`，`height = u32 @ meta+24`，`bpp = i16 @ meta+30`；
- 若宽或高为 0，退回 `u32 @ meta+0` / `u32 @ meta+4`。

像素：

- 8bpp：调色板索引；调色板从 `meta+56` 起，最多 256 项；若 meta 只有约 1024 字节且没有头，则取最后 1024 字节；
- 24bpp：BGR 三字节；
- 32bpp：BGRA 四字节（alpha 可被 ACIF 覆盖）；
- 行按 **4 字节对齐**：8bpp 行字节 `(w+3)&~3`，24bpp `(w*3+3)&~3`，32bpp `w*4`；
- 盘上是 **bottom-up**：解码到 top-down 时要反转行序；通道序为 BGR(A)。

### 核验状态（部分待核验）

| 部分 | 状态 | 依据（内部审计） | 待核验什么 |
|---|---|---|---|
| `ACGF` 头 / 版本 / 段长度 | **部分** | 引擎图像读取段的错误路径已定位（`AGFヘッダ...`） | 24 字节头逐字段对读取点 |
| meta / body 的 LZSS | **待核验** | 实现参数（最短 3 / 窗口 4078） | 引擎解压函数与参数 |
| `ACIF` alpha | **待核验** | 实现；语料有 ACIF 样本 | 引擎侧 alpha 读取/使用点 |
| 宽高 / bpp / 调色板 / 行序 | **待核验** | 实现 + 解码守卫 | 引擎像素读取与 blit 路径 |
| "压得小才压"规则 | **部分** | 实现 + 真实件往返 | 引擎是否同规则（重打包不要求） |

**已建立关联**：AGF 读取路径的入口/错误串已登记（内部台账）；**没有**把每个字段与具体读取指令逐条对上，所以整章不标"已核验"。

### 与 ALF / ASM 的边界

- ALF 是归档容器，负责"文件从哪里取"；AGF 是图像容器，负责"像素怎样排"；ASM 是脚本协议，负责"指令怎样编码"。三者互不替代。
