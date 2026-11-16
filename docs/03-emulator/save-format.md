# 存档目录：结构，以及"外部怎么组装出一个能加载的目录"

> **口径**：本文只写**引擎/脚本侧的机制**与**组装步骤**，不写剧情、不写进度。
> 每条结论后面跟 `〔EA …〕`（二进制锚）或 `〔脚本 …〕`（AGE 脚本位置，附复现方式）或 `<待核>`。
> 措辞纪律：下面所有"引擎会…"的意思是**与可获得的证据不矛盾、且可被独立复核**，⛔ 不是"与真机等价"。
>
> **锚点怎么复核**（全部只读、不构建）：
> ```bash
> pnpm tools disasm-fn show 0x437480 --lst        # 头写侧；--lst 逐字核验
> pnpm tools disasm-at at --ea 0x42DD49 --lines 40 # 只读头命令读哪几个偏移
> pnpm tools disasm-fn callers sub_408A40         # 存档目录解析器被谁用
> rg -n 'FindFirstFileA' corpus/disasm/files/*.lst # 引擎到底有没有列目录
> ```
> 脚本侧：`dist/views/data/SAVE.BIN.txt`（2418 行；生成物，⛔ 不是锚，只用来读位置）。
> 脚本里的 `iXXX` = **十六进制 opcode**（`packages/age-format/src/asm/opcodes.mts` 的 `'i' + opcode.toString(16).padStart(3,'0')`），
> 所以 `i19e` = 414 = `0x19E`，可以和下面的 handler EA 一一对上。

---

## 目录布局

### 1. 目录名从哪来

`sub_408A40(this, out, cap)` = **存档目录解析器**〔EA 0x408A40，被 11 处调用：槽读写 / 系统容器读写全用它〕：

```
root = (config("set:UseAppDataFolder") == 1) ? <SHGetFolderPath(CSIDL_LOCAL_APPDATA, …)>
                                             : ""（byte_51EA3C，一个 4 字节全 0 的"空串哨兵"）
out  = root
if (out != "") out += "\"            ; 只在拼到此处非空时补，不折叠
out += config("set:SavePath")
```

〔EA 0x408A40 体内：`set:UseAppDataFolder` 取键、`j_SHGetSpecialFolderPathA_0`/`SHGetFolderPath` 两支、
`byte_51EA3C`（`.data:0051EA3C` = `4 dup(0)`，即空串）、`asc_51D6DC` = `"\"`、`aSetSavepath` = `"set:SavePath"`〕

* ★ **判据是"恰为 1"**，不是"非 0"：`cmp eax, 1` / `jnz loc_408B5D`〔EA 0x408A71–0x408A74〕
  ⇒ `set:UseAppDataFolder = 2` 走的是**空根**那一支。
* ⇒ **`--appdata` 必须让宿主把"本地 AppData"报成那个根**；存档目录 = 该根 + `\` + `set:SavePath`。
  `set:UseAppDataFolder != 1` 时根是空串 ⇒ 目录变成**相对进程工作目录**的 `set:SavePath`（发行态即安装目录）。
* 老系统那一支取的是 `SHGetSpecialFolderPathA(0, pszPath, 26, 1)`（`26 = CSIDL_APPDATA`，漫游），
  新系统那一支取的是 `SHGetFolderPath` 的 `0x801C`（`28 = CSIDL_LOCAL_APPDATA`，本地）
  〔EA 0x408A40 体内：`j_SHGetSpecialFolderPathA_0(0, pszPath, 26, 1)` 与 `ProcAddress(0, 32796, …)`〕
  ⇒ "本地 vs 漫游"取决于 OS 版本分支，不是配置。
* 同一个函数末尾还做一次"目录按需创建"：拿 `<dir>\SAVE.DAT` 当探针 `CreateFileA(…, OPEN_EXISTING)`，
  打不开就 `MakeSureDirectoryPathExists(<dir>\\)`〔EA 0x408A40 尾段，`aSSaveDat` = `"%s\SAVE.DAT"`〕。
  ⇒ **目录不存在会被自动建出来**；这一步不建任何文件。
* 本机样本登记的真机路径是 `…\Eushully\天結いキャッスルマイスター\SAVE`（`corpus/assets.json` 的 `roots.gameSaves`），
  所以 `set:SavePath` 里大概就带着 `Eushully\天結いキャッスルマイスター\SAVE` 这几个子目录 ——
  它在**玩家 INI** 里，本文不追（见 `## 待核`）。

### 2. 引擎会在目录里碰哪些名字

| 名字 | 谁写 | 谁读 |
|---|---|---|
| `SAVE.DAT` | `0x40AB49` 起（先写 `$$SAVE.DAT` → `MoveFileA` 换名，旧件转 `SAVE.BAK`） | 启动期一次：`sub_40AEE0`（唯一调用方 = `_WinMain@16`）→ `sub_438940` |
| `RT.DAT` | 同一个写侧（`0x40AD5D` 写 `$$RT.DAT`、`0x40ADDA` 写 `RT.DAT`） | 同一个启动期读侧 |
| `SAVE%2.2d.DAT` | 槽写命令 | 槽读 / 只读头命令 |
| `SAVE%2.2d.STH` | 缩略图写命令 | 缩略图读命令 |
| `SAVE%2.2d.STH` | 缩略图写命令 | 缩略图读命令 |
| `SAVE.BAK` / `$$SAVE.DAT` / `$$RT.DAT` | 写侧的备份与临时名 | 启动期读侧在 `SAVE.DAT` 打不开时**退到 `SAVE.BAK`** |

〔格式串：`.data:005206D8 aSSave22dDat = '%s\SAVE%2.2d.DAT'`、`.data:005206EC aSSave22dSth = '%s\SAVE%2.2d.STH'`、
`.data:0051EA9C aSSaveDat`、`.data:0051EF68 aSSaveBak`、`.data:0051EF9C aSSaveDat_0`（`$$SAVE.DAT`）、
`.data:0051EF50 aSRtDat_0`、`.data:0051EF5C aSRtDat`（`$$RT.DAT`）〕

★ **引擎不列目录**：全语料里 `FindFirstFileA` 只有**一处**〔EA 0x455838，通配是 `"*.AAI"`，与存档无关〕。
⇒ 没有 `SAVE*.DAT` 扫描、没有"槽索引文件"、也没有第二个搜索目录。
**每个槽号都只由调用方给，一次只算出一个文件名。**

### 3. `SAVE.DAT` 与 `RT.DAT` 是什么

* 两者与槽文件**共用同一套 292 字节头 + 20 字节参数块 + 变换/压缩负载**的外壳
  〔读侧 `sub_438940`（唯一调用方 `0x40AEE0`）内部调 `sub_438120` 做 292 字节头校验；外壳结构见下节〕。
* 魔数不同：`SAVE.DAT` 起头是 `S4SD`，`RT.DAT` 起头是 `S3RT`
  〔字节：`xxd -l 16 corpus/fixtures/SAVE.DAT corpus/fixtures/RT.DAT`；族标识逐容器，不是全引擎一个值 ——
  读侧是 `strncmp("S4", <引擎自己的族标识>, 2)` 决定期望 `S4SD` 还是 `S3SD`，容器种类是常量〔EA 0x438120〕〕。
* **角色（按"引擎的哪个功能用它"判，不按名字猜）**：
  * `SAVE.DAT` = **脚本键的持久化池**：`save-int`/`load-int`/`save-string`/`load-string` 四个 opcode
    把"跨存档记住的脚本变量"放进一个键表，`SAVE.DAT` 就是这张表的落盘形态
    〔EA 0x434F60 = opcode 418 `save-int`；0x42DF40 = 419 `load-int`（`wsprintfA(v6,"%c%8.8x",3,idx)` +
    `sub_428E00(this+5452, v6)` 的哈希查找）；键表读写见 `0x40AAE0` / `0x40AEE0`〕。
    它**不是**槽索引 —— 里面没有任何"哪些槽存在"的清单。
  * `RT.DAT` = 另一份系统容器（同一外壳、族标识 `S3`）。**内容语义未取证**（样本的 `what` 说是文本/消息库，
    但那一句是清单里的人类注记，不构成锚）⇒ 本文只把它当"要一起带上的第二个容器"。
* **缺了会怎样**（能确定的）：
  * 缺 `SAVE.DAT`：目录解析器据此判定"目录不存在"→ 建目录〔EA 0x408A40 尾段〕；启动期读侧 **先退到 `SAVE.BAK`**，
    两者都打不开就走跳过支〔EA 0x40AEE0，体内 `while (!sub_454730(…)) { …Saves.BAK… }` + `LABEL_23`〕。
    ⇒ 不致命，表现为"脚本键全空"（例如"上次存到第几槽"回到 0）。
  * 缺 `RT.DAT`：走同一读侧，**具体后果未取证** ⇒ `<待核>`。
  * 缺 `SAVE%2.2d.DAT`：**静默** —— 只读头命令打开失败就返回 `1`〔EA 0x42DC70 的 `CreateFileA == -1` 支，
    直接 `sub_42B4B0(this,1,1)`〕，没有任何对话框。
    对话框只出现在**保存**路径且**文件存在但头不合法**时（"すでにセーブデータが存在しています…" + "セーブデータが不正です"，
    `MB_YESNO|MB_ICONWARNING`，选"否"就中止）〔EA 0x42DA39–0x42DA58〕。

---

## 槽号来源（含"改名是否成立"的裁决与 EA 锚）

### 裁决

> **槽号只来自文件名一侧：脚本给的整数操作数 → `%2.2d` → `SAVE<nn>.DAT` / `.STH`。
> 载荷头里没有槽号，也没有任何"期望槽号"会被拿来比对 ⇒ 改名成立。**

### 证据链（四条，全部是二进制侧）

1. **文件名只有"目录 + 一个整数"两个可变槽**：格式串 `'%s\SAVE%2.2d.DAT'` 的数值实参来自
   `sub_41BF50(this, 2)` = **该指令的第 2 个操作数**（整型读）。
   〔EA 0x42D9C3 `push 2` / 0x42D9C7 `call sub_41BF50` / 0x42D9D4 `push offset aSSave22dDat`；
   `sub_41BF50` 取操作数的定址在 0x41BF50 体内（`[thread+op] + 8*n`，类型标签在值前 4 字节，`0` = 整型）。
   同一个"操作数 2 = 槽号"的模式出现在 0x42DB10、0x42DC70、0x42DDE0、0x42DFC0、0x42E0A0（源/目标 = 操作数 2/3）、
   0x42E1F0、0x42E320。〕
2. **头写侧不含槽号**：`sub_437480` 把 292 字节 `Buffer[292]` 填满后连同 20 字节参数块与负载写出，
   写的格子只有：`+0` 魔数、`+4` 版本串、`+8` 标题（`strcpy_s(&Buffer[8], 0xE8, a4)`）、`+240` 负载字节数、
   `+264` `GetLocalTime` 的 16 字节、`+280` 游戏秒、`+284` 与 `+288` 两个版本字段
   〔EA 0x437480，写侧字面量：`*(_DWORD*)Buffer = 1146303571 : 1146303315`（= `"S4SD"`/`"S3SD"`）、
   `*(_DWORD*)&Buffer[4]=*a3`、`GetLocalTime((LPSYSTEMTIME)&Buffer[264])`、`*(_DWORD*)&Buffer[240]=v31`、
   `*(_DWORD*)&Buffer[280]=v24`、`*(_DWORD*)&Buffer[284]=a7`、`*(_DWORD*)&Buffer[288]=a8`〕。
   **没有任何一格写槽号，也没有任何一格写文件名。**
3. **头读侧不比槽号**：`sub_438120(this, hFile, …, const char *title, int *out)` =
   `ReadFile` 恰好 **292 字节** → `Buffer[0] == "S4SD"`（或 `"S3SD"`）→ `strcmp(&Buffer[2], title) == 0` →
   `qmemcpy(out, Buffer, 0x124)` + `return 1`；任何一步不成立就写错误串并 `return 0`。
   〔EA 0x438120；调用方 5 个：0x410160、0x42D700、0x42D980、0x42DC70、0x438940 → 它就是**唯一**的头读者，
   槽读、只读头、系统容器读全都走它。〕
4. **数值字段唯一的消费者是"只读头"命令**：`sub_42DC70`（opcode 416 = `0x1A0`）把 6 个 u16 与 1 个 u32 交回脚本：
   读 `+264`、`+266`、**跳过 `+268`**、`+270`、`+272`、`+274`、`+276`，再读 `+280` 的 32 位；
   `+268`（星期）与 `+278`（毫秒）**不回传**。
   〔EA 0x42DD49–0x42DDB8：`movzx eax,[ebp+var_220]` → `push 3`…`push 9` → `sub_42B4B0`；
   `var_220`/`var_21E`/`var_21A`/`var_218`/`var_216`/`var_214` 相对 292 字节缓冲的偏移 = `+264/+266/+270/+272/+274/+276`〕。
   这些值只用于**显示**，不参与选址、不与文件名比。

### "改名能用"的判据（可机械复核）

* 槽路径上**文件名就是身份**：唯一的头校验是"魔数 + 标题"，两者都与槽号无关〔EA 0x438120〕。
* 引擎**不扫描目录**、**不读任何索引**〔全语料仅一处 `FindFirstFileA`，通配 `*.AAI`，EA 0x455838〕。
* ⇒ 把 `SAVE76.DAT/.STH` **逐字节复制**成 `SAVE00.DAT/.STH`，读取侧无法把它和"引擎自己往 0 号槽存过一次"区分开
  （能看出来的只有展示字段：头里的存档时刻、`+280` 游戏秒、缩略图内容）。
  ⛔ 反过来不成立的情形也没有：不存在"头里的号覆盖文件名"这条路径 —— 头里没有那个号。

### 槽号的范围（引擎侧）

* **引擎不做范围检查**：没有比较、没有上限、没有拒绝分支；`sub_408050` 只是把整数格式化进字符串
  〔EA 0x42D980 / 0x42DC70 全程只有 `CreateFileA` / `ReadFile` / `WriteFile` 与错误串，无槽号比较〕。
  ⇒ "越界"的表现只是**另一个文件名**。
* 因此"一共几个槽"是 **UI/脚本**的事，不是引擎的事（见下节）。

### 附带：按路径的变体（不经过槽号）

`0x42D580`（opcode 170 = `0xAA`，写）、`0x42D650`（171 = `0xAB`，读）、`0x42D700`（172 = `0xAC`，只读头）、
`0x42D8E0`（413 = `0x19D`，读 + 版本门）这一族**不拼存档目录、也不格式化槽号**：
第 2 个操作数被当作**文件路径**（`sub_454FA0` 解析），并且 `0x42D8E0` 会先看这个操作数
"像不像指针"（`& 0xFF000000`）再比配置里的 `set:SaveVersion`〔EA 0x42D8E0〕。
⇒ 想绕过槽号时这是一条独立的入口，与"改名"无关。

---

## 加载界面的列表与翻页

### 谁在构造列表

**脚本**，不是引擎。引擎侧只有"给一个槽号，读一份头回来"这一个原语：
opcode `0x1A0` = 416 → `sub_42DC70`〔EA 0x42DC70〕。它返回 **状态码**（`0` 成功 / `1` 打不开 / `2` 头不合法）
加七个展示字段（见上节）。

脚本侧（`dist/views/data/SAVE.BIN.txt`，2418 行；`iXXX` 与 EA 的对应见文首）：

| 脚本位置 | 内容 | 对应 opcode → EA |
|---|---|---|
| 第 1092 行 | `i19e (local-int 12) (local-int 214)` = **写槽**（操作数 1 = 结果，操作数 2 = 槽号） | `0x19E` = 414 → 0x42D980 |
| 第 935 行 | `i1a1 (local-int 12) (local-int 214)` = **读槽并应用** | `0x1A1` = 417 → 0x42DDE0 |
| 第 80 / 848 / 1109 / 1255 / 1454 行 | `i1a0 &status[slot], slot, &…` = **取一份头填进列表缓存** | `0x1A0` = 416 → 0x42DC70 |
| 第 1100 行 | `i1ae (local-int 12) (local-int 214) e` = **写缩略图**（操作数 3 = 图像对象号） | `0x1AE` = 430 → 0x42E1F0 |
| 第 1247 / 1446 行 | `i1ac …` = 复制槽 | `0x1AC` = 428 → 0x42E0A0 |
| 第 1308 行 | `i1ab …` = 删槽 | `0x1AB` = 427 → 0x42DFC0 |

### 一页几项

* **10 行**：行索引是脚本局部变量 `2`，判据是 `lt (local-int 21c6) (local-int 2) a`（`a` = 10）
  〔脚本 `SAVE.BIN.txt` 第 732 行 / 第 1788 行附近〕。
* **页基**是脚本局部变量 `61b`，**以 10 为步长**、下界 `0`、上界 `0x50`
  〔脚本 `label_00001f7c`（约第 500–524 行）：`eq 61b 0` → 不能再上一页；`sub 61b 61b a`；
  `eq 61b 0x50` → 不能再下一页；`add 61b 61b a`〕
  ⇒ **9 页 × 10 行 = 手选槽号 0..89**。
* 页基的初值 = "持久化的上次槽号 / 10 × 10"：脚本第 11 行 `load-int (global-int 138e)` 把这个值从
  **`SAVE.DAT` 的脚本键表**读回来，第 54–57 行 `div … a` + `mul 61b … a` 算出页基
  〔脚本 `label_00000368` 附近；`save-int (global-int 138e)` 在"确认读档"之后（第 931 行）与保存之后（第 1020 行）〕。
  ⇒ **打不开或键为空 ⇒ 页基 0（第一页）**。

### 翻页怎么触发（两条，都是引擎送进来的输入）

1. **横向滚轮 = 翻页**（主路径）
   * WndProc 的 `WM_MOUSEHWHEEL`（0x020E = 526）支把 delta 累加到 `engine+0x1E78`
     〔EA 0x4B9D0D：`add [eax+1E78h], ecx`，`ecx = (signed short)(wParam>>16)`；
     该 switch 的另一条同形支是 `WM_MOUSEWHEEL`（0x020A = 522）→ `add [eax+1E74h], ecx`，EA 0x4B9CCB〕
   * 脚本用 `i2e5` 取走并**清零**该寄存器：`sub_4310D0` = `v = this[1950]; this[1950] = 0; return v`
     （`1950*4 = 0x1E78`）〔EA 0x4310D0；opcode `0x2E5` = 741〕
   * 脚本判据：`i2e5 (local-int 15)` → `15 != 0` 就进翻页函数；`15 < 0` = 上一页（`61b -= 10`）、`15 > 0` = 下一页
     〔脚本第 304–305 行 + `label_00001f7c`；`jcc` 的方向由 opcode 160 = `0xA0` → `sub_4209B0` 定：
     操作数 1 非 0 跳第 2 个操作数、为 0 跳第 3 个，**第 2 个操作数是 `-1` 就落空不跳**〕
2. **纵向滚轮 = 移动选择，页跟着走**（更容易驱动的一条）
   * `WM_MOUSEWHEEL` → `+0x1E74`〔EA 0x4B9CCB〕→ 脚本 `read-mouse-wheel (local-int 14)`
   * `14 > 0`：选择槽号 `138e = (138e + 1) % 0x5a`；`14 < 0`：`(138e + 0x59) % 0x5a`
     （`0x5a` = 90 ⇒ 在 0..89 里循环）〔脚本 `label_00006f00` / `label_00006ebc`〕
   * 然后 `61b = (138e / 10) * 10`（页基跟随），变化了才刷新〔脚本 `label_00006f44`〕
   * ⇒ **只要往下滚 N 次就能从第 1 页走到第 N+1 页**；不需要横向滚轮。

### 空槽怎么表示

* **文件不存在** ⇒ `sub_42DC70` 返回 `1`（静默、无对话框）〔EA 0x42DC70 的 `CreateFileA == -1` 支〕。
* **文件存在但头不合法**（魔数/标题不符）⇒ 返回 `2`（同样静默）〔同上，`sub_438120` 返回 0 的那一支〕。
* 脚本把状态写进按槽索引的数组（`i1a0` 的第 1 个操作数是 `&status[slot]`，即 `local-int 61d` 的元素），
  再据此画/不画该行〔脚本第 848–860 行的刷新子程序〕。
* 脚本还有一条与"槽号"相关的**另一族**槽：`gre (local-int 214) 0x3d4`（槽号 > 980）时拒绝手动保存并提示
  "自動セーブ枠とクイックセーブ枠には任意セーブできません"〔脚本第 939 行，`label_00003af0`〕
  ⇒ 980 以上属自动/快速存档族；**它们的文件名长什么样未取证**（`%2.2d` 遇 3 位数会照写）⇒ `<待核>`。

### 第一页有没有东西 —— 决定"要放几个槽"

* 引擎侧不关心（没有扫描）⇒ 只取决于**页基初值**与**该页 10 个文件名在不在**。
* 页基初值来自 `SAVE.DAT` 里持久化的"上次槽号"。用现成样本时它可能是 7n（于是第一屏就停在 70–79），
  也可能键为空（于是停在 0–9）⇒ **两种情况都要覆盖**：
  把 `00..09` 铺满，并**同时保留** `76..79`（见下一节的命令）。

---

## 缩略图 `.STH`

* 是一份**引擎自己组装的 BMP**，不是平台截图文件的转存。写侧 = `sub_43BF20(this+1978, 图像对象号, hFile)`
  （配置 `this[166964]==1` 时走 `sub_4A5260` 另一支）；读侧 = 先把整份文件读进内存（`GetFileSize` + 读），
  再 `sub_40BF20(this+1978, 图像对象号, -1, buf, size)`〔EA 0x42E1F0 写 / 0x42E320 读；
  两个函数都用操作数 2 = 槽号拼 `.STH` 名，操作数 3 = 图像对象号〕。
* 样本实测（`xxd -l 64 corpus/fixtures/SAVE76.STH`）：`BM`、声明文件长 `0x0002A328 = 172840`、
  像素偏移 `0x36 = 54`、信息头 40、宽 `0x140 = 320`、高 `0xB4 = 180`、平面 1、位深 `0x18 = 24`、压缩 0、`biSizeImage` 0。
  实际文件长 **172854** ⇒ 声明值比实际**少 14**（写侧写的是 `40 + 像素字节数`）。
* **能不能缺**：读侧打开失败就返回 `1`〔EA 0x42E320 的 `CreateFileA == -1` 支〕⇒ 不会阻止读档。
  列表在缺 `.STH` 时画什么（空框 / 上一张残留）**未取证** ⇒ `<待核>`。

---

## 外部组装（逐条命令）

**纪律**：只读 `corpus/fixtures/`（⛔ 不改名、不改 mtime、不就地改字节）；产物落 `.tmp/save/<变体>/`；
⛔ 不碰 `links/install`。下面这套**没有在本轮执行**（本轮是只读取证），命令按可粘贴形式给出。

```bash
cd /Users/nekosu/Documents/Projects/amayui-re
OUT=.tmp/save/page0
mkdir -p "$OUT"

# ① 两个系统容器（一起带；缺 SAVE.DAT 会退到 .BAK 再退到"跳过"，缺 RT.DAT 的后果未取证）
cp -p corpus/fixtures/SAVE.DAT corpus/fixtures/RT.DAT "$OUT/"

# ② 第一页铺满：把 76..79 复制成 00..03（-p 保留 mtime，见下面的说明）
i=0
for src in 76 77 78 79; do
  dst=$(printf '%02d' "$i")
  cp -p "corpus/fixtures/SAVE${src}.DAT" "$OUT/SAVE${dst}.DAT"
  cp -p "corpus/fixtures/SAVE${src}.STH" "$OUT/SAVE${dst}.STH"
  i=$((i + 1))
done

# ③ 同一对文件再摊满 04..09（让第一页 10 行都有项；缩略图相同不影响"有项"）
for dst in 04 05 06 07 08 09; do
  cp -p corpus/fixtures/SAVE76.DAT "$OUT/SAVE${dst}.DAT"
  cp -p corpus/fixtures/SAVE76.STH "$OUT/SAVE${dst}.STH"
done

# ④ 原槽号也留着（样本清单声称的"上次槽号"若落在 7n，第一屏就停在 70..79，这一组直接有项）
cp -p corpus/fixtures/SAVE7[6-9].DAT corpus/fixtures/SAVE7[6-9].STH "$OUT/"

# 核对（只读）
ls -l "$OUT"; echo "DAT=$(ls "$OUT"/SAVE[0-9][0-9].DAT | wc -l) STH=$(ls "$OUT"/SAVE[0-9][0-9].STH | wc -l)"
```

**mtime 要不要还原？**

* **引擎不读**：整个语料里 `GetFileTime` / `GetFileAttributesA` 只有 4 处
  （`0x40491F`、`0x499344`、`0x49953A`、`0x499550`），**没有一处**落在槽读写那一段
  （`0x42D8xx–0x42E4xx`）或头读写（`0x437480` / `0x438120`）里
  〔复核：`rg -n 'GetFileTime|GetFileAttributesA' corpus/disasm/files/*.lst`〕。
  头里的存档时刻是 `GetLocalTime` 写进去的**独立副本**〔EA 0x437480〕。
* **本仓的判据要**：`corpus/fixtures/samples.json` 记了源 instant，测试断言"至少一个真槽的头 == 文件 mtime"。
  那只对 `corpus/fixtures/` 本体生效。复制到 `.tmp/` 的副本用 `cp -p` 继承即可；
  若要显式拨回，**不要**用 `touch -t`（跨时区会错），用 instant：
  `touch -d @$(node -e "console.log(1790130898)") <file>` 这类写法，或直接依赖 `cp -p`。
  ⛔ 不要对 `corpus/fixtures/` 里的文件跑。

**这样组出来的目录，引擎会认为是几个槽？**

* **引擎侧：没有"几个槽"这个概念** —— 不扫描、不索引〔EA 0x455838 唯一 `FindFirstFileA` 是 `*.AAI`〕。
  它只在某个 opcode 被调用时把一个槽号变成一对文件名。
* **UI 侧**：上面这套让手选槽号 `00..09`（第一页 10 行）与 `76..79` 都有项 ⇒
  * 页基初值 = 0（`SAVE.DAT` 里的键为空 / 打不开）⇒ **第一页 10 行全有项**；
  * 页基初值落在 70（键 = 76..79 之一）⇒ **第一屏就是 70..79，其中 4 行有项**。
  两种初值下"第一页都有东西"，不需要先翻页。
* 自动/快速存档族（槽号 > 980）不在这套里，也不该由外部伪造（它们由存档链自己写）⇒ `<待核>`。

---

## 待核

1. **刷新一次会 open 哪些槽号**：脚本刷新子程序按 `array232[页基*10 + i]`（`i < 100`）取槽号，
   而绘制只画 10 行、页基步长又是 10 —— 两种"乘 10"并存，我**没有裁决**"一次翻页实际会去 open 0..89
   还是每页 100 个（含 90..899）"。
   〔脚本 `label_000032fc`（约第 833–860 行）与 `label_00002194`（约第 550 行）；
   复核方式：一次 run 里记录 `CreateFileA` 的实参（或 `strace`-级跟踪），或让控制层 `pause`+`sample` 看缓存数组〕
2. **点击 → 槽号的命中框**：`label_00002194` 用 `138e*10 + 行` 索引同一张数组，与"页基 = 槽基"的用法冲突
   （见上一条）。**没有裁决**；本轮不猜。
3. **自动/快速存档槽（> 980）**：文件名字形（`SAVE980.DAT`？别的名字？）、页面上在哪、缩略图有没有 —— 全部未取证。
4. **`RT.DAT` 缺了会怎样**：只确认它走同一套外壳与同一个启动期读侧〔EA 0x40AEE0 → 0x438940〕，
   缺件后的可见后果未取证。
5. **`--appdata` 该指哪一层 —— 已裁决（用户口径 2026-11-16）**：`--appdata` 指的就是 **`%AppData%` 这一层**，
   引擎自己在它下面建 `Eushully\<标题>\…`（目录不存在时由目录解析器用 `<dir>\SAVE.DAT` 探针建出来，见 §1）。
   ⇒ 外部组装的落点是 `<--appdata>/Eushully/<标题>/SAVE/`。
   ★ **实测（2026-11-16，用户自己跑出的一份真目录）**：`.tmp/appdata/` 里就是这套结构 ——
   `Local/Eushully/天結いキャッスルマイスター/{SAVE.DAT,RT.DAT,SAVE00..01..03.DAT/.STH}`。
   原因在 VFS：guest 的 `CSIDL_LOCAL_APPDATA` = `C:\Users\Amayui\AppData\Local` 映射到 **`<--appdata>/Local`**、
   `CSIDL_APPDATA`（漫游）= `…\AppData\Roaming` 映射到 `<--appdata>/Roaming`
   〔`apps/emulator-lift/src/runtime/vfs.cpp:516-517,671`，`setAppDataDir` 在 `:590`〕。
   ⇒ 存档目录主机路径 = **`<--appdata>/Local/Eushully/天結いキャッスルマイスター/SAVE/`**（`<标题>` = 游戏标题目录名）。
   ⚠ **组装时只用 `corpus/fixtures/` 的原版样本**（用户口径 2026-11-16）——但理由**不是**"头校验会失败"：

   **实测（用户，2026-11-16）**：旧汉化版的存档**能看见、能加载**，只有存档里"标题 / 当前章节"这两个**字符串**
   按 SJIS 解码成**乱码**（旧汉化把脚本文本改成了 **GBK**；我们产出的存档是 **Shift-JIS**）。
   ⇒ 编码差异落在**载荷里的文本**上，**不影响**槽能否被读到。

   ⛔ 因此别把"头里 `+8` 那个字符串 == 列表里显示的标题"当成结论：
   `+8` 只是写侧 `strcpy_s(&Buffer[8], 0xE8, a4)` 填进去的一个 232 字节串，读侧拿它跟传入的 `title` 比
   〔EA `0x438120`〕——它能让 GBK 存档通过，说明它**不是**那个被渲染成乱码的显示标题（那一个在载荷里）。
   `a4` 到底是什么、`strcmp` 的 `title` 从哪来 ⇒ `<待核>`。

   ⇒ 用 fixtures 的真正理由只有一个：**读数干净**（原版文本，不会在列表里出现乱码而干扰"有没有项 / 点没点中"的判断）。
   ⚠ 下面这段保留为"当初为什么没定"的记录。
5b. **`set:SavePath` 的实际值与默认值表**：`.data:00529BD4 aSavepath = 'SavePath'`（键名）紧邻
   `.data:00529BC8 aRegfilepath = 'RegFilePath'`，而 `.data:00529A5C aEushully = 'Eushully'` 与
   `.data:00529A68 aSave = 'SAVE'` 也紧挨着 —— 真机路径恰好是 `<根>\Eushully\<标题>\SAVE`，
   ⇒ 两个串很可能是**默认值的组成片**。但"默认值表怎么被读、怎么拼"没有逐字取证
   ⇒ `--appdata` 该指哪一层（appdata 根 / 还是 appdata 下的 `Eushully\…`）需要**一次 run** 确认
   （最快的判据：让控制层把 `sub_408A40` 的输出（`ArgList`）打出来，或看它 `CreateFileA` 的实参）。
6. **列表里"空槽"的取值方向**：脚本保存成功后把 `61d[slot]` 置 `1`，而另一处用 `!= 0` 判"不可加载" ——
   两处口径相反，**没有裁决**。
7. **槽路径的版本门**：`set:SaveVersion1/2` 与头 `+284/+288` 的比较我只在"按路径"的变体上读到
   〔EA 0x42D8E0〕；**槽路径**（`sub_410160` 内部）的版本门未逐字核。
8. **`%2.2d` 的负数/三位数行为**：`sub_408050` 是自研格式化例程，我只核了正数两位的样本
   （`00`、`76`）；负数与 ≥100 的字形未取证（引擎侧不做范围检查这一点已核）。
9. **缩略图能缺到什么程度**：读侧打开失败返回 `1` 已核〔EA 0x42E320〕；列表缺图时画什么未取证。
