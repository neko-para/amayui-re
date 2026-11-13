# 字体策略：Amayui CN（CP932 码位映射字体）

> 本文只写**不变的口径**：字体是什么、映射表是什么、渲染链怎么走、cmap 怎么查、坑在哪、改哪里。
> **状态 / 进度 / 计数**看 `pnpm tools requirements plan`；**字体构建的自描述**是
> `pnpm tools fonts describe`；**字典与 patch 的字段口径**分别见 `data/translations/subs-cn-jp.md`
> 与 `pnpm tools patch describe`。**Win32 / GDI 合同**见 `contracts-win32.md` 的 `L1-GDI-*` /
> `L1-CP-*`；宿主偏差见 `apps/emulator-lift/SCOPE.md` §5。
> ★ `dist/**` 是 `.gitignore` 的**生成视图**，不是真源：字体的真源是「基底 TTF + 字典」，文本视图的
> 真源是「基线 BIN + patch」。引用生成物时必须同时给出生成它的命令。
>
> ★ 本文命令默认在**仓库根**执行；需要 `apps/emulator-lift` 的命令会显式 `cd`。

## 速查

| 想知道 | 去哪 |
|---|---|
| 文本用什么编码、为什么不能转 UTF-8/GBK | §1 |
| `subs-cn-jp.json` 的一条映射 / 谁在读它 | §2 |
| guest 字节怎么走到 glyph / 我们的 shim 在哪 | §3 |
| 某个码位在字体里画什么 / 有没有字形 | §4 |
| 常见的错法 | §5 |
| 换字体 / 改字典 / 改实现要动哪里、跑什么判据 | §6 |
| 相关需求单与仍待核的点 | §7 |

## 1. 一句话：码位是日文，字形是中文

**Amayui CN 是一份按 CP932 码位排布的映射字体**：文本里存的仍是**日文码位**，字体把其中一部分
码位的字形换成**简体中文**的字形。它不是编码转换器，也不改变文本的字节语义。

- **文本始终是 CP932 编码**。guest 的 BIN 文本、A 版 API 的 `lpString` 字节串、
  `GetGlyphOutlineA` 的 `uChar`（`(lead << 8) | trail` 的打包双字节）都是 CP932 字节。
  ⛔ **不把 guest 文本转成 UTF-8 / GBK 再落盘**，也不把 `uChar` 当 Unicode 码点。
- 唯一允许的 UTF-8 → CP932 边界在**构建期**：`patch.json` 是 UTF-8 JSON、存中文；写 BIN 时由
  `tools/lib/cn-jp.mjs` 的 `toBin()` 逐字决定「写哪个写法」，再由 CP932 编码表编成字节。
  运行期不再做这件事。
- **两层分工**（这是最容易混淆的地方）：

```text
文本层：中文（UTF-8） ── toBin() ──> 「日文写法」码位 ── cp932Encode ──> CP932 字节（BIN / guest）
字形层：CP932 字节 ── cp932Decode ──> 码位 ── 字体 cmap ──> glyph（简体字形）
```

`subs-cn-jp.json` 只解决**文本层写哪个码位**；字体 cmap 只解决**那个码位画谁的字形**。
两者都不做「字节编码转换」——那是 `apps/emulator-lift/src/runtime/cp932.{h,cpp}` 的事。

字体的三个落点：

| 件 | 作用 | 谁读 |
|---|---|---|
| `corpus/assets/fonts/Amayui-CN_cnjp.ttf` / `-Bold.ttf` | 随包发的**分发字体**；也是字体构建的校验基准 | 模拟器（`LIFT_HOST_FONT_PATH`）、`pnpm tools fonts verify` |
| `data/translations/subs-cn-jp.json` | 「简体 → 日文写法」字典；**只读输入** | 字体构建、patch 构建 / 提取 |
| `corpus/assets/fonts/SarasaGothicSC/*.ttf` | 上游**基底**（7z 解压产物、gitignore） | `pnpm tools fonts build` |

字体是 LFS 入库件；fresh clone 先 `git lfs pull`。基底不在场时构建 / 校验会明确报错，不会静默成功。

## 2. 映射表 `data/translations/subs-cn-jp.json`

### 2.1 它是什么

一张**扁平 JSON 对象**：键 = 简体中文，值 = 一个**能编进 CP932 的日文写法**。它解决的是
「这个简体字在 BIN 里该写成哪个码位」，**不是**「这个码位该画什么字形」，也**不是**编码器。
模拟器运行期**不读**这份 JSON：它只在构建期被字体与 patch 消费，运行期只读 TTF（cmap + 字形表）。

值看起来像异体字（例如 `你 → 凜`）是**码位选择的后果**，不是错字：`你` 编不进 CP932，
所以文本里先写 `凜`；字体再把 `凜` 的码位指向 `你` 的字形。字段 / 不变量 / 怎么改以
`data/translations/subs-cn-jp.md` 为准，本文不复述那里的条目规则。

### 2.2 谁生成、谁消费

- **生成**：本仓没有它的生成器。它是从旧仓 `res/subs_cn_jp.json` **逐字节迁入**的只读真源
  （来源与去向登记在 `corpus/assets.json` 的 `translation/subs-cn-jp`）。要改就是显式编辑这份
  JSON，不是跑某个 `--write` 生成它。
- **消费方 A（字体）**：`tools/lib/fonts.mjs` 的 `buildCnjp()` / `remapCmap()`。
  它把字典**反转**（日写法 → 简体），对字体 cmap 做 `cmap[日写法] = cmap[简体]`（原地改，
  允许链式；跳过平台 1 / Mac 的 cmap）。这正是「凜 的码位画 你 的字形」的来源。
  字体构建只重写 cmap（另加 `name` / `OS/2` 的族名与码页声明），`glyf` / `GPOS` / `hmtx`
  等表逐字节搬运——所以它换的是**码位到字形的指向**，不是字形本身。细节见
  `pnpm tools fonts describe`。
- **消费方 B（patch）**：`tools/lib/cn-jp.mjs` 的 `makeMapper()`，经 `tools/lib/patch.mjs`
  用于 `toBin()` / `toCn()`。规则是 `canEncodeCp932(ch) ? ch : (dict[ch] ?? '　')` ——
  **能编就原样，编不进去才查字典，字典也缺就记问题并用全角空格兜底**；顺序不能反。
  patch 清单里的 `subsSha` 就是这份字典的指纹（schema 见 `pnpm tools patch describe`）。

### 2.3 怎么查一条映射

```bash
node --input-type=module -e "import fs from 'node:fs'; import {canEncodeCp932} from './packages/age-format/src/asm/codec.mts'; const d=JSON.parse(fs.readFileSync('data/translations/subs-cn-jp.json','utf8')); for (const ch of ['你','这','說','凜']) console.log(ch, 'cp932='+canEncodeCp932(ch), 'dict='+(d[ch] ?? '—'));"
```

读数（只读查询，不是状态）：

```text
你 cp932=false dict=凜
这 cp932=false dict=這
說 cp932=false dict=—
凜 cp932=true dict=—
```

读法：`你` 编不进 CP932，所以写 `凜`；`凜` 本身编得进，字典里没有它。
`這` / `説` 这类能编进 CP932 的写法由 `toBin()` 原样保留，不查字典。

字典本身没有 `describe` 可指（它没有控制脚本）；字体侧的自描述是 `pnpm tools fonts describe`，
patch 侧是 `pnpm tools patch describe`。**不要**在文档里另写一份字段表。

## 3. 渲染链与接管点

### 3.1 一条链

```text
guest 字节（CP932）
  ├─ GetTextExtentPoint32A / TextOutA：readAnsiText 按 cch 读字节
  └─ GetGlyphOutlineA：uChar = (lead << 8) | trail
       ↓
  gdiDecodeCp932() / gdiCp932Char()            apps/emulator-lift/src/runtime/font.cpp:627 / :646
       ↓  cp932ToWide() / cp932Decode()        apps/emulator-lift/src/runtime/cp932.cpp:83 / :47
       ↓  UTF-16 码元 → char32_t               （表：apps/emulator-lift/src/runtime/cp932_table.h）
       ↓
  gdiGlyph() → FontEngine::glyph()             apps/emulator-lift/src/runtime/font.cpp:379
       ↓  FreeType face（faceForSize）          apps/emulator-lift/src/runtime/font.cpp:282
       ↓  FT_Load_Char → GlyphMask              （coverage / origin / advance）
       ↓
  GGO 打包 gdiPackGlyph() / 宽 gdiTextExtent() / 画 gdiDrawText()
```

- **唯一取字 / 推进路径**是 `gdiGlyph()`：`GetGlyphOutlineA` 的 `gmCellIncX`、
  `GetTextExtentPoint32A` 的宽、`TextOutA` 的推进都从同一个 `mask.advance` 派生
  （`font.cpp:688` / `:803`）。⛔ 不要为了某个 API 另算一遍。
- GGO 的格式算术（`GGO_BITMAP` 1bpp、`GGO_GRAY4` 17 级、`GGO_GRAY8` 65 级、行 DWORD 对齐、
  `cbBuffer == 0` 返回所需字节数）是**合同**，在 `contracts-win32.md` 的 `L1-GDI-02` / `L1-GDI-03`；
  本文不复制这些公式。`TextOutA` 用 FT 的**连续覆盖率**混合，不等同于 GGO 的 1 位量化
  （平台偏差，见 `apps/emulator-lift/SCOPE.md` §5）。
- 度量来自 FreeType 的 `face->size->metrics`，缓存键是 `(clamp 后像素字号, bold, 字体路径)`，
  `weight / italic / underlined / struck_out` 不进缓存、只由 `gdiTextMetrics` 从 `LOGFONT`
  覆盖（`font.cpp:492` 起）。`--profile` 会打印 `font!TEXTMETRIC-cache hits=… misses=…`。
- **基线补偿是宿主视觉策略，不是引擎语义**：`kBaselineLift = 2` 只在
  `FontEngine::glyph()` 里加到 `gmptGlyphOrigin.y`（`font.cpp:430`），**不改度量**；
  `--baseline-lift 0` 关掉，`--baseline-lift N` 改 N。⛔ 不要写进 `tmAscent`，也不要
  在 `gdiDrawText` 里再减一次（否则抬 2N px）。理由与回退口径见 `SCOPE.md` §5.1。

### 3.2 谁接管了 Win32 形状

| 文件 | 接管什么 |
|---|---|
| `apps/emulator-lift/src/shims/gdi32_font.cpp` | `CreateFontIndirectA` / `GetTextMetricsA` / `GetGlyphOutlineA` / `GetTextExtentPoint32A` / `TextOutA` / `EnumFontFamiliesExA` / `AddFontMemResourceEx` |
| `apps/emulator-lift/src/runtime/font.cpp` / `font.h` | FreeType 桥、字形 / 度量、GGO 打包、文本绘制、`FontConfig` / `kBaselineLift` |
| `apps/emulator-lift/src/runtime/cp932.cpp` / `cp932_table.h` | CP932 ⇄ UTF-16；表是派生文件，⛔ 不手改（生成器 `tools/lift-cp932-gen.mjs`） |
| `apps/emulator-lift/src/main.cpp` | `--font` / `--baseline-lift` / `--font-engine-name`（`:549` / `:557` / `:563`） |
| `apps/emulator-lift/CMakeLists.txt` | `LIFT_HOST_FONT_PATH` 的缺省注入（`:204`） |

字体来源的硬口径（用户裁决，登记在 `apps/emulator-lift/SCOPE.md` §5）：

- **主字体**一律是宿主 Amayui CN（`--font` 只是调试覆盖）。`CreateFontIndirectA` 的 `lfFaceName` 只是标签，
  ⛔ 不按族名路由；`--font-engine-name` 只改变 `EnumFontFamiliesExA` 上报的族名，**不切换字形来源**。
- **缺字后备**：`AddFontMemResourceEx` 送进来的内存**不再丢弃**——字节先**拷贝**，再用 `FT_New_Memory_Face`
  建 face，按**登记顺序**登记成后备（游戏自带的 `AGE Extend` 就是这样来的，字体在 `SYS4INI.BIN` 的 ALF 里）。
  主 face 有该码位时**永不**使用后备；主 face 缺时才按顺序试。⛔ 后备**不是**换主字体、也**不是**按族名选字体。
- **判据是 `FT_Get_Char_Index(face, cp) != 0`**：`FT_Load_Char` 对缺 cmap 的码位**不失败**（会画 `.notdef`，见 §4.3）。
  主 face 与**全部**后备都缺 ⇒ 诚实画 `.notdef`（⛔ 不是"没有字体"的失败；首次 warn 一条），**记账**在 `--profile` 摘要的
  `font!glyph-fallback` 行（字段口径见 `apps/emulator-lift/README.md` §4.3.1；逐次来路用 `LIFT_TEXT_TRACE` 看，§3.4）。
- 没有可用字体时，`GetTextMetricsA` / `GetGlyphOutlineA` / `GetTextExtentPoint32A` /
  `TextOutA` **明确失败**（0 / `GDI_ERROR` + 一行日志），⛔ 不画空白冒充成功。
- 实际生效的路径看启动日志的 `font :` 行，或 `./out/emulator-lift --help` 的
  `--font` / `--baseline-lift` 说明；不要只看 CMake。

### 3.3 字体名表不是字形来源

`EnumFontFamiliesExA` 是引擎**字体名表**的填充点（`gdi32_font.cpp:464`）：它按固定名单逐条
回调（第一条是宿主 `Amayui CN`），但每条都落到同一份 Amayui CN 字形。两个容易写错的点：

- `lfCharSet` 必须**回显请求的 charset**（引擎请求 `SHIFTJIS_CHARSET = 0x80`）。回 0 会让
  引擎的 `Proc` 把每条都过滤掉 ⇒ 字体名表为空 ⇒ 运行期查 `ＭＳ ゴシック` 返回 `-1` ⇒
  `std::out_of_range`。`GetTextMetricsA` 里的 `tmCharSet` 恒 0 是另一条路径，别把它当
  枚举请求值（`gdi32_font.cpp:96` vs `:503`）。
- 回调极性是 **非 0 = 继续，0 = 停止**（MSDN `EnumFontFamExProc`；引擎的 `Proc` 所有路径都
  返回 1）。写成「非 0 = 停止」会让枚举在第 1 条被自己掐断，崩溃照旧。
  代表单：`REQ-01M72G8JY1Y989H9H8M02DJ2PX`。

### 3.4 诊断开关

- `LIFT_TEXT_TRACE=1`：`textTraceEnabled()` 用 `getenv(...) != nullptr` 判断，
  **只要变量存在就开**（值是什么都行，写 `=0` 也算开）。输出前缀 `[text-trace]`；取字链会打印
  `cp=U+%04X … charmap[%d] platformID=%u encodingID=%u glyphID=%u`，A 版文本 API 会打印
  前 32 字节 hex 与 CP932 逐字节走法（`consumed=1/2`）。默认零输出；它只是诊断层，
  ⛔ 不改返回值 / 推进 / 缓存，取证结束后可整段删。
  要真跑需要一份安装树；开关与格式的落点见
  `apps/emulator-lift/src/runtime/font.cpp:33` 与 `apps/emulator-lift/src/shims/gdi32_font.cpp:337`。
- `--profile`：`font!TEXTMETRIC-cache` 行回答「度量缓存有没有用」，不是墙钟；
  口径见 `apps/emulator-lift/README.md` §4.3.1。

## 4. cmap 事实与自查

### 4.1 不依赖 fontTools 的自查（本仓脚本）

```bash
node --input-type=module -e "import fs from 'node:fs'; import {parseSfnt,readCmap} from './tools/lib/fonts.mjs'; const b=fs.readFileSync('corpus/assets/fonts/Amayui-CN_cnjp.ttf'); const f=parseSfnt(b); for (const s of readCmap(b,f.byTag.get('cmap'))) { const at=(cp)=>s.cmap.get(cp)??0; console.log(s.platformID+'/'+s.platEncID+' f'+s.format+' 你='+at(0x4F60)+' 凜='+at(0x51DC)+' U+E010='+at(0xE010)+' ♡='+at(0x2661)); }"
```

读数（macOS arm64，Node 24，2026-11-13；Regular 与 Bold 在这批码位上相同）：

```text
0/3 f4 你=25760 凜=25760 U+E010=0 ♡=1815
0/4 f12 你=25760 凜=25760 U+E010=0 ♡=1815
0/5 f14 你=0 凜=0 U+E010=0 ♡=0
3/1 f4 你=25760 凜=25760 U+E010=0 ♡=1815
3/10 f12 你=25760 凜=25760 U+E010=0 ♡=1815
```

读法：

- `0/3`、`0/4`、`3/1`、`3/10` 是 Unicode cmap；`0/5` 的 f14 是**变体序列**子表，
  `readCmap` 不解析它，输出全 0 是正常的，别据此判断缺字。
- FreeType 在本机默认选中 `charmap[4] = 3/10 f12`（`face->charmap`；见 4.3 的探针）。
  这批 BMP 码位在所有 Unicode 子表里一致，所以选哪张不影响读数。
- 字体里**没有 Shift-JIS（platform 3 / encoding 2）cmap**：它只认 Unicode 码位。
  所以「CP932 字节 → Unicode」必须先由 `apps/emulator-lift/src/runtime/cp932.cpp` 完成，
  不能把 CP932 字节直接丢给 FreeType。
- **`你` 与 `凜` 是同一个 glyph（25760）**：这就是「文本写 `凜`、画出来是 `你`」的直接证据。
  字典 `你 → 凜` + 字体 `cmap[凜] = cmap[你]`，两半合起来才是 cnjp。
- `U+E010` = 0 表示没有映射（glyph 0 = `.notdef`）；`♡`（U+2661）= 1815 表示真正的
  空心心形在另一个码位。**码位存在 ≠ 字形存在**，见 4.3。

### 4.2 CP932 用户定义区 → PUA：U+E000 区

CP932 的用户定义区字节对 `0xF040–0xF9FC` 解出来是 Unicode 私用区 `U+E000–U+E757`：

```bash
node --input-type=module -e "import {cp932Pair} from './tools/lib/cp932.mjs'; for (const [a,b] of [[0xF0,0x40],[0xF0,0x50],[0xF9,0xFC]]) { const s=cp932Pair(a,b); console.log(a.toString(16).toUpperCase()+' '+b.toString(16).toUpperCase()+' => U+'+s.codePointAt(0).toString(16).toUpperCase()); }"
```

```text
F0 40 => U+E000
F0 50 => U+E010
F9 FC => U+E757
```

Amayui CN **整段没有 PUA 映射**（Regular / Bold 都是）：

```bash
node --input-type=module -e "import fs from 'node:fs'; import {parseSfnt,readCmap} from './tools/lib/fonts.mjs'; for (const p of ['corpus/assets/fonts/Amayui-CN_cnjp.ttf','corpus/assets/fonts/Amayui-CN_cnjp-Bold.ttf']) { const b=fs.readFileSync(p); const f=parseSfnt(b); const subs=readCmap(b,f.byTag.get('cmap')); const hit=[]; for (let cp=0xE000; cp<=0xE757; cp++) if (subs.some(s=>s.cmap.has(cp))) hit.push(cp); console.log(p, 'PUA mapped:', hit.length ? hit.map(c=>'U+'+c.toString(16).toUpperCase()).join(',') : '(none)'); }"
```

```text
corpus/assets/fonts/Amayui-CN_cnjp.ttf PUA mapped: (none)
corpus/assets/fonts/Amayui-CN_cnjp-Bold.ttf PUA mapped: (none)
```

所以：任何走 `0xF040–0xF9FC` 的文本都会落到 `U+E000–U+E757`。Amayui CN **本身**没有这些码位的
字形；登记了游戏自带字体作缺字后备后，这些码位可能由后备提供（§3.2），主字体与后备都没有才取到
glyph 0 / `.notdef`。**不要**为了让某个字显示出来去改 `cp932_table.h` 把 `0xF050` 解成 `U+2661`：
那会改变字节语义，并破坏「三平台 CP932 表逐字节一致」的口径
（`docs/03-emulator/encoding-backend.md`）。真机走的是不是这条后备路径仍是待核问题，见 §7 的 bug 单。

### 4.3 「码位存在 ≠ 字形存在」：FreeType 的 `.notdef`

`FT_Load_Char` **不会**因为 cmap 没有该码位而失败：`FT_Get_Char_Index` 返回 0，
`FT_Load_Char` 仍然返回 0，并加载 glyph 0（`.notdef`）。用下面这段最小探针复核
（macOS / Linux；Windows 用 vcpkg 的 FreeType 同样可编）：

```c
// .tmp/ftprobe.c
#include <stdio.h>
#include <ft2build.h>
#include FT_FREETYPE_H
int main(int argc, char** argv) {
    FT_Library lib; FT_Face face;
    if (FT_Init_FreeType(&lib) || FT_New_Face(lib, argv[1], 0, &face)) return 2;
    int sel = -1;
    for (int i = 0; i < face->num_charmaps; ++i) if (face->charmaps[i] == face->charmap) sel = i;
    printf("selected charmap[%d] platformID=%u encodingID=%u\n", sel,
           (unsigned)face->charmap->platform_id, (unsigned)face->charmap->encoding_id);
    FT_Set_Pixel_Sizes(face, 0, 20);
    unsigned cps[] = {0x4F60, 0x51DC, 0xE010, 0x2661};
    for (int i = 0; i < 4; ++i) {
        unsigned cp = cps[i];
        FT_UInt gid = FT_Get_Char_Index(face, cp);
        FT_Error err = FT_Load_Char(face, cp, FT_LOAD_RENDER);
        printf("cp=U+%04X glyphID=%u load_err=%d bitmap=%ux%u advance26.6=%ld\n",
               cp, gid, (int)err, (unsigned)face->glyph->bitmap.width,
               (unsigned)face->glyph->bitmap.rows, (long)face->glyph->advance.x);
    }
    return 0;
}
```

```bash
cc -o .tmp/ftprobe .tmp/ftprobe.c $(pkg-config --cflags --libs freetype2)
.tmp/ftprobe corpus/assets/fonts/Amayui-CN_cnjp.ttf
```

读数（macOS arm64，系统 FreeType（pkg-config），2026-11-13）：

```text
selected charmap[4] platformID=3 encodingID=10
cp=U+4F60 glyphID=25760 load_err=0 bitmap=20x20 advance26.6=1280
cp=U+51DC glyphID=25760 load_err=0 bitmap=20x20 advance26.6=1280
cp=U+E010 glyphID=0 load_err=0 bitmap=7x22 advance26.6=832
cp=U+2661 glyphID=1815 load_err=0 bitmap=18x16 advance26.6=1280
```

**结论**：判「字形存在」只能看 `FT_Get_Char_Index() != 0`（或先看 `LIFT_TEXT_TRACE` 的 `glyphID`），
`FT_Load_Char` 的返回值不够 —— `U+E010` 这种「cmap 没条目」的码位会被它当成加载成功，给出 `.notdef` 位图。
现行 `FontEngine::glyph()` 就是按这个判据逐 face 查（主 face → 后备，§3.2）；早先只看
`FT_Load_Char` 返回值，那是本单修掉的错法。代表单：`REQ-01M79N1ZV391BCM7MYSKK0GVV0`。

## 5. 常见坑

| 坑 | 错了会怎样 | 去哪查 / 谁守 |
|---|---|---|
| 把 guest 文本当 UTF-8（对 A 版 `lpString` 先 UTF-8 解码，或把 `uChar` 当 Unicode 码点） | CP932 双字节被拆成两个码位 / 高位字节丢失 ⇒ 乱码或错字 | `cp932.h`、`font.cpp:627` / `:646`、`gdi32_font.cpp:206`；`gdi-font / extent-advance` |
| 把 `subs-cn-jp.json` 当编码器（直接 `dict[ch]` 写字节） | 只换了码位、没编码；或把本来能编的字也替换 ⇒ BIN 与 patch 不一致 | `tools/lib/cn-jp.mjs` 的 `toBin()`；`pnpm tools patch verify` |
| 把字典当单射（同一个日文写法被多个简体共用） | 反查 / 反转只保留**第一个**；`toCn` 可能反查不回来，`remapCmap` 也只写一条 | `tools/lib/cn-jp.mjs` 的 `inverse`、`tools/lib/fonts.mjs` 的 `remapCmap`；`tools/test/patch.test.mjs` 的 mapper 用例 |
| 往字典里塞不能编 CP932 / 基底没有对应字形的写法 | 写 BIN 时可能落成 `?`；字体侧 `remapCmap` 跳过 ⇒ 缺字或错字 | `pnpm tools fonts verify`；`pnpm tools patch verify` |
| 用系统字体替代 Amayui CN（或以为 `lfFaceName` 会选系统字） | 简体字形丢失 / 变回日文字形；字体名表也救不了 | `font.cpp:282`；`SCOPE.md` §5 |
| 粗体用合成（embolden）而不是 `-Bold.ttf` | 笔画 / 度量与契约不同；常规与粗体两条 face 路径失效 | `font.cpp:74` / `:282`；`gdi-font / metrics-stable` |
| 只比 `GetTextExtentPoint32A` 的宽，不与 GGO / `TextOutA` 交叉核对 | CJK 等宽下 extent 对但字形可能全错；advance 的 floor/round 也会漂 | `font.cpp:688` / `:803`；`gdi.cpp / extent-advance-self-consistency` |
| 把 PUA（`U+E000` 区）当普通字 | CP932 用户定义区 → PUA；Amayui CN 无 cmap ⇒ 交给缺字后备（若已登记游戏字体），主字体与后备都缺才画 `.notdef`，且 `FT_Load_Char` 不报错 | `cp932_table.h`；bug `REQ-01M79N1ZV391BCM7MYSKK0GVV0` |
| 只看 `FT_Load_Char` 返回值判断「字形存在」 | glyph 0 / `.notdef` 也返回 0；缺字被缓存成正常字形 | `font.cpp` 的 `glyphIndexFor()` / `glyph()`；同上 bug 单 |
| 把基线补偿写进 `tmAscent`，或在 `gdiDrawText` 里再减一次 | 行距变 / 墨迹抬 2N px；`TEXTMETRICA` 不再逐字节稳定 | `font.h:35`、`font.cpp:430`；`text-baseline-lift / lift/metrics-unchanged` |
| 把 `tmCharSet` 恒 0 当成「引擎请求的 charset」 | 枚举路径若回 0，引擎回调过滤掉所有族名 ⇒ 字体名表空 ⇒ `std::out_of_range` | `gdi32_font.cpp:96` vs `:503`；`gdi.cpp / enum-font-families` |
| `EnumFontFamiliesExA` 回调极性写成「非 0 = 停止」 | 只报 1 条族名；引擎查 `ＭＳ ゴシック` 返回 -1 ⇒ 崩 | `gdi32_font.cpp:464`；`gdi.cpp / enum-font-families-stop` |
| 直接手改 / 覆盖 `corpus/assets/fonts/Amayui-CN_cnjp*.ttf` | 校验基准被破坏；`pnpm tools fonts verify` 红；无法从「基底 + 字典」复现 | `tools/fonts.md`；`tools/test/fonts.test.mjs` |
| 换 `--font` / 改配置后不失效缓存 | 旧字形 / 度量继续命中；字面与字号串 | `font.cpp:182` / `:220`；`gdi-font-metrics-cache / cache/config-miss` |
| 想让 `AddFontMemResourceEx` 的字体当主字体（或按 `lfFaceName` 路由） | 内存字体**只当缺字后备**：主 face 有该码位时永不使用；按名字路由会把全部中文切回日文字形 | `gdi32_font.cpp:606`、`runtime/font.h`；`SCOPE.md` §5 |
| 缓存键去掉 bold / 字体路径 | 常规与粗体、或两次 `--font` 之间串度量 | `font.cpp:512`；`gdi-font-metrics-cache / cache/bold-not-aliased` |
| 绕过 `gdiGlyph` 自己 `FT_Load_Char` | GGO / extent / `TextOutA` 不再自洽（三套推进） | `font.cpp:379`；`gdi.cpp / extent-advance-self-consistency` |

## 6. 改字体 / 换字体

### 6.1 只做调试：换路径 / 关补偿 / A/B 上报

```bash
cd apps/emulator-lift
./out/emulator-lift --help | rg -- '--font|--baseline-lift|--font-engine-name'
```

```text
  --font <path>       宿主字体（缺省：CMake 注入的 Amayui CN；调试覆盖）
  --baseline-lift <N> 宿主基线补偿像素（缺省 2；0 = 关；★ 宿主视觉策略，不是引擎语义）
  --font-engine-name  字体 A/B 对照档：上报引擎字面名（字形仍来自宿主 Amayui CN）
```

- `--font <ttf>` 只覆盖 `regular_path`，并清空 `bold_path`；`fontSetConfig()` 会用
  `boldPathFor()` 派生 `-Bold.ttf`（只认 `.ttf` 后缀；其它后缀会回退同一份，`font.cpp:74`）。
- `--baseline-lift 0` 关补偿；`N` 改 N（0..64）。
- `--font-engine-name` **只影响上报族名**，不换字形。真正换字形要改分发字体或实现，
  不要指望这个开关。

### 6.2 换分发字体（产品决策，不是调试）

Amayui CN 的来源链是「Sarasa Gothic SC 基底 + 字典」。**不要**直接把某个 TTF 覆盖到
`corpus/assets/fonts/` 就算换字体——那会同时破坏校验基准与「基底 + 字典可复现」这条口径。

要改就按链改：

1. 换基底：替换 `corpus/assets/fonts/SarasaGothicSC/*.ttf`（7z 解压产物、gitignore）或改
   `tools/lib/fonts.mjs` 的 `DEFAULT_BASE` / `DEFAULT_BOLD_BASE`。
2. 改字典：编辑 `data/translations/subs-cn-jp.json`（或 `--dict` 指另一份）。字典变了，
   patch 的 `subsSha` 也会变，必须重跑 patch 构建，⛔ 不手改 `subsSha`。
3. 构建 / 校验：

```bash
pnpm tools fonts describe        # 先读自描述：三步变换 / 判据 / 序列化口径
pnpm tools fonts verify          # ★ 与 corpus 里的可信产物比（归一化 head.modified 后逐字节）
pnpm tools fonts verify --bold
pnpm tools fonts build --write   # 只写 dist/fonts/（生成物）；不覆盖 corpus
```

读数（2026-11-13，基底在场；Regular / Bold 各跑一次）：
`✔ 归一化 head.modified 后逐字节相同`。这条判据只证明「构建结果 = 可信产物」，
不证明「字形一定对」——字形对不对要靠 §4 的 cmap 自查与真机对照。

4. 确认新字体后，**显式再烘**到 `corpus/assets/fonts/Amayui-CN_cnjp{,-Bold}.ttf`；这是
   一次有意的基准替换，不是 `build` 的副作用。之后用 `pnpm tools corpus scan --write`
   更新清单里的指纹（⛔ 不手改 `corpus/assets.json` 的 sha / 大小）。
5. 判据 / 守卫：
   - `pnpm tools fonts verify`（+ `--bold`）：字体构建链的判据；
   - `tools/test/fonts.test.mjs`：同一判据的纯函数守卫（基底 / 基准不在场时按惯例 skip）；
   - `pnpm verify` 跑工具链档（含上一条）。
6. 如果换的不是「Sarasa + 字典」这条链，`tools/fonts` 就不再适用；那是一次产品层改动，
   要同时改 `SCOPE.md` §5（用户裁决的宿主策略）与相关需求单，不能只改路径。

### 6.3 改实现（字形 / 度量 / 文本）

| 想改 | 文件 | 跑什么 |
|---|---|---|
| 取字 / 度量 / GGO / 绘制 | `apps/emulator-lift/src/runtime/font.cpp` | 见本节末的命令 |
| Win32 形状 / 参数解包 | `apps/emulator-lift/src/shims/gdi32_font.cpp` | 同上（`gdi` 是 pure；`gdi-font` / `text-baseline-lift` 是 assets，缺字体时 SKIP 77） |
| CP932 表 | `apps/emulator-lift/src/runtime/cp932_table.h` + 生成器 `tools/lift-cp932-gen.mjs` | 按生成器头注释重生成（⛔ 不手改派生表）；CTest `codepage` / `strfmt` |
| 字体路径 / 基线常量 | `apps/emulator-lift/CMakeLists.txt:204`、`font.h:35` | 重新 configure + build；`text-baseline-lift` |

守卫用例名（**CTest 测试名** + 测试内部的用例名；`-R` 只能匹配前者，内部用例名要跑整个
CTest 才会打印）：

- CTest `gdi`（pure）：`enum-font-families`、`enum-font-families-stop`、
  `extent-advance-self-consistency`、`pack-mono-row-align` / `pack-gray4-range` / `pack-gray8-range`；
- CTest `gdi-font`（assets）：`ggo-formats`、`extent-advance`、`metrics-stable`；
- CTest `gdi-font-metrics-cache`（pure）：`cache/size-change`、`cache/bold-not-aliased`、
  `cache/config-miss`、`cache/profile-line`；
- CTest `text-baseline-lift`（assets）：`lift/origin-delta`、`lift/metrics-unchanged`、
  `lift/pixels`。

```bash
ctest --test-dir apps/emulator-lift/out -R '^(gdi|gdi-font-metrics-cache|gdi-font|text-baseline-lift)$' --output-on-failure
```

## 7. 相关单与待核

相关需求单（只读；状态 / 进度看 `pnpm tools requirements plan`，不要从本文推断）：

```bash
pnpm tools requirements show 01M6KHDTBREGT6Z90E4EHPKE6M   # GDI 字体/文本：宿主 Amayui CN + 三档 GGO
pnpm tools requirements show 01M72G8JY1Y989H9H8M02DJ2PX   # 字体名表 / EnumFontFamiliesExA
pnpm tools requirements show 01M6WXEDGGJY1Q1YDEM07S1YR3   # 度量缓存 + 基线补偿
pnpm tools requirements show 01M79N1ZV391BCM7MYSKK0GVV0   # U+E010 / PUA 字形（bug）
pnpm tools requirements show 01M79N24Y09F5G9Y40QV3SVFDG   # 本文档的落点单
```

**待核**：

- `U+E010`（以及 `U+E000–U+E757` 用户定义区）在**真机**为什么能画出空心心形：
  字体走的是哪张 cmap / 是否有 legacy / symbol 映射 / 是否有 fallback / 引擎是不是用 A 版
  字节对——这些都还没有定论。本文只登记已复核的事实（Amayui CN 的 cmap 没有这些码位、
  FreeType 取到 glyph 0、`FT_Load_Char` 不报错），**不猜原因**。见 bug 单
  `REQ-01M79N1ZV391BCM7MYSKK0GVV0` 的 `<待核>`。
  ★ 现行 **lift** 已把游戏自带字体登记成缺字后备（§3.2），`AGE Extend` 的 cmap 有 `U+E010`；
  上面问的是**真机**是否也走这条后备，仍未定论。
- `MＳ 明朝` 的来路在该单里只有父级画面观察，exe / 安装树里没有该字面量；这不影响字形来源，
  但影响 `kEnumFamilyNames` 的名单解释。见 `REQ-01M72G8JY1Y989H9H8M02DJ2PX` 的「未做 / 边界」。
