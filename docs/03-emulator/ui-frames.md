# overlay「local frame 列表 / 栈顶脚本名」取证报告

- 取证对象：`corpus/disasm/files/AGE.EXE__dumped.sectfix.EXE.lst`（只读）
- 锚：**EA**（不锚 `.lst` 行号；行号只因换一次导出就全变）。`.c` 只用来提假设，不构成证据。
- 既有实现：`apps/emulator-lift/src/runtime/engine_marker.{h,cpp}`（身份采样器）、
  `data/requirements/01M74Z3E0K9QXP93T881KRQ1B2.md`（`script` 行 = **当前帧**，不是栈顶）。
- 本文**只**回答"帧列表 / 栈顶"能不能取证、怎么取证；⛔ 不改任何产品代码 / 测试 / 文档 / 数据。

---

## 结论

> **TL;DR**：帧列表**可以取证**——40 槽数组（`Engine+0x5D894`，步长 `0x78`），每槽
> `body(+0x00) != 0` 即"已装载"，`parent` 在 `+0x38`，`0x3` 的子帧确实落在 `parent+1` 槽。
> "栈顶"只能定义成 `cur`（执行点）；**引擎没有独立的栈顶变量**，`0x6`/`0x8` 能让槽乱序装载。
> `+0x5C`（spec 说的 `ret`）只有写点、**本镜像零读点** ⇒ 不进 API，返回接续靠 `+0x60` 长度槽。

### 已证（C1–C14；每条都能在 `.lst` 里按 EA 再核）

- **C1 · 帧区几何**：`Engine+0x5D894` 起 **40 条记录**、步长 `0x78`、区间
  `[Engine+0x5D894, Engine+0x5EB54)`。Engine 对象 = `operator new(0xAABC8)`（`0x4BAA11`，发布点
  `0x4BAA3A`）⇒ 帧区**在对象内**，且 `0x5EB54 < 0xAABC8`。初始化 = 循环 `slot=0..39` 调
  `sub_405640`（`0x415BFE` 调用；`0x415C0A cmp eax,28h`；`0x415C0D jl`）。
- **C2 · 记录字段映射**（偏移 / 写点 / 读点见「证据」表）：
  `+0x00` body 基址 · `+0x04` PC · `+0x08..+0x1C` 六个 local 池 **count** ·
  `+0x20..+0x34` 六个 local 池 **base** · `+0x38` **parent** · `+0x3C` **统一文件 id** ·
  `+0x40/+0x44`、`+0x48/+0x4C`、`+0x50/+0x54` 三组 **(count, ptr) 表** ·
  `+0x58` **当前位置（word index）** · `+0x5C` **保存的位置（word index / 表元素）** ·
  `+0x60` **指令长度 `2*argc+1`** · `+0x64/+0x68/+0x6C` 三块 8 B/元素堆表 ·
  `+0x70/+0x74` 两个 16 B 容器（3 dword：begin/end/cap）。
- **C3 · 槽的"活动"判据 = `record+0x00 != 0`**（body 基址非 0，且指向已提交页）：
  `0x2` 的向下跳过空槽（`0x41A92E` / `0x41A958`）、`0x8` 的"未装载"判定（`0x41C946`）、
  释放 `0x40EA16..0x40EA27`（`GlobalFree` 后写 0）、初始化 `0x405681`（写 0）三处一致。
  **id 非 0 / PC 范围都不是活动判据**（id=0 是根脚本的合法 id；PC 只在位置读数里用）。
- **C4 · 空槽的规范形状**：`sub_405640`（resetFrame）写 `body=0` / `PC=0` / `len=0` /
  `id=-1` / `parent=-1` / `+0x58=-1` / `+0x5C=-1` / 六个 base=0 / 三块堆表指针=0 /
  两个容器指针=0。释放 `sub_40EA00` 写 `body=0` / `PC=0` / `id=-1` / `parent=-1` /
  `+0x58=-1` / `+0x5C=-1`（**不清** `+0x60` 与 `+0x08..0x1C` 的旧值 ⇒ 不能拿它们判空）。
- **C5 · 槽可以乱序装载 / 任意释放 / 留空洞**（⇒ "0..cur 全算活动帧"是**错的**）：
  `0x6` 把**任意**槽号（operand 2）当目标、临时改 `cur` 再还原（`0x41C854` / `0x41C8A6`，
  上界 `0x41C85A cmp eax,28h`）；`0x7` 释放**任意**槽（`0x41C8F7` 调 `sub_40EA00(operand1)`）；
  `0x8` 切到**任意已装载槽**（`0x41C940`），未装载则报 `この階層にはファイルが読み込まれていません．Depth=%d`；
  `0x9` 一次释放全部 40 槽再重装根（`sub_428A60`：`for (i=0;i<40;++i) sub_40EA00(this,i)`）。
  ★ `0x2` 的释放是**有条件**的（`parent+1 == cur` 才释放，`0x41A90E..0x41A916`）⇒
  `0x8` 跨槽返回后，被切出的那一槽**会留在"已装载"状态**（body != 0）——陈旧槽是设计使然，不是 bug。
- **C6 · `0x3` = 进入子脚本，子帧固定在 `parent+1` 槽**：`0x41C73C inc ecx` /
  `0x41C73D mov [Engine+0x5D880], ecx`，随后 `0x41C746 call sub_40ED40`（装载器）在**新 cur** 上写记录。
  深度判据是**槽号**不是层数：`0x41C6C1 cmp [Engine+0x5D880],27h`（39）⇒ 最大 40 槽。
- **C7 · `parent` = 记录 `+0x38`**，语义 = "把我装进来 / 切进来的那个帧号"：
  装载器 `0x40EDFF mov eax,[Engine+0x5D884]` / `0x40EE05 mov [record+0x38],eax`；
  `0x3` 装载前往 `0x5D884` 写**旧 cur**（`0x41C736`）；`0x6` 同样写旧 cur（`0x41C84E`）；
  `0x8` 直接把 `0x5D884`（= 切帧者）写进目标槽 `+0x38`（`0x41C92B` → `0x41C996`）；
  `0x4` 写 `cur-1`（`0x41C780`）；`0x2` 读它（`0x41A834`）；构造期 `0x5D884=-1`（`0x40DF44`）
  ⇒ 根帧 parent = -1；释放写 -1（`0x40EA41`）。
- **C8 · `cur` = "正在执行的帧"，而不是"最高活动槽"**：`0x6` 装载任意槽时**不改 cur**
  （`0x41C8A6` 还原成旧值）；`0x8` 可以切到比 cur 低或高的任意已装载槽（`0x41C940`）。
  ⇒ 只存在"`cur` 这个执行点"，不存在独立的"栈顶变量"；"栈顶"是**定义**问题（见 U5）。
- **C9 · `+0x3C` = 统一文件 id**：装载器 `0x40ED82 mov ebx,[ebp+arg_4]` → `0x40EDEB mov [record+0x3C],ebx`；
  `0x3` 的 arg4 = operand 1（`0x41C6FF push 1 / call sub_41BF50` → `0x41C720 push eax`）；
  `0x6` 的 arg4 = operand 1（`0x41C838` / `0x41C896 push edi`）；
  同一次装载用 `(a3=Engine+0x5EB54, a4=id)` 查 FileDB（`0x40ED9D..0x40EDAC sub_4559C0`）。
  读点例：`0x4083CC cmp Engine+0xCA6C, record(cur)+0x3C`，不等则用 `sub_454FA0` 把两边 id 解成名字
  报 `Depth が不正です %s != %s`（`0x4083DF` / `0x408400`）；`0x412A99` 把 id 抄进 `Engine+0x69278`。
  释放写 -1（`0x40EB1D`）。
- **C10 · 返回后"PC 怎么接上"= 长度槽 + 派发后的统一步进**（这是唯一在本镜像里闭合的机制）：
  派发点在 handler 返回后按**当前 cur** 的 `+0x60` 推进 PC：`0x411A7E`→`0x411A8C add [PC],4*len`、
  `0x41347D`→`0x413487`、`0x413885`→`0x41388F`。`0x3` 把**调用者**的 `+0x60` 写 3（`0x41C6B6`）、
  自己不改调用者 PC；`0x2` 把 `cur` 拨回 parent 时**正常返回路径不写** `+0x60`
  （唯一写 `+0x60` 的分支是 parent==-10：`0x41A8B2 mov [..+5D8F4h],edi`，edi=0，走的是"恢复 savedCur"那条）
  ⇒ `0x2` 返回后的步进用 parent 的旧长度 3 ⇒ 正好跨过 `0x3` 指令。对称地：`0x6` 写自己的 5（`0x41C7D6`）、
  `0x8` 写自己的 3（`0x41C916`）、`0x7` 写 3（`0x41C8E0`）；装载器把**新帧**的 `+0x60` 清 0
  （`0x40F6EE`，`ebx` 在 `0x40F397` 被 `xor ebx,ebx` 置 0）⇒ 进入子帧时步进 0，PC 停在 body 起点。
- **C11 · `+0x5C` 是"保存的位置"，但本镜像里没有读者**：写点 `0x41C724`（`0x3` 存调用者位置 =
  `(PC-body)>>2`）、`0x4194B9`（另一个控制转移 handler 同形）、`0x40F7CD/0x40F887/0x40F942/0x40FA87`
  （`sub_40F750`：从 `+0x48/+0x4C` 表里取出元素写进去）；清点 `0x41A998`（`0x2` 返回时写 -1）、
  `0x40EB29`（释放）、`0x405671`（初始化 -1）。
  ★ **全语料 `rg '5D8F0h'` 只有写、没有一处读**（唯一非写的命中是 `0x45D8F0` 这个函数名与
  `0x55D8F0` 这个全局，都不是帧槽）⇒ 不能声称"引擎用 `+0x5C` 恢复返回点"。
- **C12 · parent 的负值哨兵**：`-1` = 根 / 无父（`0x40DF44` + `0x2` 的分支）；`-10` = id 队列驱动的换脚本
  （`0x40FBB2` 存 `savedCur`、`0x40FBCE/0x40FBD3` 写 parent=-10、`0x40FBF5` 切槽）；`-11` = 存档版本装载
  （`0x410DA4` / `0x41151B` 写 `0x5D884=-11` 后调装载器），`0x2` 对 -11 / -10 有各自的特判
  （`0x41A84C cmp eax,0FFFFFFF5h` / `0x41A896 cmp eax,0FFFFFFF6h`）。
  ⇒ API 必须原样暴露 `parent`（含负值），⛔ 不能把负数一律当"根"。
- **C13 · id→名字的解析器对任意 id 都可复用**：`MarkerSampler::resolveName()` 只读 `s->id`
  （`engine_marker.cpp` 的 `resolveName` / `resolveIdName`），不看 `cur`；`nameTableSignature(mem, raw_id)`
  本来就按 id 取指纹。引擎侧同一个解析器 = `sub_454FA0`（`0x4083DF` / `0x408400` 两处实调）。
  ⇒ 任意槽只需构造 `MarkerSample{valid=true,id=raw}` 再调 `resolveName`，**不需要改 engine_marker**。
- **C14 · 存档写出器读的就是这些字段**（⇒ 它们是引擎的持久帧状态，不是宿主臆造）：
  `0x40D2EA` / `0x40D5AB` 读 `record(slot)+0x38`；`0x40D360` 同时读 `[eax]`(parent) 与 `[eax+4]`(id)；
  `0x40D3BB` 把 `+0x5C` 在 `+0x48/+0x4C` 表里**查下标**；`0x40D437` 把 `+0x58` 在 `+0x40/+0x44` 表里查下标。

### 未证 / 不可证（U1–U8；⛔ 不许写成结论）

- **U1 · `+0x5C` 的消费者**：本镜像零读点。它可能是别的模块 / 别的构建的消费者，也可能就是死字段。
  ⇒ 帧列表 API **不依赖**它，也不把它当"返回点"。
- **U2 · `+0x64/+0x68/+0x6C` 三块 8 B/元素堆表**：只证到"装载时 `operator new[]` + `0xFF` 填充、
  释放时删"（`0x40F583..0x40F5F4` / `0x40EB7C..0x40EBA0`），**没有找到读者** ⇒ 语义未证。
- **U3 · 三组 `(count,ptr)` 表（`+0x40..+0x54`）的元素语义**：证到"count 来自脚本头、ptr = body + 4*头里的字偏移、
  是 dword 数组、被存档按值查下标、`+0x58` 用它查消息、`+0x5C` 用它查控制转移目标"，
  但**元素本身是"label 位置"还是"字符串偏移"还是"键"没有逐条取证**。⇒ API 不暴露它们。
- **U4 · `0x6` / `0x8` 在真实脚本里是否真的被用到**：handler 语义已证（任意槽），但"哪些脚本用了、
  用到哪一槽"是**数据层**问题，本轮未普查。⇒ 不能拿"真脚本会留下陈旧槽"当已证事实。
- **U5 · "栈顶"的定义**：引擎没有独立的 top；`cur` 是执行点，`max(活动槽)` 是另一回事（C8）。
  选哪个是**产品口径**，不是引擎事实。本报告推荐 `top = (slot == cur)`。
- **U6 · 名字条目的 NUL 保证 / 80 字节截断**：沿用 `engine_marker.h` 的既有口径（读到 NUL 或 80 字节为止），
  本报告没有新增取证。
- **U7 · 真 boot 下帧区各页的"已提交"状态**：只证到 Engine 对象大小（C1）与判据（C3），
  没有真机读数证明"40 条记录的 0x78 字节全部已提交"。⇒ 读之前仍然逐条 `committed`。
- **U8 · `Engine+0x5EB54` 的角色**：装载器把它当 a3（FileDB 查找的一个键），handler 也读它
  （`0x41C730` / `0x41C890` / `0x40F774`…），但**它的写者与语义未定** ⇒ 不进 API。

---

## 证据

> 复现方式：`pnpm tools disasm-at at --ea 0x41C6A0 --lines 60`（有界窗口）·
> `pnpm tools disasm-fn show sub_41A820 --lst`（整函数）·
> `rg -n '<EA 的十六进制>' corpus/disasm/files/AGE.EXE__dumped.sectfix.EXE.lst`。
> 指令原文按 `.lst` 抄；偏移一律"记录内偏移"（基址 `Engine+0x5D894`）。

### 1. 帧区几何 / 初始化

| # | EA | 指令原文（摘） | 偏移 | 证明了什么 |
|---|---|---|---|---|
| 1 | `0x4BAA11` | `push 0AABC8h` / `call ??2@YAPAXI@Z` | — | Engine 对象大小 `0xAABC8` ⇒ 帧区在对象内、可做上界检查 |
| 2 | `0x4BAA3A` | `mov ds:dword_55E1BC, eax` | — | 单例发布点（既有口径） |
| 3 | `0x415BFE` | `call sub_405640`（`0x415C0A cmp eax,28h` / `jl`） | — | 初始化逐槽循环 **0..39** |
| 4 | `0x405640` | `mov [eax+5D894h],esi` 等一整段 | 见 C4 | 空槽的规范形状（resetFrame） |
| 5 | `0x415C0A` | `cmp eax, 28h` / `0x415C0D jl` | — | 40 槽（`0x28`） |

### 2. 记录字段：写点 → 读点

| # | 字段（偏移） | 写点（EA / 指令） | 读点（EA / 指令） | 证明什么 |
|---|---|---|---|---|
| 6 | `+0x00` body | `0x40EE57 mov [..+5D894h],eax`（`GlobalAlloc` 返回）；`0x40EA27` 写 0 | `0x41A92E` / `0x41A958 cmp [..+5D894h],edi`；`0x41C946 cmp dword ptr [ecx+5D894h],0`；`0x40F067` / `0x40F69E`（ReadFile lpBuffer） | body=0 ⇔ 空槽（C3） |
| 7 | `+0x04` PC | `0x40F6DB mov [eax+5D898h],edx`（edx=body）；`0x41C9B3`（`0x8`）；`0x412AB5`（恢复待办位置）；`0x413487` / `0x411A8C` / `0x41388F add [..+5D898h],4*len` | `0x411A52` / `0x413446`（取 opcode）；`0x41EDC6` | PC；步进 = 4×长度槽 |
| 8 | `+0x08..+0x1C` 六 count | `0x40F228/0x40F242/0x40F25C/0x40F276/0x40F290/0x40F2AA` 各 `mov [..+5D89Ch..5D8B0h],edx` | `0x40F4C0`（memset 尺寸）、`0x40F4A0` 循环 | 六个 local 池的元素数 |
| 9 | `+0x20..+0x34` 六 base | `0x40F2E9`(+0x20) / `0x40F321`(+0x24，`cur+0C79h` 折叠) / `0x40F3B1`(+0x28) / `0x40F3F0`(+0x2C) / `0x40F42F`(+0x30) / `0x40F471`(+0x34) | `0x42AF5B` case 9 → `[..+5D8B4h]`；`0x42AFB7` case 10 → 折叠 `[..]`；`0x42AF99` case 13 → `[..+5D8C4h]`；装载 memset `0x40F50D..0x40F579` | 六个池基址；配对由同一个函数的 `memset(base,len)` 与取址原语钉住 |
| 10 | `+0x38` parent | `0x40EE05 mov [..+5D8CCh],eax`（eax=`[Engine+5D884h]`）；`0x41C996 mov [ecx+5D8CCh],edx`（`0x8`）；`0x41C780`（`0x4` 写 cur-1）；`0x40EA41` / `0x4056B3` 写 -1 | `0x41A834 mov eax,[..+5D8CCh]`（`0x2`）；`0x40D2EA` / `0x40D5AB` / `0x40D360`（存档） | parent = +0x38（C7） |
| 11 | `+0x3C` id | `0x40EDEB mov [..+5D8D0h],ebx`；`0x40EB1D` / `0x405665` 写 -1 | `0x4083CC` / `0x4083F2`；`0x4070A7`；`0x412A99`；`0x40D360` | 统一文件 id（C9） |
| 12 | `+0x40/+0x44` 表0 | `0x40F050 mov [..+5D8D4h],edx`；`0x40F076 mov [eax+5D8D8h],edx`（= body+4*字偏移） | `0x407086` / `0x411FF6` / `0x4121DE` / `0x41B25A` → `sub_48E870`；`0x40D437`（存档查下标） | (count,ptr) 表，`+0x58` 的宿主表 |
| 13 | `+0x48/+0x4C` 表1 | `0x40F08F mov [..+5D8DCh],edx`；`0x40F0B5 mov [eax+5D8E0h],edx` | `0x4194D6` / `0x4194E8`（`PC = body + 4*表1[i]`）；`0x40D3BB`（存档查下标）；`0x40F7C0` | (count,ptr) 表，控制转移目标表 |
| 14 | `+0x50/+0x54` 表2 | `0x40F0CE mov [..+5D8E4h],edx`；`0x40F0F4 mov [eax+5D8E8h],edx` | `0x40F9E0 mov edi,[esi+edi*8+5D8E8h]`（+ `add edx,3` 写 `5EEA4h` 辅助数组） | (count,ptr) 表；语义 U3 |
| 15 | `+0x58` 当前位置 | `0x41EDDA mov [eax+5D8ECh],edx`（`0x41EDD7 sar edx,2`）；`0x40F814`；`0x40EB23` / `0x40566B` 写 -1 | `0x407086` / `0x411FF6` / `0x4121DE` / `0x41B25A`；`0x40D437`（存档） | 当前指令的 word index（用于消息/报错定位） |
| 16 | `+0x5C` 保存位置 | `0x41C724 mov [ecx+5D8F0h],edx`；`0x4194B9`；`0x40F7CD` / `0x40F887` / `0x40F942` / `0x40FA87` | **零读点**（`rg '5D8F0h'` 全是写 / 清） | 保存位置存在，但消费者未证（C11/U1） |
| 17 | `+0x60` 长度 | `0x41C6B6` 写 3（`0x3`，写**调用者**）；`0x41C7D6` 写 5（`0x6`）；`0x41C916` 写 3（`0x8`）；`0x41C8E0` 写 3（`0x7`）；`0x40F6EE` 写 0（新帧）；`0x41C9C6` 写 0（`0x8` 目标） | `0x411A7E` / `0x41347D` / `0x413885`（步进）；`0x412AC8` 写 0 | 长度 = `2*argc+1`；返回接续机制（C10） |
| 18 | `+0x64/+0x68/+0x6C` 堆表 | `0x40F5C1 mov [..+5D8F8h],eax`（`operator new[](8*(count+1))`）；`0x40F5EC memset(...,0FFh)` | 只有 `0x40EB7C..0x40EBA0` 的释放循环 | 已分配 / 已释放；**读者未证（U2）** |
| 19 | `+0x70/+0x74` 容器 | `0x40F63B mov [..+5D904h],ecx`；`0x40F678 mov [..+5D908h],ecx`（各 `new(0x10)`，3 dword 清零） | `0x42B24A` / `0x42B362` / `0x42C30A` / `0x42C422` / `0x433507` | 两个 16 B 容器（begin/end/cap） |

### 3. 控制转移 handler（写点集中处）

| # | EA | 指令原文（摘） | 偏移 | 证明了什么 |
|---|---|---|---|---|
| 20 | `0x41C6B6` | `mov dword ptr [esi+ecx*8+5D8F4h], 3` | `+0x60` | `0x3` 的长度写进**调用者**（返回步进用，C10） |
| 21 | `0x41C6C1` | `cmp dword ptr [esi+5D880h], 27h` / `jl` | — | 深度按**槽号**，最大 39（40 槽） |
| 22 | `0x41C71A` / `0x41C724` | `sub edx,[ecx+5D894h]` / `sar edx,2` / `mov [ecx+5D8F0h],edx` | `+0x5C` | `0x3` 存调用者位置（word index） |
| 23 | `0x41C736` / `0x41C73C` / `0x41C73D` | `mov [esi+5D884h],ecx` / `inc ecx` / `mov [esi+5D880h],ecx` | — | **子帧 = parent+1 槽**（C6）；parent 来源（C7） |
| 24 | `0x41C746` | `call sub_40ED40` | — | 新 cur 上执行装载器 |
| 25 | `0x41C7D6` / `0x41C84E` / `0x41C854` / `0x41C85A` / `0x41C89A` / `0x41C8A6` | 写 5 / 存旧 cur / 临时改 cur / `cmp eax,28h` / 调装载器 / 还原 cur | — | `0x6` = 任意槽装载、cur 不动（C5） |
| 26 | `0x41C8E0` / `0x41C8F7` | 写 3 / `call sub_40EA00` | — | `0x7` = 释放 operand1 指定的槽（C5） |
| 27 | `0x41C916` / `0x41C92B` / `0x41C940` / `0x41C946` / `0x41C996` / `0x41C9B3` / `0x41C9C6` | 写 3 / 存切帧者 / `cur=operand1` / `cmp [..+5D894h],0` / 写目标 `+0x38` / 写目标 PC=body / 写目标 len=0 | — | `0x8` = 切到任意已装载槽，parent=切帧者（C5/C7/C10） |
| 28 | `0x41A834` / `0x41A83E` / `0x41A908..0x41A916` / `0x41A928` / `0x41A92E` / `0x41A940..0x41A95F` / `0x41A998` | 读 parent / 写 `5D884` / `inc eax; cmp eax,ecx; jnz` + 调 `sub_40EA00` / `cur=parent` / 测 body / 向下跳过空槽 / 清 `+0x5C=-1` | — | `0x2` = 返回 parent、**有条件释放**、**按 body 判空**、清保存位置（C3/C7/C10/C11） |
| 28b | `0x41A8B2` | `mov [esi+ecx*8+5D8F4h],edi`（edi=0，-10 分支） | `+0x60` | 正常返回不写长度；只有"恢复 savedCur"分支清 0（C10 的边界） |
| 29 | `0x41A84C` / `0x41A896` | `cmp eax,0FFFFFFF5h` / `cmp eax,0FFFFFFF6h` | — | parent 的 -11 / -10 哨兵（C12） |
| 30 | `0x40FBB2` / `0x40FBCE` / `0x40FBD3` / `0x40FBF5` | 存 `5D888h` / `esi=0FFFFFFF6h` / 写 `5D884h` / `cur=target` | — | -10 = id 队列换脚本 |
| 31 | `0x410DA4` / `0x41151B` | `mov dword ptr [ebx+5D884h], 0FFFFFFF5h` | — | -11 = 存档版本装载 |
| 32 | `0x40DF44` / `0x40DF69` | `mov [esi+5D884h],-1` / `mov [esi+5D880h],0` | — | 构造期：根帧 parent=-1、cur=0 |
| 33 | `0x428A83` / `0x428A89` | `push edi` / `call sub_40EA00` / `inc edi` / `cmp edi,28h` / `jl` | — | `0x9` 一次释放**全部 40 槽**（同一数组口径） |

### 4. 步进 / 恢复位置

| # | EA | 指令原文（摘） | 偏移 | 证明了什么 |
|---|---|---|---|---|
| 34 | `0x411A7E` / `0x411A8C` | `mov edx,[..+5D8F4h]` / `add edx,edx` / `add edx,edx` / `add [eax+5D898h],edx` | `+0x60`→`+0x04` | 派发后按**当前 cur** 的长度推进 PC（C10） |
| 35 | `0x41347D` / `0x413487`；`0x413885` / `0x41388F` | 同形（`0x413888` 之后的"忽略本命令继续"出口也用它） | 同上 | 三条派发路径同构 ⇒ 不是单点巧合 |
| 36 | `0x412A56..0x412AC8` | 读 `Engine+778B0h[cur]`；`PC = body + 4*idx`；写 `+0x60=0` | — | 另一条"恢复待办位置"的机制（辅助数组，不是 `+0x5C`） |
| 37 | `0x4194B6` / `0x4194B9` / `0x4194D6` / `0x4194E8` | `(PC-body)>>2` 写 `+0x5C`；随后 `table1[i]` → `PC = body + 4*值` | `+0x5C` | 同形的"保存位置"出现在另一控制转移里（仍无读者） |

### 5. 名字解析

| # | EA | 指令原文（摘） | 偏移 | 证明了什么 |
|---|---|---|---|---|
| 38 | `0x4083DF` / `0x408400` | `call sub_454FA0`（两边各一次） | — | 引擎把 id 解成名字用的就是 `sub_454FA0`（= MarkerSampler 复刻的那个） |
| 39 | `0x4083CC` / `0x4083F2` | `cmp eax,[esi+edx*8+5D8D0h]` / `mov edx,[..+5D8D0h]` | `+0x3C` | id 是脚本身份，且能被比较 |
| 40 | `0x4070A7` | `mov edx,[esi+ecx*8+5D8D0h]` → `sub_48F000(Engine+4E3ACh, id)` | `+0x3C` | id 的另一个读者（场景/脚本判定） |
| 41 | `0x412A99` | `mov edx,[eax+5D8D0h]` / `mov [esi+69278h],edx` | `+0x3C` | 当前帧 id 被抄进 Engine 级标量 |

### 6. 存档写出器（字段的持久性旁证）

| # | EA | 指令原文（摘） | 偏移 | 证明了什么 |
|---|---|---|---|---|
| 42 | `0x40D2EA` / `0x40D5AB` | `mov eax,[ebx+edx*8+5D8CCh]` | `+0x38` | parent 进存档 |
| 43 | `0x40D360` | `mov edx,[eax]` / `mov ecx,[eax+4]` / `mov [edi-4],edx` / `mov [edi],ecx` | `+0x38`,`+0x3C` | 存档逐槽写 parent / id |
| 44 | `0x40D3BB` / `0x40D437` | `mov ecx,[eax+24h]`（`+0x5C`）/ `mov [eax+20h]`（`+0x58`）+ 扫 `[eax+10h]`/`[eax+0Ch]` | `+0x58`,`+0x5C` | 两个位置字段按表下标持久化（⇒ 是引擎状态） |

---

## 建议 API

### 语义定义（先定这三条，代码才有唯一含义）

1. **活动帧** = `record(slot)+0x00 != 0` 且该地址已提交（C3）。**不是** `0..cur`，也**不是**"id 非 -1"。
2. **top** = `slot == cur`（C8：引擎没有独立的栈顶变量；`cur` 是唯一执行点）。
   ★ 两种"没有 top"的情形要分清：① `cur` 的**值**读不到 / 越界 ⇒ **整表不可用**（err）；
   ② `cur` 的值读得到、但它那一格不是活动帧（`body==0`，例如根还没装载 / 被 `0x7` 释放）⇒
   列表照给，**`top` 全 false**（⛔ 不挑一个"最高的"冒充）。
3. **顺序 = top-first**：从 `cur` 沿 `+0x38` 往上走（`cur → parent → …`，遇到 `-1` / `-10` / `-11` /
   越界 / 重复 / 非活动就停），这一段按**由内到外**排；其余"已装载但不在当前链"的槽按**槽号降序**附后。
   理由：① HUD 的 headline 是"当前是谁"，`[0]` 就应该是它（与 `game.script` 同一答案）；
   ② 调用栈的常规可读顺序就是 inner-first（报错栈也是这个方向）；③ 不需要再排序 / 反查。
   ⛔ 不选 bottom-first：那会让"取栈顶"变成 `arr[arr.length-1]`，且要额外排序。

### 最小可落地版（**推荐先发这个**；不依赖 parent 的**链排序**）

```cpp
// src/ui/script.h（语言无关；⛔ 不 include quickjs.h / 不 include 任何 guest 头）
struct UiFrameInfo {
    std::uint32_t slot = 0;         // 0..39
    std::uint32_t script_id = 0;    // 记录 +0x3C 原样（含 -1；-1 = 已释放）
    std::uint32_t parent = 0;       // 记录 +0x38 原样（0xFFFFFFFF = 根；0xFFFFFFF5/6 = 特殊装载）
    std::string script;             // 名字；降级时 = "id:0x%08X"
    bool name_degraded = false;
    bool top = false;               // slot == cur
};

class UiFrameSource {
public:
    virtual ~UiFrameSource() = default;
    // 读 guest 的帧区（40 槽）。err 非空 ⇒ 这一刻整表不可用（脚本侧看到 undefined）。
    // err 为空时：活动帧列表；cur 是活动帧 ⇒ 它排第一（top=true），其余按槽号降序。
    virtual std::vector<UiFrameInfo> localFrames(std::string* err) = 0;
};
```

- `UiFrameReadings` 加 `UiFrameSource* frames = nullptr;`（非 owning；只在 render 期间有效）。
- `game.local_frames()` = **函数**（不是 getter）：`frames == nullptr` ⇒ `undefined`；
  `localFrames(&err)` 且 `err` 非空 ⇒ `undefined`（并**一次性** stderr `[frames] why=…`）；
  否则返回 `[{slot, script_id, parent, script, name_degraded, top}, …]`。
  顺序语义同上（top-first）。
- 最小版的**排序**：`cur` 是活动帧 ⇒ 它排第一（`top=true`），其余活动槽按**槽号降序**；
  `cur` 不是活动帧 ⇒ 没有 top，全部按槽号降序。**不走 parent 链**（`parent` 字段照给，消费者可自己走）。
- 最小版**不做**：链排序 / 深度 / `in_chain`。

### 完整版（已证字段全用上）

```cpp
struct UiFrameInfo {
    std::uint32_t slot = 0;
    std::uint32_t script_id = 0;
    std::uint32_t parent = 0;        // 原样；见 C12 的三种负值
    std::uint32_t depth = 0;         // 距 cur 的 parent 跳数（不在链上 = UINT32_MAX）
    bool in_chain = false;           // 能从 cur 沿 +0x38 走到
    std::string script;
    bool name_degraded = false;
    bool top = false;
};
```

- 排序：链由内到外 → 其余按槽号降序；`top` 恒等于 `slot == cur`。
- 可选字段（消费者可忽略）：`length`（`+0x60`）、`pc`（`+0x04`）、`body`（`+0x00`）、
  `loaded_but_stale`（活动但不在链上，= `!in_chain`）。
- 若将来 U1 取证了 `+0x5C` 的消费者，再加 `ret_pos`；**现在不加**。

### 读之前必须检查什么（安全读取）

按顺序（任何一步失败 ⇒ 返回空 + why，⛔ 不猜）：

1. `engine = rd32(0x55E1BC)` 且 `engine != 0`（写者只有发布 / 退出置 0）⇒ 否则 `engine-unpublished`。
2. `engine + 0xAABC8 <= 0x100000000`（C1 的对象尺寸；`uint64` 算术，防 4 GiB 回绕）⇒ 否则 `engine-base-overflow`。
3. `committed(engine + 0x5D880, 4)` 且 `cur < 40` ⇒ 否则 `cur-uncommitted` / `cur-out-of-range`。
4. 先判 `committed(engine + 0x5D894, 0x78)`（第 0 条记录）为假 ⇒ `frame-region-uncommitted`（整表不可用）；
   再逐槽：`rec = engine + 0x5D894 + 0x78*slot`；`committed(rec, 0x78)` 为假 ⇒ **跳过该槽**（无法证明活动）。
5. `body = rd32(rec)`；`body == 0 || !committed(body, 1)` ⇒ 非活动，跳过。
6. `id = rd32(rec+0x3C)`；`committed(rec+0x3C, 4)` 为假 ⇒ 跳过（半初始化）。
7. 可选（"已就绪"判据，只影响 `loaded_but_stale` 标记，不影响是否入表）：
   `pc = rd32(rec+4)`；`pc != 0 && (pc < body || !committed(pc,4) || pc - body > kScriptBodyMaxBytes)` ⇒ 标"未就绪"。
8. 全表读完后再读一次 `engine`：变了（退出 / 重开）⇒ 丢弃结果，`engine-changed`。

**读不到时返回什么**：
- 整表不可用（1/2/3/4/8 步失败）⇒ **空 vector + `err`**；JS 侧 `undefined`。
- 单槽不可读 / 非活动 ⇒ **不进表**（等价于空槽）；这是保守判定，不是猜。
- 名字降级 ⇒ 仍进表，`script = "id:0x%08X"`、`name_degraded = true`（与 `script` 行同一降级形态）。
- `cur` 的值可读但那一格不是活动帧 ⇒ 列表照给（其余活动槽），`top` 全 false，**不设 err**。
- 整张表一个活动槽都没有（理论不可能：根帧至少应活动）⇒ `err = "no-active-frame"` ⇒ `undefined`。

`why` 的静态字面量（扩既有枚举，⛔ 不新造驼峰名）：
`engine-unpublished` · `engine-base-overflow` · `cur-uncommitted` · `cur-out-of-range` ·
`frame-region-uncommitted` · `no-active-frame` · `engine-changed` · `frames-unavailable`。

### 名字：复用 `MarkerSampler`（不改 `engine_marker.*`）

- 每个活动槽：`MarkerSample s; s.valid = true; s.id = id; sampler->resolveName(mem, &s);`
  ⇒ `s.name` / `s.name_degraded` / `s.name_table_sig`。**`resolveName` 不看 `cur`**（C13）。
- 缓存：每槽 `{ id, sig, name, degraded, valid }`；`sig = sampler->nameTableSignature(mem, id)`；
  `id` 或 `sig` 变了才重解析（TOC 从"未装载"→"已装载"时名字会自动刷新，与 marker 的既有口径一致）。
- 多槽够不够：**够**。`nameTableSignature` 是按 `raw_id` 取的（根：count+name_base；区号臂：
  卷表项 + 子索引 count/name_base），每个 id 一份指纹即可；缓存 key = `(slot, id, sig)`。
- 降级链：`id < 0`（→ `id:0x…`）· TOC 未装载 / 越界 · 名字条目未提交 / 非法 CP932 / 空 —— 全部只降级名字，
  **不影响 id 与列表本身**（⛔ 不把整表判成 undefined）。
- ⚠ `MarkerSampler` 的 `warnOnce` / 计数器是**全局的**：列表扫描会共用去重位。
  这不影响正确性（只会少打一条告警）；若要分开，给帧源一个**独立** `MarkerSampler` 实例。

### Machine / AotHost 怎么填

```cpp
// runtime/engine_frames.h（新增；runtime 层，保持 ui/script.h 语言无关）
class EngineFrameSource final : public ui::UiFrameSource {
public:
    void bind(const GuestMemory* mem, MarkerSampler* sampler) { mem_ = mem; sampler_ = sampler; }
    std::vector<ui::UiFrameInfo> localFrames(std::string* err) override;
private:
    const GuestMemory* mem_ = nullptr;
    MarkerSampler* sampler_ = nullptr;                 // 只用于 resolveName / nameTableSignature
    std::array<NameCacheEntry, kFrameCount> cache_{};
    bool warned_ = false;                              // `[frames] why=…` 一次性
};
```

- **`Machine`**：`mem_` + `marker_sampler_`（`machine.h:490`）。在 `initBare()/init()` 里
  `frame_source_.bind(&mem_, &marker_sampler_);`；新增 `ui::UiFrameSource* frameSource()`；
  装配点（`Machine::run` 之前，overlay 打开时）调一次 `sink->setUiFrameSource(frameSource())`。
- **`AotHost`**：`mem_` + `control_marker_`（`aot_host.h:504`，`control_snapshot.cpp` 已经用它做 script 采样）。
  同样 `bind(&mem_, &control_marker_)`，在 `--overlay` 打开时 `setUiFrameSource(&frame_source_)`。
- **`PresentSink`** 加一个默认空实现：`virtual void setUiFrameSource(ui::UiFrameSource* /*src*/) {}`
  （前向声明 `namespace lift::ui { class UiFrameSource; }`，⛔ 不 include `ui/script.h` 进 `present.h`）；
  `SdlWindow` 存指针，`drawOverlay()` 里 `readings.frames = impl_->ui_frame_source;`。
- **线程**：`localFrames()` 只在 render（present，模拟线程）里被调；⛔ 不要从控制线程调
  （控制层的快照口径是"只序列化值"，见 `control_snapshot.h` 头注）。
- **`game.local_frames` 的注册**：`JS_DefinePropertyValueStr(ctx, game, "local_frames",
  JS_NewCFunction(...), 0)` —— 不可写 / 不可枚举 / 不可配置（strict 下赋值 ⇒ TypeError）。
  这是"方法"而不是读数，是全对象**唯一**不用 getter 的属性；`script` / `fps` / `mouse` / `worst` 保持 getter。

### 若 parent 偏移未取证（假设情形）

最小版仍然成立：**只给 `top = (slot == cur)` + 活动槽列表（按槽号降序）**，`parent` 字段恒填 `0` 或干脆
从结构里删掉（JS 侧就没有 `parent` 键）。代价：HUD 不能画调用链，但"栈顶脚本名"照旧可给。
（本轮已证 `parent = +0x38`，所以这只是**降级预案**，不是当前建议。）

---

## 实现边界

- **B 的 overlay 迁移不依赖本机制**：`hud.js` 现在用 `game.script`（当前帧）就能画；`local_frames` 是**加法**，
  不改变任何既有判据、不改 `script` 行口径、不改 `--dump-frame` / 像素判据。
- **能马上实现（无阻塞）**：
  1. `runtime/engine_frames.{h,cpp}`：`EngineFrameSource`（C1–C4、C9、C13 全已证）。
  2. 纯 CTest：合成 Engine（`tests/input_marker.cpp` 的 `boot` / `setRecord` / `installRootTable` /
     `putName` 形状）足够覆盖活动判据、空洞、跨槽、降级、越界、未提交、parent 环。
  3. `game.local_frames()` 绑定 + `UiFrameReadings::frames` + `PresentSink::setUiFrameSource`（纯宿主接线）。
- **要等取证 / 另开单的**：
  1. `+0x5C` 的消费者（U1）—— 不取证就不暴露"返回位置"。
  2. `+0x64..0x6C` / 三组表的元素语义（U2/U3）—— 不进 API。
  3. 真机读数（控制层 `sample` 里的 `frames` 块）—— 那是 `ControlSnapshot` 的扩展，另开需求单。
  4. `0x6`/`0x8` 的真实脚本用量（U4）—— 数据层普查，另开单。
- **性能**：按需调用；每槽 ~6 个 dword 读 + 指纹（2–5 dword）⇒ 40 槽 < 300 次 guest 读，
  同线程内存读，数量级可忽略；名字只在 `(id,sig)` 变化时解析（CP932 + 两次 `std::string`）。
  若 HUD 每帧调，缓存命中后仍要付 40×(committed + 6 dword) —— 若要更省，可加"列表指纹"
  （`(cur, 活动槽位图, 每槽 id)` 的 hash）跳过重建；**先不做**，等实测。
- **不变量**：`local_frames` ⛔ 不写 guest 内存、⛔ 不改变 `MarkerSampler` 的采样点/
  计数器口径（要分开就给它独立实例）、⛔ 不影响 `script` 行的边沿去重。

---

## 待核

1. **U1 `+0x5C` 的读者**：本镜像零读点。是否在别的模块 / 别的构建 / 已被内联掉？
   可查：`rg -n '5D8F0'`（含 `seg002` 数据）、`disasm-fn impact sub_41A820 --up`。
2. **U2 `+0x64/+0x68/+0x6C`**：8 B/元素的堆表，装载分配 + `0xFF` 填充，释放删除，**无读点**。
   是给别的子系统用（指针被抄到别处）还是死字段？
3. **U3 三组 `(count,ptr)` 表的元素语义**：`+0x40/+0x44`（`+0x58` 的宿主）、`+0x48/+0x4C`（控制转移目标）、
   `+0x50/+0x54`（`+0x54` 被 `0x40F9E0` 用来填 `Engine+5EEA4` 辅助数组）—— 元素是 label 位置 / 字符串偏移 /
   键，未逐条取证。
4. **U4 真实脚本的槽使用**：`0x6`/`0x8` 是否真的造成"非 LIFO"与"陈旧槽"。
   可查：`dist/views/data/*.txt` 里 `0x6`/`0x8` 的操作数分布（数据层，不是引擎事实）。
5. **U5 "栈顶"的产品口径**：`top = cur`（本报告推荐）还是"最高活动槽"？这是用户裁决，不是取证。
6. **U7 真 boot 的帧区提交状态**：40 条记录 × `0x78` 是否全在已提交页；只有真机能读。
7. **U8 `Engine+0x5EB54`**：装载器的 a3 / FileDB 查找键；写者未找到 ⇒ 不进 API。
8. **真机读数（全部是 `<待核>`，⛔ 不编数字）**：
   - `sample` 里的 `script.cur` 与 `local_frames()[0].slot` 是否恒等；
   - 首次 `local_frames()` 的帧内耗时（预算面）；
   - 名字降级在真实 TOC 下是否出现（`id:0x…`）。

### 验证计划（可执行命令）

**A. 纯 CTest（合成 Engine；能证 C1–C4/C9/C13 与全部安全分支）**

新增 `tests/ui_frames.cpp`（注册 `ui-frames`，顶层无条件 ⇒ pure 档；链接 `emulator-lift-runtime`）：

```bash
cmake --build apps/emulator-lift/out --target lift-ui-frames
ctest --test-dir apps/emulator-lift/out -R '^ui-frames$' --output-on-failure
```

用例（全部用 `commitZeroed` + `setRecord` 形状，不依赖游戏安装）：

1. **活动判据 / 空洞**：槽 0(body!=0,id=0,parent=-1)、1(body!=0,id=3,parent=0)、
   2(body=0) 、3(body!=0,id=5,parent=1)，`cur=3` ⇒ 列表 = 3,1,0（2 被跳过）。
2. **任意槽名字**：槽 1 与槽 3 的 `script` 各自解析对（证 C13 复用 `resolveName`）。
3. **top**：只有 `slot==cur` 的 `top=true`。
4. **降级**：`+0x40C=0`（TOC 未装载）⇒ 所有 `script == "id:0x%08X"`、`name_degraded=true`、列表仍非空。
5. **cur 不可读**：把 Engine 发布到只提交 `0x5D880` 之前的小块 ⇒ 空 + `cur-uncommitted`。
6. **记录未提交**：Engine 基址贴近已提交区末尾 ⇒ 跳过该槽、不崩、不猜。
7. **parent 环**：`parent(1)=3, parent(3)=1` ⇒ 链在重复处停，不死循环。
8. **释放态**：`id=-1, body=0, parent=-1, +0x58/=+0x5C=-1` ⇒ 不进表。
9. **JS 面**：`game.local_frames()` 返回数组、键齐全；`frames=nullptr` ⇒ `undefined`；
   宿主返回 err ⇒ `undefined`；`strict` 下给 `game.local_frames` 赋值 ⇒ TypeError。

预期读数：全部 `LIFT_CHECK` 绿；不依赖任何真机数字。

**B. 真 boot（能证"同一份 guest 状态在真引擎里读得出"；需要安装树 + 执行源）**

```bash
apps/emulator-lift/out/emulator-lift-aot \
  --game-dir links/install --present window --present-hidden \
  --overlay --max-frames 2
```

- 预期：`exit 0`；开启 `LIFT_LOG_FRAMES=1`（**建议新增的 opt-in 一次性日志，本轮未实现**）时，
  stderr 出现一行 `[frames] cur=<n> slots=[<slot>:<id>:<name> …]`；不出现周期性行。
- 与 `game.script` 对账：`[frames]` 的第一项 = `game.script` 的名字 `<待核>`（本轮无真机读数，⛔ 不编）。

**C. 控制层（最强的真机读数；需要先扩 `ControlSnapshot`，另开单）**

```bash
apps/emulator-lift/out/emulator-lift-aot --game-dir links/install \
  --control-root .tmp/ctl --control-port 0 --present window --present-hidden
node tools/emu-ctl.mjs sample --json      # 读 data.sample.frames[*] 与 data.sample.script
```

- 预期（实现后）：`data.sample.frames[0].slot == data.sample.script.cur`、
  `data.sample.frames[0].script == data.sample.script.name`、`frames[0].top == true`；
  出现 `parent` 为 `-1/-10/-11` 的项时，`frames` 里保留原值 `<待核>`（本轮无读数）。
