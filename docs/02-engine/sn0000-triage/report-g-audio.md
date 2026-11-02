# 残差 triage 报告 — 簇 `g-audio`（SN0000 路径内 18 条）

- 输入：`.tmp/sn0000-triage/g-audio.md`（18 条）· `data/spec/chapters/60-ops-voice.md`（7 条规则正文）· `data/spec/audit.json`（`disposition` / `covers` / 各条 `what`·`next`）· `data/spec/bindings.json`（站点 EA）· `corpus/disasm/files/AGE.EXE__dumped.sectfix.EXE.lst` · `data/shape/types.json` · `.cache/footprint/sn0000.jsonl`
- 治理口径（照做，未重新裁决）：`REQ-01M674XHSX2HSFWSKDWZV2C63D` §audio —— 音频不排批、不改处置；保持 `unknown` 即正确；⛔ 不改 `discarded`、⛔ 不写"本版不覆盖音频"。

## 判定口径（本报告唯一的两种）

| 判定 | 含义 | 本报告的判据 |
|---|---|---|
| `deferred-audio` | 确认它就是音频/后端/真机采样类 | **剩余证据只能来自**声音后端对象语义（buffer / 声道参数 / 音量曲线 / 释放）、DirectSound-平台层、或真机·设备态 |
| `real-gap` | 不是音频类、只是被 audio 规则误挂（或**可从语料判定**） | 剩余证据是引擎侧的代码/数据事实：门位·位清者、配置键、脚本装载、请求实参形态、shape 邻接、有符号比较、读者枚举 |

★ **`real-gap` 不等于"已取证完毕"**：它只说明"不需要音频后端/采样就能收"，收口仍要走 R1/R2（带点名 `next` 的窄 `unknown` 或写进合同）。本报告**不改**任何 `disposition`。

## 计数

| | 合计 | `deferred-audio` | `real-gap` |
|---|---|---|---|
| 本簇 18 条 | 18 | **5** | **13** |
| 其中 A（正文弃权挂来的） | 9 | 2 | 7 |
| 其中 C（知识缺口） | 9 | 3 | 6 |

按规则：

| 规则 | 条数 | real-gap | deferred-audio |
|---|---|---|---|
| `CORE-OPS-AUDIO-WAV` | 7 | 6（LOAD-FAIL / PARAM-ORDER / RESOURCE / SLOT-OOB / SLOT-VS-CHANNEL / TABLE-READER） | 1（NO-BACKEND） |
| `CORE-OPS-VOICE-STOP` | 2 | 1（SLOTS） | 1（RELEASE） |
| `CORE-OPS-VOICE-VOLUME` | 2 | 1（NEGATIVE） | 1（VOLUME-BACKEND） |
| `CORE-OPS-AUDIO-BGM` | 2 | 2（CONFIG / BIT9） | 0 |
| `CORE-OPS-AUDIO-BGM-FADE` | 2 | 1（FADE-A） | 1（FADE-UNIT） |
| `CORE-OPS-VOICE-ALL-STOP` | 2 | 1（CLEAR-PROTOCOL） | 1（478-BIT0） |
| `CORE-OPS-AUDIO-SLOT-STOP-B6` | 1 | 1（B6-SLOT-FIELDS） | 0 |

`deferred-audio` 全集 = `NO-BACKEND` · `VOICE-RELEASE` · `VOLUME-BACKEND` · `BGM-FADE-UNIT` · `478-BIT0`。

## 汇总表（一行一条）

| id | 判定 | 依据（≤60 字） | 处置 |
|---|---|---|---|
| RES-AUDIO-WAV-LOAD-FAIL | real-gap | 失败支全静态：0x4B4FD4 `aWavS`+`%s`、0x4B4FEF 取常数 0x10007、0x4B4FFB `__CxxThrowException`、写表在 0x4B5004 成功支 | 转挂「异常与错误通道」+ 本条失败支；`next` 的"坏 WAV 真机采样"非必需 |
| RES-AUDIO-WAV-PARAM-ORDER | real-gap | 压栈序 0x420B24(资源)/0x420B2E(槽)；callee 0x4B4F8B 取 `[ebp+0Ch]`、0x4B5000 取 `[ebp+8]` 当 0x4B5004 的下标 | 收成请求参数 `(槽, 资源)`；顺手改 bindings 0x4b4f8b 注记措辞 |
| RES-AUDIO-WAV-RESOURCE | real-gap | 资源是**整型操作数**（0x420B1F 走读整型），0x4B4FCE→sub_454FA0 把它解析成 80 字节步长的串记录 | 转挂「数据池 / 串表」通道；0x454FA0 已给出解析形态 |
| RES-AUDIO-WAV-SLOT-OOB | real-gap | T_SE 长 1252=0x4E4，表尾 == 对象尾 ⇒ 槽 10 起写 T_Voice+0x0…+0x10（T_Voice@0x148A0） | 转挂 shape：查 T_Voice+0x0..0x10 读者；⛔ `next` 里"不可观察则 discarded"违反 audio 口径 |
| RES-AUDIO-WAV-NO-BACKEND | deferred-audio | 静态站点已在（0x4B4F69/0x4B505E/0x4B6136）；缺的是"后端为空"这一设备态在真机上怎么出现 | 保持 `unknown`；`next` = 采一次声音初始化失败的运行 |
| RES-AUDIO-WAV-SLOT-VS-CHANNEL | real-gap | 装载/播放/停止/在播四条路径同用 `[Sound+槽*4+654h]`（0x4B604C / 0x4B63CA / 0x4B6371 / 0x4B6173），界同为 15 | 收成"同一批 15 槽"；★ 与 CORE-OPS-VOICE-ALL-STOP 已写的"声道 12/13/14 = 语音槽 0/1/2"互相印证 |
| RES-AUDIO-WAV-TABLE-READER | real-gap | 唯一调用方 sub_410160（4 个站点 0x410568/0x410863/0x410D50/0x4114BF），登记在 0xA5348/0xA56DC/0xA5718/0xA5720 = 0xab/0x190/0x19f/0x1a1 | 转挂"脚本装载"通道；触发者已可点名，`next` 可收窄成"这四个 handler 的领域名" |
| RES-OPS-VOICE-SLOTS | real-gap | 那几格的读者在引擎侧：Voice+0x454 由 0x404CB0 读（`and 3`/`cmp 3`）、0x404D12 起 `lea eax,[ecx+418h]` 按资源号 > 0 判待播 | 转挂 voice 读者清单；按格拆后再看是否剩音频项 |
| RES-OPS-VOICE-RELEASE | deferred-audio | 0x4BB9F0→sub_4B6390 释放的是 buffer 对象（`[Sound+ch*4+654h]`），语义 = 缓冲释放，属后端 | 保持 `unknown`；`next` = 取该 buffer 对象的函数指针表 |
| RES-OPS-VOICE-VOLUME-BACKEND | deferred-audio | 0x4B6350 只取 buffer 再调 0x4B7110(buffer,volume)；曲线/量化在缓冲侧 | 保持 `unknown`；`next` 指 0x4B7110 与其虚表 |
| RES-OPS-VOICE-VOLUME-NEGATIVE | real-gap | 0x4B6949 是**有符号** `jl 0Fh`，通道号 = 组号+0x0C（0x4268FE）⇒ 组号 −12..2 全受理、< −12 也过闸 | 收成判据（负值 ≠ 拒绝）；`next` 的合成脚本非必需 |
| RES-AUDIO-BGM-CONFIG | real-gap | 0x420D22/0x420D3D/0x420D61 三个键 + 0x420D4F 判 `[+0x77A1C]`、0x420D6E `imul 100` ⇒ 合成 = 键值×100 | 转挂「配置」通道 + 本条音量规则；判据与公式已可读全 |
| RES-AUDIO-BGM-BIT9 | real-gap | 状态字（Engine+0xAAB44）上清第 9 位的 `and 0FFFFFDFFh` 只有 5 处：0x4129E4/0x420C28/0x420C88/0x420CEA/0x420F38 | 转挂 vm/state 位写点；余量 = 整字写点（`mov [..+0AAB44h]`/`and .., ebx`）逐个排除 |
| RES-OPS-BGM-FADE-A | real-gap | 服务链有第 27 位测点：0x412AD4 `test eax,8000000h` / `jnz 0x412A19`（不回派发支）；spec 位序表第 16 行已写 | 转挂门模型 + emulator-cxx 主循环；缺的是实现侧 service branch，不是音频 |
| RES-OPS-BGM-FADE-UNIT | deferred-audio | 0x489D10 写三格后连做三次**虚调用**（0x489D57 / 0x489D6F / 0x489D91）进音乐后端；刻度 10000 已有 0x489F80 | 保持 `unknown`；刻度/夹取可先从 `what` 摘出（静态已定），余项需虚表 |
| RES-VOICE-478-BIT0 | deferred-audio | 缺的是"声道参数命名" = 传给缓冲对象的参数语义（音量/循环），落在后端 | 保持 `unknown`；`next` = 声道参数设置子例程的三个入口 |
| RES-VOICE-CLEAR-PROTOCOL | real-gap | bit 0x10000 是选项格上的引擎数据握手位，写点就是或/与掩码（见 CORE-OPS-VOICE-PLAY/QUEUE），静态可枚举 | 转挂 state/voice 握手条目；`next` = 搜"置 bit 0x10000"的写入者 |
| RES-OPS-B6-SLOT-FIELDS | real-gap | 两格已定形：`[T_SE+槽*4+4BCh]`（10 格表）与 `[T_SE+槽*4+418h]`（另一张 10 格表）；越界落点可算 | 收成两格身份 + 越界落点算术；`next` 的"真机跑槽 10/−1"非必需 |

## 逐条（判定 / 依据 / 处置建议 / `<待核>`）

### 1. RES-AUDIO-WAV-LOAD-FAIL
- **判定**：`real-gap`（非音频类；归"异常与错误通道"）
- **依据**：失败支整段静态可见 —— 0x4B4FB3 `cmp eax,1` 分叉；失败侧 0x4B4FBF 释放缓冲（sub_455C60）、0x4B4FD4 取 `aWavS` 模板、0x4B4FDF `_sprintf_s` 填资源名、0x4B4FE7 推 `__TI1?AVCommand_ShowMessage_Exception@@`、0x4B4FEF 取常数 0x10007 并在 0x4B4FF8 存进异常对象、0x4B4FFB `__CxxThrowException`；**写表在成功支 0x4B5004**，所以残留状态 = 表不变 + 缓冲已释放。
- **处置建议**：`what` 里"抛显示消息型异常"这一半已有（`60-ops-numeric.md` 的异常与错误通道、`05-boot.md` 的保护区五类）；剩下要写的是**残留状态**与"失败不进表"。转挂：本条的失败支（写进合同）+ 异常通道（异常类与类别）。`next` 里的"取一条真实坏 WAV"可删 —— 异常码/类别/残留都已静态。
- **`<待核>`**：0x10007 的位义（是否 `0x10000 | 类别 7`，与 `60-ops-frame.md` 的"类别 7 = 可重试/忽略"是否同源）尚未逐字核；异常对象只写了两个 4 字节格（0x4B4FF5 存文本缓冲指针、0x4B4FF8 存 0x10007），其余格未读。

### 2. RES-AUDIO-WAV-PARAM-ORDER
- **判定**：`real-gap`（可从语料判定；请求参数形态不是音频知识）
- **依据**：调用点 0x420B24 先压"第 1 个操作数"（0x420B1F 读）、0x420B2E 后压"第 2 个操作数"（0x420B29 读）；callee 0x4B4F8B 取 `[ebp+0Ch]`→ 0x4B5004 作为**写进表的值**，0x4B4F9A 取 `[ebp+8]`→ 0x4B5004 的**下标**（`[esi+eax*4+4BCh]`）⇒ 声明的第 1 个形参是**槽**、第 2 个是**资源**。
- **处置建议**：把 `(槽, 资源)` 的实参次序写进本条的请求参数（边界契约）；★ 顺手改 bindings 站点 `0x4b4f8b` 的注记 —— 现文"取第 1 个实参"实际取的是 `[ebp+0Ch]`（第 2 个声明形参），与压栈序并读会自相矛盾。
- **`<待核>`**：`lpAddend` / `ArgList` 是 IDA 的推断名，我只按"写表下标 / 进 `%s`"两个用法定身份；`retn 8` 与 `__thiscall` 需在 .lst 侧逐字复核。

### 3. RES-AUDIO-WAV-RESOURCE
- **判定**：`real-gap`（转挂"操作数 / 串表"通道）
- **依据**：操作数是**整型**（0x420B1F 走读整型 sub_41BF50）；同一个值进 0x4B4FCE 的 sub_454FA0 后被解析成"高字节选段 + `this[261] + 80*a2` 的定长记录"（0x454FA0），再喂 0x4B4FD9 的 `%s`。⇒ 它不是"名字指针"，而是"整型标识 + 串表解析"。
- **处置建议**：本条正文现在写"不规定它是统一文件 id、名字还是别的形式"；静态已能把形态收到"整型标识 / 由串表解析成名字"。转挂：`60-ops` 的串表 / 操作数条目 + 本条引用它。不需音频后端。
- **`<待核>`**：`80*a2` 那条记录表的归属（是"消息串表"还是"文件名字表"）未定；`(a2>>24)` 那条分支（分段表）只在越界/特殊段时走，未追。

### 4. RES-AUDIO-WAV-SLOT-OOB
- **判定**：`real-gap`（shape 邻接 + 读者枚举）
- **依据**：shape 里 `T_SE` 长 1252 = 0x4E4，10 格表在 `T_SE+0x4BC`（0x4BC+10*4 = 0x4E4）⇒ **表尾 == 对象尾**；下一块嵌入对象是 `T_Voice`（`T_Engine+0x148A0`）。所以槽 10..14 的写落在 `T_Voice+0x0 / +0x4 / +0x8 / +0xC / +0x10`（不是任意内存）。
- **处置建议**：转挂 shape（`data/shape/`）：查这 5 格有没有读者（S3a 的四条否定证明：偏移全形态、`lea` 取址站点、整块拷贝、交出对象）。⛔ `next` 里的"不可观察则按 discards 处置"与 audio 口径冲突（不许改 `discarded`）—— 若真不可观察，应写成"实现侧已知偏离"或保留 `unknown`。
- **`<待核>`**：`T_Voice+0x0` 在 shape 里只有一个 width=4 的格（无 kind/type），`+0x4..+0x10` 根本没登记；读者搜索要按 `T_Voice` 基址的载体做，尚未做。

### 5. RES-AUDIO-WAV-NO-BACKEND
- **判定**：`deferred-audio`（真机/设备态）
- **依据**：三条支路的静态站点都在（装载 0x4B4F69、卸载 0x4B505E、在播 0x4B6136 都是判指针为空就返回）；缺的是"声音对象为空"这一状态**在真机上怎么出现、引擎随后是否继续跑**（设备缺失 / 初始化失败）。
- **处置建议**：保持 `unknown`。合同本身（"整段跳过、不发请求、不写格"）已在正文，不必等采样就能保留；要收的是可达性/可观察性。
- **`<待核>`**：三条支路判的是不是**同一个**空指针（0x4B4F69 判 `[esi+408h]`、0x4B6136 判 `[esi+560h]`、0x4B6026 判 `[esi+560h]`）—— 后两处判的是另一个格；"后端未就绪"与"后端不存在"是否同一状态本报告未裁（同族 `RES-OPS-BA-SILENT-GATES` 也指着这一项）。

### 6. RES-AUDIO-WAV-SLOT-VS-CHANNEL
- **判定**：`real-gap`（**可从语料判定，且与另一条规则自相矛盾**）
- **依据**：四条路径索引的是同一张按槽数组 `[Sound+槽*4+654h]`：播放 0x4B604C、停止 0x4B63CA、音量 0x4B6371、装载 0x4B661B、在播查询 0x4B6173（取到 buffer 后读 `[buffer+0x2460]` 作"在播"值）；临界区数组也同一张（`[Sound+(槽*3+0xD2)*8]`，0x4B65F9 / 0x4B63D5）。语音侧走进来的是"通道号 = 组号+12"（0x4268FE、0x404CD9），界同为 15 且有符号（`cmp`/`jl` 在 0x4B6142+0x4B6145、0x4B65C5+0x4B65C8、0x4B639A+0x4B639D；播放那处是 0x4B603E+0x4B6041 的 `ja 0Eh`）。⇒ **音效槽与语音通道是同一批 15 槽**。
- **处置建议**：直接收成"后端一张 15 槽的按槽表，语音占 12/13/14"；★ 本条正文现在写"不在本条（见残差）"，而 `CORE-OPS-VOICE-ALL-STOP` 已经断言"声道 12/13/14 就是语音槽 0/1/2" —— 两处口径不一致，收口时应统一。
- **`<待核>`**：这张表的 **15** 是有符号界（`jl`），所以"槽号 −1"也会进 0x4B6173 / 0x4B6371 的表前读 —— 与 `SLOT-OOB` / `VOLUME-NEGATIVE` 是同一笔；本条只判"同一批槽"，不裁负槽语义。

### 7. RES-AUDIO-WAV-TABLE-READER
- **判定**：`real-gap`（脚本装载通道）
- **依据**：`sub_4B5090` 唯一调用方 `sub_410160`，4 个站点（0x410568 / 0x410863 / 0x410D50 / 0x4114BF）都紧跟 `push offset String2 ; "CALLBACK_LOAD.BIN"` 与 `[+5D880h] ← 0`；`sub_410160` 的调用方是 4 个 handler：表项 0xA5348 / 0xA56DC / 0xA5718 / 0xA5720（基址 0xA509C，步长 4）⇒ **opcode 0xab / 0x190 / 0x19f / 0x1a1**。
- **处置建议**：转挂"脚本装载"通道；`next` 收窄成"这 4 个 handler 的领域名（各自装载/跳转什么）"。★ 足迹里这 4 个 opcode **一次都没跑**（见附注），所以"这次跑到过"答不了，静态调用图能答。
- **`<待核>`**：handler 表基址 0xA509C 我是从 bindings 的既有注记（`0xA578C = 0xA509C + 444*4`，444 = 0x1bc）反推的，没有独立复核表基；0xab 现在还在 `GAP-OPS-0000-00FF` 里（整条无规则）。

### 8. RES-OPS-VOICE-SLOTS
- **判定**：`real-gap`（读者在引擎侧，静态可枚举）
- **依据**：那几格的读者是 Voice 对象自己的方法：0x404CB0 起 `lea edi,[ebx+454h]` / `and eax,3` / `cmp al,3`（完成·跳过位）；0x404D12 起 `lea eax,[ecx+418h]`、`cmp dword ptr [eax],0` / `jg`（资源号 > 0 ⇒ 有待播）；0x404D20 还读 `[eax+3Ch]`（= +0x454 那组）。都是引擎侧谓词，不是后端。
- **处置建议**：转挂 voice 读者清单（引擎侧），按格拆：已能说的有"资源号格（>0 判在用）"与"状态字格（低 2 位 = 3 判完成）"。剩格再逐格判。
- **`<待核>`**：0x404D50 读了 `[ecx+454h]` 之后进 `[Sound+684h] → [+2470h]`（后端缓冲的一列）—— 若有格只是转交给后端参数，那部分属 audio（与 `RES-VOICE-478-BIT0` 同根），本条的 8 格里可能有一条要拆出去。

### 9. RES-OPS-VOICE-RELEASE
- **判定**：`deferred-audio`（缓冲对象语义）
- **依据**：0x4BB9F0 的释放支走 `sub_4B6390`，它取的是 `[Sound+ch*4+654h]` 这个**缓冲对象**，再做临界区与虚调用；"到底释放了什么"是 buffer 的生命周期语义，不是引擎格。
- **处置建议**：保持 `unknown`。`next` "取 SoundBuffer 的函数指针表"可执行，但**取到之后要读的是后端方法**，所以仍属音频批。
- **`<待核>`**：`SoundBuffer::vfn0(obj,1)` 这个 vtable 槽在 `sub_4B6390` 里没直接出现（我读到的窗口是 0x4B6390–0x4B63EA 的入口段）；虚调用点在更后面，未逐字核。

### 10. RES-OPS-VOICE-VOLUME-BACKEND
- **判定**：`deferred-audio`
- **依据**：0x4B6350 只做两件事：`[Sound+通道*4+654h]` 取 buffer（空则返回 0），非空则 `sub_4B7110(buffer, 音量)`（0x4B6380）。缓冲音量 / 衰减曲线在 buffer 侧（0x4B7110 及其虚表）。
- **处置建议**：保持 `unknown`。★ 注意 0x4B6350 对通道号**不做**边界检查（0x4B636E 直接 `[ecx+eax*4+654h]`），负通道会读到表前 —— 与 `RES-OPS-VOICE-VOLUME-NEGATIVE` 的越界是同一笔，收口时两处要一起说。
- **`<待核>`**：0x4B7110 未读；"音量衰减曲线"是否只是线性缩放，还是查表/对数，本报告不下结论。

### 11. RES-OPS-VOICE-VOLUME-NEGATIVE
- **判定**：`real-gap`（有符号比较，静态可判）
- **依据**：0x2f8 handler 0x4268FE `add eax,0Ch` 后直接把通道号交给 `sub_4B6940`（ecx = `Engine+0x48E8` = `T_Sound`）；0x4B6949 是 `cmp ecx,0Fh` + **`jl`（有符号）**。⇒ 组号 −12..2 全部落到 0..14 号通道被受理；组号 < −12 时通道为负、仍过 `jl`，于是在 0x4B698F `[esi+ecx*4+5DCh]` 越界写、并调 0x4B6999 的 0x4B6350（后者也不检查）。
- **处置建议**：把判据收成"拒绝的是**通道号 ≥ 15**（有符号），不是组号 ≥ 3"；负组号在 −12..−1 会写到**非语音通道**的格，< −12 是表前越界。`next` 的"构造组号 < 0 的合成脚本"不必要。
- **`<待核>`**：真的拒绝文本 0x4B6952 是 `dsSetVolume ... 不正なSound番号です`，与"组号 ≥ 3"在**可观察上**等价，所以现正文"≥3 拒绝"不算错、只是判据错位；要不要改写要 parent 定（属音频处置，本报告只给证据）。

### 12. RES-AUDIO-BGM-CONFIG
- **判定**：`real-gap`（配置通道）
- **依据**：0xbf 的 handler 0x420CC0：0x420D22 查 `set:KeepMusicVolume`、0x420D3D 查 `sound:MusicFadeOnVoicePlaying`、0x420D4F 判 `[+0x77A1C] == 0`、0x420D61 查 `sound:MusicFadeOnVoicePlayingVolume`，0x420D6E `imul eax,64h`（×100）后 0x420D78 `sub_489C00(音乐对象, 值)`。⇒ 判据是"两个键都为 1 且那格为 0"，合成公式 = 键值 × 100。
- **处置建议**：转挂「配置」通道（`55-config.md`）+ 本条补一条音量规则。次序也定了：**播放请求（0x420D14）之后**。
- **`<待核>`**：`[+0x77A1C]` 的领域身份未定（我读到的只是"必须为 0"）；它是不是"有配音在播/压低在进行"的标记，要查它的写者。★ 另：0x420CEF `push 64h` + 0x420CFD `sub_489E50` 说明"进度满 = 100"，与 0xb7 的"冲到满(100)"同源。

### 13. RES-AUDIO-BGM-BIT9
- **判定**：`real-gap`（状态字位写点枚举）
- **依据**：状态字 = `Engine+0xAAB44`（0x420CDC 读、0x420CF7 写回）。对它有 `and 0FFFFFDFFh`（清第 9 位）的**只有 5 处**：0x4129E4（sub_412290，服务体）、0x420C28、0x420C88、0x420CEA（= 0xbf）、0x420F38。置第 9 位只有 0x420E55（0xc2 的 B 支）。此外 0x412989 是"`or eax,200h` 后紧接着 0x41298E 写回"（同一函数里的置位）。
- **处置建议**：转挂 vm/state 的位写点表；把"5 处清 + 2 处置"逐站点名（EA 已给），余量是**整字写点**（`mov [..+0AAB44h], eax` 多处、`and [..+0AAB44h], ebx` 在 0x408F67 / 0x408FB8）要逐个排除。
- **`<待核>`**：`and [esi+0AAB44h], ebx` 两处（0x408F67 / 0x408FB8）的 ebx 是运行时掩码，可能清第 9 位；本报告只登记"有这两个形态"，未追 ebx 的来源。

### 14. RES-OPS-BGM-FADE-A
- **判定**：`real-gap`（门/VM 通道；★ 也适用于 `RES-OPS-VOICE-PLAY-MOVIE` 的同句）
- **依据**：服务链**有**第 27 位测点：0x412AD4 `test eax,8000000h` / 0x412AD9 `jnz 0x412A19`（不回 0x4123F0 的派发支）。spec 的位序表第 16 行已登记"第 27 位 / 交回迭代头"。⇒ "由谁服务"在**规则面已答**；残差里"本模型的主循环只有第 0/10/13 位"说的是**实现**（apps/emulator*）。
- **处置建议**：转挂门模型单 + `apps/emulator-cxx`（补一条 service branch）；不需要音频。`next` 应从"找服务分支"改成"在 cxx 主循环补位 27 的 service branch（置起 ⇒ 本迭代不派发）"。
- **`<待核>`**：0x412A19 紧接着测第 26 位（0x412A1F `test eax,4000000h`），所以"交回迭代头"这个措辞是否准确要复核 —— 若它其实是"从第 26 位继续扫"，`70-vm.md` 位序表第 16 行的"服务后"一列要改写。**这条只报疑，不改 spec。**

### 15. RES-OPS-BGM-FADE-UNIT
- **判定**：`deferred-audio`（前半是 real-gap 成分）
- **依据**：0x489D10 写三格（`[+0x424]`←目标、`[+0x41C]`←步长、`[+0x418]`←0）后，0x489D57 / 0x489D6F / 0x489D91 三次**取虚表再 call** 进音乐后端（`[esi+idx*4+434h]` 的对象）。刻度"满值 10000"已有静态绑定（0x489F80）；另有 0x420D6E 的 ×100 可互证（键 0..100 → 值 0..10000）。
- **处置建议**：保持 `unknown`。可做的一步：把"刻度 = 10000、夹取在 0x489D10 之外"从 `what` 里摘出来先落静态结论，余项（"真正生效"的虚表目标）留在音频批。★ 摘出属改正文措辞，本报告只建议、未动。
- **`<待核>`**：那三个虚表槽（+0x18 / +0x14 / +0x8）的目标对象 `[music+idx*4+0x434]` 属于哪一类（AGE 自己的 Music 类 / 外部播放器）未定 —— 若落在 AGE 自己的类里，"真正生效"也可以是 real-gap，本条判定要翻。

### 16. RES-VOICE-478-BIT0
- **判定**：`deferred-audio`（后端参数语义）
- **依据**：残差自己写"只读到分支，没有追到声道参数命名" —— 那三个分支的去向都是**给缓冲对象设参数**（音量 / 循环数），命名要落在那条子例程的参数语义上。
- **处置建议**：保持 `unknown`。`next` 具体化到"声道参数设置子例程的三个入口 + 各自第 3/4 个参数"。
- **`<待核>`**：三个入口的 EA 未在本条 bindings 里；我未逐个定位（属音频批工作），本报告不指认。

### 17. RES-VOICE-CLEAR-PROTOCOL
- **判定**：`real-gap`（引擎数据握手位）
- **依据**：bit `0x10000` 是**选项格/开关格**上的"已处理"标记，写点就是掩码或/与（`或 0x10000` 保留位 0 / `与 0x10000` 清位 0，见 CORE-OPS-VOICE-PLAY 与 QUEUE 的合同）；它是引擎格，不是后端。
- **处置建议**：转挂 state/voice 的握手条目（一条讲"谁置、谁清、三态"）；`next` = 搜"置 bit 0x10000"的所有写入者（含 `or ..., 10000h` 与整字写）。
- **`<待核>`**：本报告未做那一次全量搜索；"第三处写入者"是否存在仍未知（我只确认这个位属引擎格，不属后端）。

### 18. RES-OPS-B6-SLOT-FIELDS
- **判定**：`real-gap`（shape 身份 + 落点算术）
- **依据**：0xb6 的卸载支 0x4B5050 清两格：0x4B506A `[T_SE+槽*4+4BCh]`（10 格「槽→资源标识」表，shape 已登记）与 0x4B5075 `[T_SE+槽*4+418h]`（**第二张同样 10 格的表**，shape 里 `T_SE+0x418` 是 dword、0x440 起是 count=10 的数组）。越界落点可算：槽 10..14 的第二格落到 `T_SE+0x440..+0x454`（仍在 T_SE 内，进那张 count=10 的数组），第一格落到 `T_Voice+0x0..+0x10`；槽 −1 落在 `T_SE+0x4B8` / `T_SE+0x414`（都是已登记的 dword）—— **两侧都不会出对象**。
- **处置建议**：把"第二格 = T_SE+0x418 的 10 格表"写成身份，越界写成落点算术（不写"未观察"）；`next` 里的"真机各跑一次槽 10 / −1"不必要（没有崩溃可能，只有格被改）。余下的是这两张表的读者（第一张的读者已由 0x4B5090 定了）。
- **`<待核>`**：第二张表（+0x418）的写者我只核到这条卸载路径（0x4B5075），装载路径 0x4B5004 只写第一张 —— "第二张由谁写"仍缺，`next` 应改成这个。★ 另：0x4B639A 的受理判据是**有符号** `jl 0Fh`，负槽号会进 0x4B63CA 的 `[Sound+槽*4+654h]`（表前读），这一笔属 `RES-OPS-VOICE-VOLUME-BACKEND`/`SLOT-OOB` 的同一族，未亲核。

## 有没有被误挂在 audio 下的？

**有，13/18。** 它们挂在 audio 规则下，但剩余证据都在引擎侧（代码/数据），不需要声音后端、DirectSound 或真机：

| 转入通道 | 残差 | 一句话 |
|---|---|---|
| 异常与错误通道 | LOAD-FAIL | 抛出物与残留状态全静态 |
| 操作数 / 串表 | RESOURCE | 整型标识 + 80 字节串记录解析 |
| 边界请求参数 | PARAM-ORDER | 压栈序 + callee 取参已定 |
| shape（邻接 / 载体） | SLOT-OOB, SLOT-VS-CHANNEL, B6-SLOT-FIELDS | 表尾==对象尾、同 `[+0x654]` 一张表、两格身份 |
| 脚本装载 | TABLE-READER | 唯一调用方 + 4 个 handler 表项 |
| 配置 | BGM-CONFIG | 三个键 + ×100 已可读全 |
| vm / state（位与清点） | BGM-BIT9, BGM-FADE-A, VOICE-CLEAR-PROTOCOL | 状态字位写点 / 位 27 服务 / 0x10000 握手 |
| 引擎侧读者枚举 | VOICE-SLOTS | 读者是 0x404CB0/0x404D10 这类谓词 |
| 有符号比较 | VOLUME-NEGATIVE | `jl 0Fh` 判通道号，组号不判 |

**确实是音频/后端/采样类的只剩 5 条**：`NO-BACKEND`（设备失败态，要真机）、`VOICE-RELEASE` / `VOLUME-BACKEND`（buffer 对象语义）、`BGM-FADE-UNIT`（音乐后端虚表）、`478-BIT0`（声道参数命名）。四条是 buffer/后端对象，一条是设备态 —— 这五条**保持 `unknown` 就是正确**，与本簇治理口径一致。

★ 三个附带发现（供 parent 定，不属本次处置）：
1. **`SLOT-VS-CHANNEL` 与正文冲突**：`60-ops-voice.md` 的 `CORE-OPS-VOICE-ALL-STOP` 已断言"声道 12/13/14 就是语音槽 0/1/2"，而本条正文说"同一批槽不在本条（见残差）"。
2. **`BGM-FADE-A` / `VOICE-PLAY-MOVIE` 共用同一条门位**：两条残差都在问"第 27 位由谁置、由谁服务"，应合成一条（且是门模型/实现侧，不是音频）。
3. **`VOLUME-BACKEND` 与 `VOLUME-NEGATIVE` 共用一笔越界**：0x4B6350 与 0x4B6940 都不检查通道号上下界，收口时两处必须说同一件事。

## 附注：这次跑到过什么（只作观察，不作依据）

`.cache/footprint/sn0000.jsonl`（steps=140426）里，本簇相关 opcode 的计数：

| opcode | 次数 | opcode | 次数 |
|---|---|---|---|
| 0xb4 | 4 | 0x2f6 | 4 |
| 0xb5 | 5 | 0x2f8 | 6 |
| 0xb6 | 5 | 0x1bc | 1 |
| 0xbf | 2 | 0xc2 | 1 |

**没跑**：0xb7 / 0xb8 / 0xb9 / 0xc0 / 0xc3 / 0xc4 / 0xc5 / 0xc6 / 0xc7 / 0xba / 0x1ba / 0x2f5 / 0x2f7 / 0x2ff，以及 `TABLE-READER` 的那四个装载 handler（0xab / 0x190 / 0x19f / 0x1a1）。
⇒ 本簇里"这次跑到过"只能给 0xb4/0xb5/0xb6/0xbf/0xc2/0x2f6/0x2f8/0x1bc 八条指令作观察；其余各条的判据一律来自语料或 shape，**没有**来自足迹的推断。

## 本报告的边界

- 只写了本文件；未改 `data/spec/**`、未改 `audit.json` / `bindings.json`、未 `git add/commit`、未开 subagent。
- 未读 `apps/emulator*` 的实现、未把 `docs/` 的旧结论当事实；所有"依据"都来自我实际读过的 `.lst` 窗口（EA 已给）、`data/shape/types.json`、`audit.json` / `bindings.json` 的既有登记。
- 锚只有 EA；上文偶尔出现 `sub_XXXXXX` 只作**定位**（它本身就是 EA），没有把 `.c` 行号或函数名当锚。
- 复核方式：`pnpm tools disasm-at at --ea <EA> --lines N`（本报告引到的每个 EA 窗口都可用这一条重放）；`pnpm tools disasm-fn callers sub_4B5090` / `callers sub_410160`；shape 侧读 `data/shape/types.json` 的 `T_SE` / `T_Voice`。
