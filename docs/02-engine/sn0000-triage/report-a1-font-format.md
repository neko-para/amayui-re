# 残差 triage 报告 — 簇 `a1-font-format`（SN0000 路径内）

- 条数：**20**（`a1-font-format.md` 全量，逐条处理，无合并判定）
- 判定计数：`narrowed` **13** · `real-gap` **4** · `duplicate` **1** · `covered` **1** · `needs-machine` **1**
- 覆盖的规则：`CORE-OPS-RENDER-TEXT-204`（15）· `CORE-OPS-TEXT-RECORD-REGISTER-1D2`（2）· `CORE-OPS-TEXT-UNEQUAL-195`（1）· `CORE-OPS-TEXT-RUN-HEADER-7A`（1）· `CORE-OPS-TEXT-REDRAW-20A`（1）
- 取证环境：`pnpm tools disasm-at` / `disasm-fn`（`.lst` 只读）· `data/spec/{chapters,audit.json,bindings.json}` · `.cache/footprint/sn0000.jsonl`（仅作"跑到过"的观察）。**未读** `apps/emulator*`、**未读** `docs/`。仓库其它文件未改。

## 判定口径（本报告用的一套，先说明再引用）

| 判定 | 含义（本报告） |
|---|---|
| `covered` | 该残留的**问题**已被**同一规则正文**回答（引原句）；残留可删 |
| `narrowed` | 正文回答了一部分 / 本轮把范围压到更小；`what`+`next` 要重写 |
| `real-gap` | 正文没有这条合同（或明说"不规定"），而本轮在 `.lst` 里取到了能写成合同的机器事实 ⇒ **spec 缺口**，处置 = 补正文 |
| `needs-machine` | 只能由目标平台 / 运行观测回答（静态读完后仍不可判） |
| `duplicate` | 与另一条残留（或另一条规则的 discarded 项）同一义务，应合并 |

★ 说明：`covered`/`narrowed` 按任务要求引同规则正文；**本轮新取到、但正文没有的事实一律记 `real-gap`**（否则会把"我们读到了"当成"spec 说过了"）。
★ `path` 桶的局限已计入：`.cache/footprint/sn0000.jsonl` 只到 opcode 级（`0x204` count=6 / `0x7a` count=1 / `0x1d2` count=2 / `0x195` count=2000 / `0x20a` count=1），**不能**证明某条分支执行过 ⇒ 本报告没有一条判定只靠"在 path 里"。

## 总表（每残差一行）

| # | id | 判定 | 依据（EA 或正文行，≤60 字） | 处置 |
|---|---|---|---|---|
| 1 | RES-RENDER-TEXT-FONT-SELECT-WHY | narrowed | 正文 60-ops-render.md:111 判据已定；55-config.md:138 该键为条件键 | 改 `what` 为 discarded（"为什么"不可观察） |
| 2 | RES-RENDER-TEXT-FONT-FALLBACK | real-gap | `0x46F43F cmp di,0x100` / `0x46F468 [ebx+3147Ch]←1` | 补正文（合同句草案见下） |
| 3 | RES-RENDER-TEXT-FONT-SHARED-METRICS | narrowed | `0x46F7E3 cmp [ebx+35598h],0`（读点）；置 0/1 写点 ≥20 | 改写 `what`+`next` |
| 4 | RES-RENDER-TEXT-SPECIAL | real-gap | `0x46F8E4`/`0x46F919 call sub_4A3980`（F042 两次） | 补正文（次数+参数来源） |
| 5 | RES-RENDER-TEXT-LOCK | narrowed | `0x46F324`/`0x46F32E jz loc_46FA46` 与加锁失败同口 | 改写 `what` |
| 6 | RES-RENDER-TEXT-FONT-INSTANCE | narrowed | `0x4233D9`/`0x423466` 同一 `ecx=engine+14D30` | 补正文一句（0x205 同实例） |
| 7 | RES-RENDER-TEXT-GLYPH-RASTER | duplicate | 与 WRITE-BITS / ALIGN-BACKEND 两条 discarded 重叠 | 合并为一条 discarded |
| 8 | RES-RENDER-TEXT-SURFACE-FORMAT | narrowed | `0x41F3BB..0x41F3D3` 字节反转；正文 108 与 text:102 矛盾 | 改 `what` + 改正文那句 |
| 9 | RES-RENDER-TEXT-PUA | narrowed | `0x46F881..0x46F92C` 窗口内无 SelectObject | 并入 SPECIAL |
| 10 | RES-RENDER-TEXT-ENCODING | narrowed | 正文 98 已给十进制文本口径；0x205 条已写死 | 改 `what`（删该半） |
| 11 | RES-RENDER-TEXT-ALIGN | narrowed | 正文 190 原问；SelectObject 全语料 190 处，相关 3 处已定位 | 改 `what`+`next` |
| 12 | RES-RENDER-TEXT-ALIGN-REFERENCE | needs-machine | 正文 191 明说平台量度未取证 | 保持（`next` 加样本口径） |
| 13 | RES-RENDER-TEXT-ADVANCE | narrowed | `0x46F7EC`/`0x46F7FF`/`0x46F826` 三分支已定位 | 改 `what`+`next` |
| 14 | RES-RENDER-TEXT-PRESENT | real-gap | 本轮没取到（全语料无 `call Present/EndScene` 匹配） | `next` 改成可执行判据 |
| 15 | RES-RENDER-TEXT-PROJECTION | narrowed | `0x469EB5` 等 5 点落在借用区 `0x469719`/`0x46AE08` 内 | 移出本条 → 开需求单 |
| 16 | RES-TEXT-RECORD-REGISTER-CONSUMER | narrowed | `0x45EFEE` 读默认列表号 `[esi+4CCh]`；构造 `0x465632` 置 1 | 改 `what`+`next` |
| 17 | RES-TEXT-RECORD-REGISTER-DIRTY | real-gap | 置 1 只有 `0x45D793`/`0x45EE3F` 两处 | 补正文 |
| 18 | RES-OPS-195-COMPARE-SUBROUTINE | covered | text:384「长度不同 ⇒ 1」；`0x401594 or eax,-1` 证实 | 删残差 |
| 19 | RES-TEXT-RUN-HEADER-CONSUMER | narrowed | 存放格 = `[[层+30h]−14h]`（`0x45A92D`/`0x45A936`） | 改 `next`（给出读者搜索判据） |
| 20 | RES-TEXT-REDRAW-20A-DELAY-CELL | narrowed | text:529 置位者已枚举；`0x423668` 之后无写该格 | 改 `what` / 可删（并入 1CE） |

---

## 展开（covered / narrowed / real-gap）

### 18 · RES-OPS-195-COMPARE-SUBROUTINE — covered
- 正文原句：`data/spec/chapters/60-ops-text.md:384`「长度不同 ⇒ **1**（先比长度、再逐字节比；只取"是否非 0"，所以观察不到字典序方向）」。
- 为什么回答了它：本残留的唯一开口是"长度不同时的返回符号没核"。逐字读完两级子例程：`0x401066 sar eax,1Fh` + `0x401069 or eax,1`（把字节差**归一成 ±1**，相等走 `0x401070 xor eax,eax`）；长度不等由调用方 `sub_401540` 自己给 `0x401594 or eax,0FFFFFFFFh`（短）或 `0x40159E xor eax,eax` + `0x4015A2 setnz al`（长）⇒ 恒 ±1，非 0。
- `0x42D093 neg/sbb/neg` 把非 0 归一成 1 ⇒ `0x195` 在长度不同时**恒 1**，与正文一致。
- 处置：**删 residual**（`audit.json` 与正文 60-ops-text.md 的残差表两处都删；`falsifier` 已覆盖该方向）。
- `<待核>`：`sub_401540` 的"短/长"分支是否还能被别的调用方以别的长度口径触发（4 个调用方，未逐个读）。

### 2 · RES-RENDER-TEXT-FONT-FALLBACK — real-gap
- 正文现状：60-ops-render.md:139 只说「另一个是**回退字体**的状态」；触发条件、临时字体的建 / 删都没有。
- 本轮取到：`0x46F43F cmp di,dx`（`edx=0x100`）/ `0x46F442 jnb loc_46F474` ⇒ **字形码 < 0x100 才走建**；`0x46F460 call sub_458A30`（喂 `ebx+4D0h` 的 0x3C 字节描述）；`0x46F468 mov dword ptr [ebx+3147Ch],1`。释放支：`0x46F421 call SelectObject`（把旧句柄选回）→ `0x46F42B call DeleteObject`（删临时字体）→ `0x46F434 mov [ebx+3147Ch],esi`（`esi=0`）。
- 合同句草案（可直插 60-ops-render.md 的「引擎数据」后）：「**回退字体**：逐字写入时若**字形码 < 0x100**，用字体对象里那份 0x3C 字节的字体描述建一个**临时字体**、选进绘制表面并把回退状态置 1；字形码 ≥ 0x100 时**只释放**（把进入时的句柄选回、删临时字体、状态清 0）。临时字体不是本条第二个布尔开关之外的任何状态，**不写进描述 / 不持久**。」
- 处置：补正文 + 把 `what` 改成"已定"、`next` 改成"临时字体描述（`+0x4D0`）与 A/B 支的关系"。
- `<待核>`：`0x458A30` 是否是"建句柄 + SelectObject"（本窗口只看到调用）；`0x3147Ch` 与 `0x35598h` 是**两格**（回退字体 vs 按格宽推进），⛔ 不许合并。

### 4 · RES-RENDER-TEXT-SPECIAL — real-gap
- 正文现状：60-ops-render.md:138「矩形操作本版不规定」；`what` 里的"几次、覆盖哪块矩形"没有任何 EA。
- 本轮取到：「几次」= **F043 一次**（`0x46F8E4 call sub_4A3980`）、**F042 两次**（`0x46F8E9 cmp word ptr [ebp+var_54],0F042h` / `0x46F8F2 jnz loc_46F91E` 决定是否再走 `0x46F919 call sub_4A3980`）。矩形参数来源：`gm.gmptGlyphOrigin.y`（`0x46F893`）、`gm.gmBlackBoxY`（`0x46F896`）、`gm.gmBlackBoxX`（`0x46F8AB`）、**投影偏移横** `[ebx+568h]`（`0x46F899`、`0x46F904`）、推进量 `[ebp+arg_4]`（`0x46F8FA`）。解锁 `0x46F88B call sub_4A3910`、重锁 `0x46F92C call sub_4A3890`。
- 合同句草案：「特殊字形路径的**矩形**由该字形的字框（origin / blackbox 两个分量）加**投影偏移横**与当前推进量装配；**F043 做一次、F042 做两次**（第二次的起点再叠加一次偏移横与推进量）。」
- 处置：补正文（替换"不规定"），`RES-RENDER-TEXT-SPECIAL` 与 `RES-RENDER-TEXT-PUA` 合并成一条。
- `<待核>`：`sub_4A3980` 的语义（"矩形操作"是拷贝 / 填充 / 还是别的）**没读到**，合同句只能写到"参数来源 + 次数"。

### 17 · RES-TEXT-RECORD-REGISTER-DIRTY — real-gap
- 正文现状：60-ops-text.md:159「本版只规定"非 0 ⇒ 置第 0 位、并清 0"」；写者没写。
- 本轮取到：置 1 的写点**全语料只有两处**：`0x45D793 mov dword ptr [ebx+esi*4+0D44h],1` 与 `0x45EE3F mov dword ptr [ebx+edi*4+0D44h],1`，两处都紧跟在**行记录收尾**（`[+0D34h]/[+0D38h]/+0D6C/+0D70` 那组行计数）之后；清 0 有三处：`0x45EF02`、`0x45F002`、`0x45F0E9`（三个追加变体）。
- 判据补充：追加器 `0x45EFF4 cmp [esi+eax*4+0D44h],ebx` → `0x45EFFD..0x45F002` 清 0 并选 `0x20000001`（位 29|位 0），否则 `0x20000000` ⇒ 正文写的那一步**逐字对上**。
- 合同句草案：「**脏槽**（按记录下标、基址 +0xD44）的唯一写者是**行记录收尾**：该下标上一条记录结束了一段 ⇒ 置 1，下一条 `0x1d2` 追加到同一下标时把它变成"新段起点"并清 0。」
- 处置：补正文 + 改 `next` 为"位 0 的组语义是否只由行边界划"（该问题现在归 `RES-TEXT-RECORD-QUERY-GROUP`，可合并）。
- `<待核>`：`0x45D793` 与 `0x45EE3F` 分属两个函数（未读它们各自的上层触发条件）；`0x465643 mov [ebx+0D44h],esi` 是**另一个类**的同偏移（无下标缩放），与本残差无关。

### 8 · RES-RENDER-TEXT-SURFACE-FORMAT — narrowed（★ 发现正文自相矛盾）
- 正文原句：60-ops-render.md:108「脚本侧写进来的是 `0xRRGGBB`；引擎存放时按平台颜色字节序（**低字节为蓝**、最高字节不用）」；同书另一条规则 60-ops-text.md:102「脚本给 `0x112233`，引擎格里是 `0x332211`」（= 三字节**反转**）。
- 本轮取到：`0x76` 的写点逐字是反转 —— `0x41F3BB movzx edx,al` / `0x41F3BE movzx eax,ah` / `0x41F3C1 shl edx,8` / `0x41F3C4 add edx,eax` / `0x41F3C6 movzx eax,[ebp+var_4+2]` / `0x41F3CA shl edx,8` / `0x41F3D3 add edx,eax` ⇒ 存放 = `b0<<16 | b1<<8 | b2`，**低字节 = 输入的高字节**。
- 矛盾：若输入是 `0xRRGGBB`，存放的低字节就是 **R**，不是"蓝"；两句不能同时成立（除非"脚本侧 `0xRRGGBB`"这个前提本身错）。
- 处置：① 删 `what` 里已过时的"取证记录…这一环本轮没有复核"；② 正文 108 那句改成"按平台字节序**反转**存放（低字节 = 脚本侧最高位字节）"，⛔ 不再写"低字节为蓝"，除非补一条像素 / 真机证据；③ `next` 只留"32 位表面通道归属 + 与真机像素对照"。
- `<待核>`：32 位支的通道归属（R 落在哪个字节）仍取决于表面格式；本轮**没有**取到该格式字段。

### 1 · RES-RENDER-TEXT-FONT-SELECT-WHY — narrowed
- 正文原句：60-ops-render.md:111「若**显示记录**里的**虚拟全屏类型**格…等于 **2**，把该对象物化出的字体句柄**选进**绘制表面」；配置侧：55-config.md:138「`VirtualFullScreenType` 只在 `VirtualFullScreen` 非 0 时**出现**」。⇒ 判据与取值来源都已定，残差只剩"**为什么**只有 == 2 才换字体"。
- 为什么可结：设计意图**不是可观察行为**（按 `amayui-spec` §2：spec 只写"给定输入引擎做什么"），把它留在 `unknown` 会让"未取证"与"不需要取证"混在一起。
- 处置：`what` 改成「该格 = 配置 `display:VirtualFullScreenType`（条件键）；`== 2` 才选字体是引擎的选择，**为什么**属设计意图、不可观察 ⇒ 明确舍弃（`kind: discarded`，`next: 无`）」；`next` 删掉"真机对比各档字形表现"（那不是这条判据的证据）。
- `<待核>`：发行态该键的实际取值（本轮足迹只到 opcode 级，`cell` 记录是抽象地址，没取到该格的值）。

### 3 · RES-RENDER-TEXT-FONT-SHARED-METRICS — narrowed
- 正文原句：60-ops-render.md:110「字体对象：把一个内部布尔开关清 0」（本条 = `0x456766 mov dword ptr [esi+35598h],0`）。
- 本轮取到：读点就在绘制循环 `0x46F7E3 cmp dword ptr [ebx+35598h],0` / `0x46F7EA jnz`，**0 ⇒ 推进量 = `gm.gmCellIncX`**（`0x46F7EC movsx ecx,[ebp+gm.gmCellIncX]`）；非 0 ⇒ 走"按算出的格宽"那支（见 #13）。写点 ≥20（0/1 都写：`0x460261`、`0x4620F7`、`0x46325A`、`0x46352B`、`0x464E50`、`0x465182`、`0x4660DF`、`0x467354`、`0x46764D`、`0x469013`、`0x4697DE`、`0x46AF40`、`0x46B1FC`、`0x46BDE8`、`0x46BF48`、`0x46CB4A`、`0x46CC09` …；读点还有 `0x458A47`、`0x459347`）。
- ⇒ 领域身份不是"共享缩放 / 度量复用"，而是**"推进量按算出的格宽排字，而不是按字形步进"**。
- 处置：`what` 改成上述身份 + 写点下界；`next` 改成"这 20+ 写点各自属于哪条路径（本条 0x204 清 0 ⇒ 本条恒走字形步进），确认无第三条语义"。

### 5 · RES-RENDER-TEXT-LOCK — narrowed
- 正文原句：60-ops-render.md:143「**取尺寸 / 取矩形失败**时的行为本版**不规定**（见残差）」。
- 本轮取到：「取尺寸」= `0x46F31B call sub_49ED60`，随后 `0x46F320 cmp [ebp+var_78],0` / `0x46F32A cmp [ebp+var_74],0`，任一为 0 ⇒ `0x46F324`/`0x46F32E jz loc_46FA46`；加锁失败是 `0x46F347 test eax,eax` / `0x46F349 jz loc_46FA46` ⇒ **同一个出口**。`loc_46FA46`（`0x46FA46`）只有栈 cookie 校验 + `retn 18h` ⇒ **不写像素、本函数内不报错**。
- 处置：`what` 改成"取尺寸失败 = 与加锁失败同一早退；本函数不写像素、不发文本"；正文 143 那句从"不规定"改成该出口（错误通道的文本仍归加锁子例程，见 `<待核>`）；「取矩形失败」本轮**没定位**，`next` 保留这一项。
- `<待核>`：`var_78`/`var_74` 是否就是宽 / 高（未读 `sub_49ED60` 的出参语义）；错误文本是否由 `sub_4A3890` 内部发（本函数窗口内无文本写）。

### 6 · RES-RENDER-TEXT-FONT-INSTANCE — narrowed
- 正文原句：60-ops-render.md:104「本条**没有**字体 / 颜色实参。绘制入口自己从引擎**唯一**的字体对象读取」。
- 本轮取到：`0x204` 与 `0x205` 两条 handler 都以 `lea ecx,[esi+14D30h]` 交给**同一个**入口 `sub_456710`（`0x4233D6`/`0x4233D9` 与 `0x423469`/`0x423466`）；而 `sub_456710` 全语料**只有这 2 个调用点** ⇒ 两条文字入口共用同一实例（同一套主色 / 副色 / 绘制模式）。
- 处置：正文补一句"`0x205` 与「离屏文字」是同一个字体对象"；残差缩到"**唯一性**（是否只有一处构造）"。
- `<待核>`：两条 handler 里的 `esi` 是否同一个引擎对象（都从 dispatch 表进来，未逐字核）；`engine+0x14D30` 以外是否还有第二个构造点。

### 9 · RES-RENDER-TEXT-PUA — narrowed
- 正文原句：60-ops-render.md:138「**解锁 → 矩形操作 → 重新加锁**」。
- 本轮取到：该区间 `0x46F881..0x46F92C` 内**没有** SelectObject（全语料 190 处，无一落在 `0x46F880..0x46F930`）⇒ 特殊字形**不改**绘制表面当前字体，用的就是当时选着的那个（`== 2` 时是 `0x456787` 选入的对象字体；否则是表面原来那个）。
- ⇒ "由哪一份字体描述 / 哪一个句柄画出" = **不是**另建句柄；"是否另有额外句柄" = 本路径内没有（回退字体那句柄另算，见 #2）。
- 处置：并入 #4 的 `RES-RENDER-TEXT-SPECIAL`（一条残差同时管"用哪个句柄"与"矩形几次"）；`what` 只保留"PUA 走绘制表面当前字体"。
- `<待核>`：竖排 / B 支描述下 `0xF042/F043` 是否另有句柄（未取证）。

### 10 · RES-RENDER-TEXT-ENCODING — narrowed
- 正文原句：60-ops-render.md:98「数值型操作数给出**十进制文本**（口径同「字符串」的源侧：整数无前导零、无宽度填充；浮点定点 6 位小数）」；`0x205` 条 205–211 行把格式化（符号 / 前导零 / 溢出 / 全角 / 半角）写死。
- ⇒ `what` 里"数值型操作数 → 十进制文本的格式（符号 / 宽度 / 浮点）"这一半**已经被答**，不该再列在残差里。
- 处置：`what` 只留两项（① 字体描述的**字符集格**与字形查找的对应；② `0x8140` 全角空格的量度 / 空白宽度配置分支）；`next` 相应只留两项。
- `<待核>`：字符集格在字体描述里的位置（`0x456D7A` 写"字符集默认"，未与字形查找对齐）。

### 11 · RES-RENDER-TEXT-ALIGN — narrowed
- 正文原句：60-ops-render.md:190「仍未规定的是：**除重建入口那次 SelectObject（把参考字体选进绘制表面）之外**，还有谁在什么时候改绘制表面的当前字体」。
- 本轮取到：SelectObject 全语料 **190 处**；与本条请求相关已定位 3 处 —— `0x456787`（虚拟全屏类型 = 2 ⇒ 选对象字体）、`0x456D94`（参考字体）、`0x46F421`（回退字体释放支把旧句柄选回）。⇒ 问题从"存在性"变成"逐条判是否落在同一个绘制表面（`[esi+454h]`）"。
- 处置：`what` 写"190 处 / 相关 3 处"这个**下界**；`next` 改成"按绘制表面句柄格过滤：只保留 hdc = `[Font+454h]` 的站点"。这是个机械判据，可落地。
- `<待核>`：`[esi+454h]` 是否是唯一的绘制表面句柄格。

### 13 · RES-RENDER-TEXT-ADVANCE — narrowed
- 正文原句：60-ops-render.md:99「**推进量不是无条件等于字形量度**：半角字符（`< 0x100`）、空白宽度配置、以及对位模式都会改变它；本版不规定逐档算法」。
- 本轮取到三分支（都在绘制循环里，`ebx` = 字体对象）：
  1. `0x46F7E3 cmp [ebx+35598h],0` → `0x46F7EC movsx ecx,[ebp+gm.gmCellIncX]`（开关 = 0 ⇒ 推进 = 字形步进）；
  2. `0x46F7F5 cmp eax,1` / `0x46F7FF cmp di,dx`（`0x100`）/ `0x46F804 fld flt_526954` vs `0x46F80C fld1` / `0x46F819 movsx eax,word ptr [ebx+313E8h]` / `0x46F81E fdiv` / `0x46F826 fmul [ebx+355E0h]` / `0x46F827 fadd dbl_51D7F8` / `0x46F82D __ftol2_sse`（⇒ `round(字号 ÷ 半角系数 × 缩放 + 0.5)`）；
  3. `0x46F834 cmp eax,2` / `0x46F862 movsx eax,[ebp+gm.gmCellIncX]`；
  空白宽度配置支在 `0x46F936` 起（`0x46F952 push offset aSetBlankextent` / `0x46F94B cmp [ebx+313D0h],1`）。
- 处置：`what` 改成"三分支已定位（给三条 EA）"；`next` 只剩：`flt_526954` 的数值、`[ebx+355E0h]` 的身份、`0x8140` 特例。
- `<待核>`：`eax`（`0x46F7F5` 处）是哪个"模式"—— 对位模式还是绘制模式（未逐字追该寄存器的来源）。

### 15 · RES-RENDER-TEXT-PROJECTION — narrowed（★ 结论：其它路径**确实**在借用期读它）
- 正文原句：60-ops-render.md:126「**本条**绘制请求只读它…因此本条读取到的值不受那些入口影响」+ 195「**其它直接调用绘制入口的路径**是否会在借用期读到另一个值…见残差」。
- 本轮取到：投影偏移的两格 `[ebx+568h]`/`[ebx+56Ch]` 构造时置 1（`0x4651BD`/`0x4651C3`），**同两格**在样式应用里被写成存档值（`0x469719 mov [ebx+568h],ecx` / `0x46971F mov [ebx+56Ch],eax`），归还点 `0x46AE08`（同一函数 `sub_469260`）。
- 且在**借用区间内**存在其它绘制入口调用点：`0x469EB5`、`0x469F09`、`0x46A63C`、`0x46A663`、`0x46A6AA`（接收者 `0x469EB3 mov ecx,ebx`，且 `0x469E87 cmp dword ptr [ebx+548h],0` 读的就是字体对象的抗锯齿开关 ⇒ 同一个字体对象）。
- ⇒ 残差的"若存在此类路径"变成**存在**；`what` 必须改写（"恒定 (1,1)"只是**本条**的性质）。
- 处置：**移出** `CORE-OPS-RENDER-TEXT-204`，开需求单（标题：「样式应用借用期的投影偏移：哪些绘制路径会读到存档值」，父节点 = 文本 / 样式族）；`what` 改成已证事实 + 未定项（该值是否参与字形格 / 推进量）。
- `<待核>`：`0x469719` 的 `ecx/eax` 是否取自"样式存档的横向 / 纵向偏移"（由绑定 0x4601E8/0x461803 的成对关系推，未读 `0x469719` 之前的取值链）；借用期间是否有 early-return 跳过 `0x46AE08` 的归还。

### 16 · RES-TEXT-RECORD-REGISTER-CONSUMER — narrowed
- 正文原句：60-ops-text.md:156「**列表号** = 默认列表号（**取值与写者本版不规定**，见残差；绘制路径不读它）」；163「这些记录真正的读者见残差」。
- 本轮取到：默认列表号 = `[esi+4CCh]`（`0x45EFEE mov eax,[esi+4CCh]`，槽号 0 ⇒ 用它）；字体对象的构造点 `0x465632 mov [ebx+4CCh],edi`（`edi=1`）⇒ **初值 1**（另一处同偏移写点 `0x4B4EC3` 属另一类对象，`<待核>`）。
- 消费侧：`0x82` 读同一条记录向量（「画一段已排版的文本记录」条 / 60-ops-text.md:107 的字段表），但第 29 位置起的记录它**跳过** ⇒ 本指令写的记录不是 `0x82` 画的。
- 处置：`what` 只留"第 29 位置起的记录由谁消费（`0x1d3` 的查 key 之外是否还有读者）"；默认列表号那半结掉（写进正文 156 行：值 = `[+0x4CC]`，构造置 1）。
- `<待核>`：`0x4B4EC3` 是否第二个写者（对象类型不同，未读）。

### 19 · RES-TEXT-RUN-HEADER-CONSUMER — narrowed
- 正文原句：60-ops-text.md:480「**消费者**（这两个整数怎样进绘制请求）本版不规定（见残差）」。
- 本轮取到：run 头两格的**存放地址**已定 —— `0x45A92D mov [eax-14h],edx` / `0x45A936 mov [eax-10h],ecx`，其中 `eax = [[层对象+30h]]`（`0x45A927`）；槽 0 ⇒ `[+4CCh]` 默认槽（`0x45A91A`）。
- ⇒ `next` 从"读 run 头的排版 / 绘制消费点"改成可执行判据：「找 `mov reg,[obj+30h]` 后接 `[reg-14h]` / `[reg-10h]` 的读点（或对 `+30h` 那份缓冲基址的负偏移访问），逐个判它属排版还是绘制」。
- 处置：`what` 补上存放地址；`next` 换成上面这句。
- `<待核>`：这两个整数在 run 头里的语义（可能是排版起点 / 行距，**没证**，不许猜）。

### 14 · RES-RENDER-TEXT-PRESENT — real-gap
- 本轮没取到（原因）：全语料 `call Present` / `call EndScene` **零匹配**（这两个名字不出现），呈现点应在设备 vtable 的**间接调用**里（`call [reg+off]`）；间接调用点全语料 3100+，为一条残差去筛成本不划算 ⇒ 本轮没做。
- 处置：保留 `kind: unknown`，`next` 改成可执行判据（见下），并把"呈现顺序"与「帧步进」条（`CORE-OPS-RENDER-FRAME-STEP`）挂钩，⛔ 不要在本条重复定义帧提交。
- 新 `next`：「先按设备对象（`0x33f`/渲染器初始化写的那个）找它的 vtable 槽，定位把后备缓冲交出去的那一个间接调用点；再看它相对 `0x20c` 渲染步（及门前判据那一条）的位置 —— 只需给"写入 → 提交 → 呈现"三者的相对顺序，不需要平台细节」。
- `<待核>`：呈现是否发生在渲染步**之内**（这决定"加锁 / 写入与呈现的先后"是否还有自由度）。

### 20 · RES-TEXT-REDRAW-20A-DELAY-CELL — narrowed
- 正文原句：60-ops-text.md:590「随后只测门字第 **30** 位：置起时**再读一次**层号并补画该层**当前格号对应的那一个揭示瓦片**…（该瓦片是否随即被清见残差）」；位 30 的置位者在 `CORE-OPS-TEXT-CELL-REVEAL-1CE` 的 529 行已枚举（`0x1ce` 启用支 + `0x72` 尾段）。
- 本轮取到：`0x20a` 的补画是 `0x42366F mov edx,[esi+692E0h]`（= 当前格号）→ `0x42367C call sub_41BF50` → `0x423688 call sub_45A940`；区间 `0x423668..0x42368D` 内**没有**写 `0x692E0` ⇒ **本条不清该格**（清的只有 `0x1ce` 启用支 `0x4202F7` 与 `0x72` 尾段 `0x41F0A3`）。
- ⇒ 残差的两个问题都已有答案（置位者见 1CE 条；"是否随即被清" = 不清）⇒ 本体只剩"这个格号写进的是不是同一个 `0x692E0`"这一条待核。
- 处置：**删残差**，改在正文 590 行补一句"本条不清当前格号（清者是 `0x1ce` 启用支与 `0x72` 尾段）"；若不删，则 `what` 缩成上述待核项。
- `<待核>`：1CE 的"当前格号"格与 `0x692E0` 是否同一格（1CE 绑定 `0x412B4F`/`0x412B76` 与 `0x4202F7` 未标偏移，本轮没逐字对）。

---

## 本轮没取到的证据（诚实项）

| 缺什么 | 为什么 / 卡在哪 |
|---|---|
| `RES-RENDER-TEXT-PRESENT` 的呈现点 | 语料里没有 `Present`/`EndScene` 名字；需先解设备 vtable 槽（间接调用 3100+ 处，本轮没筛） |
| `sub_4A3980` / `sub_4A3910` / `sub_4A3890` 的语义 | 只按调用形态判"解锁 / 矩形 / 重锁"，**没读函数体**；`#4` 的合同句因此只写到"参数来源 + 次数" |
| 32 位表面通道归属（R/G/B 各在哪个字节） | 已到"alpha 占最高字节 + 24 位色按原序进低位"（`0x46DDB5`/`0x46DDBB`），但**表面格式字段没取到**，也不想凭 D3D 惯例猜 |
| `flt_526954` 的数值 / `[ebx+355E0h]` 的身份 | `#13` 的推进公式里的两个未定项；没读数据段 |
| `0x46F320`/`0x46F32A` 两个出参的名字 | 需读 `sub_49ED60` 的出参语义（`#5`） |
| run 头两格的读者 | 存放地址已定，读者要按"`[obj+30h]` 基址的负偏移"反查；本轮只做了 `+D44h`/`+4CCh` 两个 grep，没做这个 |
| `0x4B4EC3` 是否是第二个默认列表号写者 | 属另一类对象（`sub_4B4E60`），没读 |
| SN0000 里**分支级**执行证据 | 足迹只有 opcode 级（`op`/`gate`/`cell`/`writer` 聚合），`cell` 地址是抽象区（`0x10000000…`），无法把"某格被写"对到"虚拟全屏类型 = 2"这类具体分支 |

## 建议父级先做的 3 条

1. **#18 `RES-OPS-195-COMPARE-SUBROUTINE` — 直接删**。全文已闭合（正文 384 行 + `0x401594`/`0x4015A2` 逐字证），零风险，且删掉一条 `unknown` 能立刻让这条规则的残差表变短；不需要复审第二轮。
2. **#8 `RES-RENDER-TEXT-SURFACE-FORMAT` — 先裁决正文自相矛盾**。正文 108 行"低字节为蓝"与 60-ops-text.md:102「`0x112233` → `0x332211`」互为反证，而 `0x76` 的写点逐字是反转（`0x41F3BB..0x41F3D3`）。这是**规则层面的错**（`amayui-spec` §9.2 的 C 类），比"再补一条证据"优先：先定哪一句留，再补通道归属。
3. **#15 `RES-RENDER-TEXT-PROJECTION` — 移出本条并开需求单**。它已经不是"本条"的问题：借用点 `0x469719`、归还点 `0x46AE08` 与 5 个绘制入口调用点（`0x469EB5` 等）同处 `sub_469260`，`[ebx+568h]/[ebx+56Ch]` 与构造点 `0x4651BD`/`0x4651C3` 是同一个两格。留在 0x204 条里会让"本条读到的值不受影响"被误读成全引擎性质。

（次优先：**#2 / #4 / #17 的补正文**——三条都已取到能写成合同的机器事实，改动小、收益直接；**#7 的 discarded 合并**可以顺手做。）
