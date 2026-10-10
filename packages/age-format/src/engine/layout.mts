/**
 * packages/age-format/src/engine/layout.mjs —— **引擎镜像布局的观察记录**（不是模拟器的一部分）
 *
 * ## 这个文件是什么
 * 它是"**这份 `AGE.EXE__dumped.sectfix.EXE` 里，引擎把东西放在哪**"的**观察记录**：
 * 绝对地址（`Engine+0x5D880`）、记录内偏移（本文件里 `帧+0xNN` = 相对 `FRAME_LAYOUT.base = 0x5D894`；
 * ★ 旧注释里基址 `0x5D880` 时代的记法要 **+0x14** 才是同一个绝对地址）、池的计数/基址槽、EA 出处。
 *
 * ## ★ 为什么它**不在** `apps/emulator` 里
 * 模拟器是"按建模后的语义**重新实现**"的东西 —— 它要回答的是
 * "有哪些槽位、谁装得下什么、怎么编码、越界怎么办"。
 * 而"某个槽在第几号字节 / 哪条 `mov` 碰过它"是**逆向知识**（`AGENTS.md` §6 的知识层）：
 * 它随**这一份反汇编导出**而变，换一份镜像/换一次导出就全变。
 * ⇒ 把它放进模拟器，等于让模拟器**依赖镜像布局**，那是把"实现"和"观察"搅在一起。
 *
 * 本模块把两者分开：**这里只放观察到的事实（带 EA）**；
 * 语义（池有哪几个、编码与否、operand type tag）在 `apps/emulator/src/model/`。
 * 两边由 `tools/test/emulator-model.test.mjs` 回语料对账 —— 也就是说：
 * **偏移可以被替换，语义不必跟着动**；而偏移错了，守卫会红。
 *
 * ## 与台账的关系
 * 本文件里每个常量都对应 `data/ledger/` 里的一条观察（`subject` 形如 `Engine+0x5D880/…`）。
 * 台账是**结论的真源**；本文件是那些结论的**可执行形式**（守卫直接拿它回语料复核）。
 */

/** `Engine` 对象的全局池族槽位（绝对值 = `Engine 基址 + 偏移`） */
export const GLOBAL_SLOTS = {
  base: {
    int: 0x5d800, float: 0x5d808, string: 0x5d810,
    intRef: 0x5d818, floatRef: 0x5d820, stringRef: 0x5d828,
  },
  /** `*_alt` 槽（memflip 的第二份缓冲）。★ 语义**未定** ⇒ 只记存在，不实现行为 */
  alt: { int: 0x5d804, float: 0x5d80c, string: 0x5d814, intRef: 0x5d81c, floatRef: 0x5d824, stringRef: 0x5d82c },
  /** 计数槽（★ `pool_string_count` 在旧仓 `fields.json` 里**同名两条**：0x5D7F0 与 0x5D814 ⇒ 只认前者为"计数"） */
  count: { int: 0x5d7e8, float: 0x5d7ec, string: 0x5d7f0 },
};

/**
 * ## ★★ 帧记录的布局 —— 2026-10 **三次订正**：`base` 由 `0x5D880` 改成 **`0x5D894`**
 *
 * 裁决 = 台账 `KN-01M4H0MZGD4K4P4F0J1E7E5G79`（subject `Engine+0x5D880/frame-index-stride-vs-record-size`）。
 * 判据（全语料机械普查，可复跑）：以 `cur` 索引的字段形态 `[reg+reg*8+5D8xxxh]`
 * （`reg = 15·cur` ⇒ 净 `120·cur`）命中的绝对地址**恰好落在 `[0x5D894, 0x5D908]`**，
 * 最小 = `0x5D894`、最大 = `0x5D908`（= 基址 `+0x74`）；区间 `0x5D870..0x5D893` 与 `0x5D90C` 以上**零处**。
 * ⇒ **记录大小 == 索引步长 == `0x78`**（`0x74 + 4`），与下一条记录的 `+0x00` 严丝合缝。
 *
 * ### ★ 两种"以 `cur` 索引"的写法**都真实出现**，别混（这就是旧口径的错处）
 * * `Engine[0x5D880]` = **`cur` 自身**（Engine 级标量，见 `FRAME_ENGINE_SCALARS`）；
 *   装载器 `sub_40ED40` 读它算 `15·cur`（`.text:0040EDD0 mov eax,[esi+5D880h]` → `shl ecx,4; sub ecx,eax`）
 *   ⇒ 净 `120·cur` → 落到 **`0x5D894 + 0x78*cur`**（**第 `cur` 条记录**）。
 * * `0x5D880 + 0x78*cur` **不是任何记录地址**：`0x5D880..0x5D893` 这 5 个 dword 落在记录**之前**
 *   （`0x5D894 − 0x5D880 = 0x14`）⇒ 拿它当基址算出来的偏移全是**假偏移**（旧读数 `+0x84` 实为 `+0x70`）。
 *   ⛔ 语料里**不存在**任何以 `0x5D880` 为基址的 `*8` 索引形态（零处）—— 记录基址只能是 `0x5D894`。
 *
 * ### ★ 记法（**本文件起**）
 * `帧+0xNN` 一律 = **记录内偏移**（基址 = 本常量 `base`）。旧记录/旧注释里的 `帧+0xNN` 是
 * **基址 `0x5D880` 时代的记法** ⇒ `旧偏移 = 新偏移 + 0x14`（例：旧 `帧+0x34` = 新 `记录+0x20`）。
 *
 * ### 旁证（不单独当判据）
 * `0x5D894 + 40*0x78 = 0x5EB54` 正好是帧区之后那个成员（全语料 94 处引用）；
 * 而旧算术的 `0x5D880 + 40*0x78 = 0x5EB40` 在 `.text` 里**零处**引用（只有 `seg002` 一个 db）。
 */
export const FRAME_LAYOUT = {
  /** 第 0 条记录的基址（`Engine+0x5D894`） */
  base: 0x5d894,
  /** ★ **记录大小 == 索引步长**（两个量是同一个：最大字段偏移 `+0x74`，`+4` = 记录边界） */
  stride: 0x78,
  count: 40,
  /**
   * 记录内**偏移 → 语义名**（相对 `base`）。
   * ★ 这是**观察**：每一个都能在语料里找到出处（见文件末尾 `EVIDENCE`；完整普查见 `FRAME_SLOTS_OBSERVED`）。
   */
  off: {
    /** `0x5D894` —— **脚本缓冲基址**（`GlobalAlloc` 那块；三组 (len,ptr) 的偏移也由它加出来） */
    strBase: 0x0,
    /** `0x5D898` —— `ip` / 操作数基址（操作数每个 8 字节、opcode 在 `[第一操作数−4]`） */
    operands: 0x4,
    /** `0x5D89C` —— 第 1 个 local 计数槽（6 个连续 dword，见 `LOCAL_POOL_SLOTS`） */
    count0: 0x8,
    /** `0x5D8B4` —— 第 1 个 local 池基址槽（6 个连续 dword） */
    base0: 0x20,
    /** `0x5D8CC` —— `caller`（跨脚本回链；装载时由 Engine 级标量 `0x5D884` 写进来） */
    caller: 0x38,
    /** `0x5D8D0` —— `frameArg`（打开该脚本用的统一文件 id；被当键比较） */
    frameArg: 0x3c,
    /** `0x5D8D4..0x5D8E8` —— 三组 (长度, 指针)（长度在前、指针在后，各差 4 字节） */
    triples: [
      { len: 0x40, ptr: 0x44 },
      { len: 0x48, ptr: 0x4c },
      { len: 0x50, ptr: 0x54 },
    ],
    /**
     * `0x5D8EC` / `0x5D8F0` —— **位置**（`(ip − 脚本缓冲基址) >> 2`），**不是**下标
     * （台账 `Engine+0x5D880/frame-0x6C-0x70-position-not-index`：`0x41EDD7 sar edx,2` / `0x41EDDA mov [eax+5D8ECh],edx`）。
     * ★ 旧名 `state6C`/`state70` 里的 `6C`/`70` 是**旧基址（0x5D880）记法**，本轮随基址订正改名为新偏移。
     */
    position58: 0x58,
    position5c: 0x5c,
    /** `0x5D8F4` —— ★ `operand_count` = `2*argc + 1`（**不是** `arity`） */
    operandCount: 0x60,
    /** `0x5D904` —— `array_container`（`std::vector`：`operator new(0x10)`，3 dword = begin/end/cap） */
    arrayContainer: 0x70,
    /**
     * `0x5D8F8` / `0x5D8FC` / `0x5D900` —— **记录内三格**（`+0x64/+0x68/+0x6C`），**不是**独立数组。
     * 判据：它们以 `[reg+reg*4+5D8F8h]`（`reg = 30·cur` ⇒ 净字节步长**同为 0x78**）寻址，
     * 落点落在同一个 `[0x5D894, 0x5D908]` 区间内（见 `★★ 帧记录 = 0x5D894 起 0x78 字节` 那条守卫）。
     */
    grids: [0x64, 0x68, 0x6c],
  },
};

/**
 * ★★ **帧区旁边**的 Engine 级标量（⛔ **不是**记录字段 —— 它们**不随 `cur` 走**）。
 *
 * 判据（可复跑）：`0x5D880 / 0x5D884 / 0x5D888 / 0x5D88C / 0x5D890` 全语料**一个 `*8` 索引形态都没有**
 * （只有绝对形态 `[reg+5D8xxh]`）⇒ 它们不是 per-cur 数组的基址，也就不是记录字段。
 * ⇒ 记录基址只能从 `0x5D894` 起（见 `FRAME_LAYOUT` 的头注）。
 *
 * ⛔ 这里**不给含义**（除 `cur` 一条有逐字判据）：`dword * 4` = 字节偏移，与 `ENGINE_SCALAR_WRITES` 同口径。
 */
export const FRAME_ENGINE_SCALARS = {
  /** 当前帧号 `cur`（`.text:0041C854 mov [esi+5D880h],eax` 写它；装载器读它算 `120*cur`） */
  cur: 0x5d880,
  /**
   * `0x5D884` —— 装载器把它的**值**写进记录的 `caller` 槽（`.text:0040EDFF mov eax,[esi+5D884h]` →
   * `.text:0040EE05 mov [esi+edx*8+5D8CCh],eax`），`exit` 也写它（`.text:0041A83E`）；
   * `0x6 load-frame` 拿它存旧 `cur`（`.text:0041C84E`）。⇒ ★ **角色未定**（旧名 `callerChain`），只登记地址。
   */
  callerChain: 0x5d884,
  /** `0x5D888` —— 旧名 `savedCur`/`dispatch_saved_cur`（`.text:0040FBB2 mov [ecx+5D888h],edx`）⇒ ⛔ 只登记存在 */
  scalar05D888: 0x5d888,
  /** `0x5D88C` —— 旧名 `effectFlags`（位形态：`.text:0041999C and [ecx+5D88Ch],eax`）⇒ ⛔ 只登记存在 */
  scalar05D88C: 0x5d88c,
  /** `0x5D890` —— 旧名 `valid`（`.text:00410593 mov dword ptr [ebx+5D890h],1`）⇒ ⛔ 只登记存在 */
  scalar05D890: 0x5d890,
};

/**
 * 记录里**机械普查到**的 slot 全集：`0x5D894..0x5D908` ⇒ 偏移 `0x00..0x74`，**逐 dword 连续 30 格**
 * （这正是"记录大小 = `0x74 + 4 = 0x78`"的另一种写法）。
 * ★ 用**两种寻址形态**归一后得到：
 *   * 形态 A（字面）：`[reg+reg*8+5D8xxh]` / `[reg+5D8xxh]`
 *   * 形态 B（折叠）：`[reg+reg*8]`，偏移来自 `add/lea …,0C79h`（`0xC79 × 0x78 = 0x5D8B8` ⇒ 命中 `+0x24`）
 * ★ 只认形态 A 会**漏掉折叠的那些**（踩过：于是得出了"`+0x24` 零出现"的错误全称否定）。
 * ★ 五个 Engine 级标量（`0x5D880..0x5D890`）**不在这张表里** —— 见 `FRAME_ENGINE_SCALARS`。
 */
export const FRAME_SLOTS_OBSERVED = [
  0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c,
  0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c,
  0x40, 0x44, 0x48, 0x4c, 0x50, 0x54, 0x58, 0x5c,
  0x60, 0x64, 0x68, 0x6c, 0x70, 0x74,
];

/**
 * **引擎标量数组**：`Engine[base + 索引] = 值` 这种按索引写的槽族。
 *
 * ★ 与 `ENGINE_SCALAR_WRITES` 的差别：那里的槽下标是**指令里写死的**（每个 opcode 一个固定 dword），
 *   这里下标由**操作数**给出。
 *   ★ 由操作数给出的下标**怎么落地**（键名要不要带索引、越界要不要留痕）是**实现口径**，不在本层 ——
 *   它在 `apps/emulator/src/vm/ops.ts` 的 `scalarArrayHandler`。
 *
 * ★ `outOfRange` = **引擎自己怎么做**：`'skip'` = 引擎**静默跳过**这次写（逐字 `if (result <= 0x1F) …`，
 *   **没有**异常）；`'throw'` = 引擎抛（见 `0x30a`）。⛔ 两者不许混成一条 —— 那会把"引擎会崩"与
 *   "引擎当无事发生"变成同一种表现（照抄与留痕的做法在 `ops.ts` 的 `scalarArrayHandler`）。
 */
export const ENGINE_SCALAR_ARRAYS = [
  // `0x107`（`sub_421E50`，argc 2）：`v2 = op2; result = op1; if (result <= 0x1F) this[result + 551] = v2;`
  // ★ 实测用法（SYSTEM4#88..#96）：`copy-local-array` 先把脚本里的块填进 `global:int[43457..]`，
  //   紧接着 8 条 `0x107 (imm 0..7) (global-int 43457+i)` 把**那 8 个值**搬进引擎的这张表。
  { name: 'Engine.d551', base: 551, len: 32, opcode: 0x107, handler: 'sub_421E50', indexOperand: 0, valueOperand: 1, maxIndex: 0x1f, outOfRange: 'skip', note: '键槽表（下标由 op1 给出）' },
  // ★ `0x10b`（`sub_422070`，argc 2）：**同族但角色互换** ——
  //   `v2 = op2（索引）; result = op1（值）; if (result <= 0x1F) this[v2 + 1383] = result;`
  //   ⇒ 索引来自 **op2**、范围检查落在 **op1** 上。两处差别（谁当索引 / 哪张表）都写在字段里，
  //   ⛔ 不要用"看起来差不多"把两条 opcode 合并实现。
  { name: 'Engine.d1383', base: 1383, len: 32, opcode: 0x10b, handler: 'sub_422070', indexOperand: 1, valueOperand: 0, maxIndex: 0x1f, outOfRange: 'skip', note: '另一张按索引写的表（下标由 op2 给出）' },
  // ★ `0x30a`（`sub_426B60`，argc 2）：表 1969，**两条范围检查且越界都抛**
  //   `v2 = op2（索引）; result = op1（值）; if (result > 0x1F || v2 > 7) throw (aSetgeskey); this[v2 + 1969] = result;`
  //   ⇒ `outOfRange: 'throw'`（与上面两条的 `'skip'` **不同**，这正是它们必须分成两条登记的理由）；
  //   另注意索引上界是 **7**（不是 0x1F）：这张表只有 8 格。
  { name: 'Engine.d1969', base: 1969, len: 8, opcode: 0x30a, handler: 'sub_426B60', indexOperand: 1, valueOperand: 0, maxIndex: 0x7, maxValue: 0x1f, outOfRange: 'throw', note: '8 格的表；值与索引都有范围检查，越界抛 aSetgeskey' },
];

/**
 * 各 local 池在**记录内**的**计数槽 / 基址槽**（观测；与 `apps/emulator` 里的语义池名对应）。
 *
 * ## ★★ 2026-10 三次订正（本轮）：记录基址 `0x5D880` → **`0x5D894`** ⇒ 本表所有偏移 **−0x14**
 * 三次订正的次序（都留着，别丢）：① 基址写成 `+0x3C…+0x54`（错：把 store 地址抄成了别的偏移）；
 * ② 回到 `帧(0x5D880)+0x34…+0x48`（store 与 type 配对正确，但**基址**取错了）；
 * ③ **本轮**：基址 = `0x5D894`（见 `FRAME_LAYOUT` 头注的三条判据）⇒ 同一批 store 的**记录内**偏移是
 * `+0x20/+0x24/+0x28/+0x2C/+0x30/+0x34`，计数是 `+0x08/+0x0C/+0x10/+0x14/+0x18/+0x1C`。
 * ★ 下面引的**绝对地址逐字不变**（`5D8B4h` 还是 `5D8B4h`）—— 变的只是"它离记录基址多远"。
 *
 * ### 判据一：装载器 `sub_40ED40` 的 6 处 store（逐字，本轮复核）
 * ```
 * 25262 .text:0040F2E9 mov [esi+edx*8+5D8B4h], eax   ;; 记录+0x20（int）
 * 25282 .text:0040F321 …（折叠形：`0xC79` ⇒ 0xC79*0x78 = 0x5D8B8 = 记录+0x24）
 * 25332 .text:0040F3B1 mov [esi+edx*8+5D8BCh], ecx   ;; 记录+0x28
 * 25351 .text:0040F3F0 mov [esi+edx*8+5D8C0h], eax   ;; 记录+0x2C
 * 25370 .text:0040F42F mov [esi+edx*8+5D8C4h], eax   ;; 记录+0x30
 * 25390 .text:0040F471 mov [esi+edx*8+5D8C8h], eax   ;; 记录+0x34
 * ```
 *
 * ### 判据二（**定配对的那一半**）：取址原语 `sub_42AEA0` 的跳转表按 **operand type** 分派
 * 入口 `sub ecx,3` + `cmp ecx,0Bh` ⇒ case `3..14`（`.lst:66226-66232`，表 `jpt_42AF16` @`0x42B47C`）。
 * IDA 在**每个 case 标签上写了 case 号**，而 case 号 = type ⇒ 池名与偏移是**机械对上**的，不靠顺序猜：
 * ```
 * 66256 loc_42AF5B ; jumptable case 9  → 66262 mov ecx,[esi+edx*8+5D8B4h]   ⇒ int       = 记录+0x20
 * 66288 loc_42AFB7 ; jumptable case 10 → 66295 mov ecx,[esi+edx*8]（0xC79 折叠）= 记录+0x24 ⇒ float
 * 66313 loc_42B000 ; jumptable case 11 → 66319 mov edx,[esi+edx*8+5D8BCh]   ⇒ string    = 记录+0x28
 * 66246 loc_42AF3D ; jumptable case 12 → 66251 mov ecx,[esi+edx*8+5D8C0h]   ⇒ ptr       = 记录+0x2C
 * 66278 loc_42AF99 ; jumptable case 13 → 66283 mov ecx,[esi+edx*8+5D8C4h]   ⇒ floatPtr  = 记录+0x30
 * 66325 loc_42B027 ; jumptable case 14 → 66330 mov ecx,[esi+edx*8+5D8C8h]   ⇒ stringPtr = 记录+0x34
 * ```
 * ★ 折叠形的算术（自证 `0xC79` 这个魔数）：`(记录基址 + 0x24) / 步长 = 0x5D8B8 / 0x78 = 0xC79`，
 *   而 case 10 的体是 `ecx = cur + 0xC79; edx = 15*ecx; [esi + edx*8]` ⇒ 地址 = `记录 + 120*cur + 0x24`。
 *
 * ### 判据三（旁证）：`ENC(key,0)` 的初值填在 `记录+0x20` 那一池
 * `.lst:25404 mov eax,[eax+5D8B4h]` + `.lst:25407 mov [eax+ecx*4],edx`，而 `edx = [esi+5EC90h] = ENC(key,0)`
 * ⇒ 那个池是 **int 池**（int 族初值口径），与判据二的 case 9 相互印证。
 *
 * ★ `count` 那六格**三次订正都没动**（绝对地址一律是 `0x5D89C..0x5D8B0`）：两种读法给出同一组。
 * ★ 教训（写给下一个复核者）：这次翻案的根因不是"旧仓对/本仓错"，而是**基址取错**（`0x5D880` 是 `cur` 的槽，
 *   不是记录基址）⇒ 逐字行号留在上面，判据要能当场复算，别只留结论。
 */
export const LOCAL_POOL_SLOTS = [
  { name: 'int', count: 0x08, base: 0x20 },
  { name: 'float', count: 0x0c, base: 0x24 },
  { name: 'string', count: 0x10, base: 0x28 },
  { name: 'ptr', count: 0x14, base: 0x2c },
  { name: 'floatPtr', count: 0x18, base: 0x30 },
  { name: 'stringPtr', count: 0x1c, base: 0x34 },
];

/**
 * 元素宽度里哪些**逐字核实过**（★ 没核实的别当事实）。
 * ★ `string: true`（2026-10 订正）：28 字节 = MSVC `std::string`（`+0` 数据 / `+16` size / `+20` capacity，
 * `cap < 0x10` ⇒ 内联 16 字节），由两条独立路径核实（`sub_433310` 的 `28*idx` 与 `sub_42A420`/`41B640` 的
 * `cmp [elem+0x14],10h`）；本仓另有一处实证（`INITCONFIG0` 的五条内联串解出字体名）。
 */
export const ELEM_BYTES_VERIFIED = { int: true, float: true, ptr: true, floatPtr: true, stringPtr: true, string: true };

/** 常量携带的语料证据（守卫会拿它回语料复核 → EA 锚的"可执行形式"） */
export const EVIDENCE = {
  frameStride: { at: '.text:0041BF64..0041BF69', insns: ['shl edx,4', 'sub edx,eax', 'mov eax,[ecx+edx*8+5D898h]'], note: '15·cur ×8 = 120·cur（两条取操作数原语同形）' },
  localPoolBases: {
    at: '.text:0040F2E9..0040F471',
    insns: ['mov [esi+edx*8+5D8B4h],eax', 'mov [esi+edx*8+5D8BCh],ecx', 'mov [esi+edx*8+5D8C0h],eax', 'mov [esi+edx*8+5D8C8h],eax'],
    note:
      '装载器 `sub_40ED40` 把 6 个 local 池的**基址**写进 记录+0x20/0x24/0x28/0x2C/0x30/0x34；' +
      '★ **配对**由取址原语 `sub_42AEA0` 的跳转表 case 号（= operand type）钉住（case 9→+0x20 … case 14→+0x34），' +
      '不是靠"分配顺序"猜。逐字行号见 `LOCAL_POOL_SLOTS` 的头注（三次订正的经过也写在那里）',
  },
  intPoolBase: { at: '.text:0042AF2F..0042AF35', insns: ['mov edx,[esi+5D800h]', 'lea eax,[edx+ecx*4]'], note: 'operand type 3 的取址：base + idx*4（下标不过编码）' },
  /** ★ int 族的 `key` 在 `Engine+0x5EC8C`（运行期赋值，从不出现在立即数里） */
  codecKey: { at: '.text:00417359', insns: ['mov [esi+5EC8Ch], edx'], note: '全清单唯一的写点' },
  /** ★ `enc_zero` 在 `Engine+0x5EC90` = `ENC(key,0)` ⇒ **非 0** */
  encZero: { at: '.text:00415990', insns: ['mov [esi+5EC90h], eax'], note: 'eax = rol(key,21) = ENC(key,0)' },
};

/**
 * ★★ **引擎标量槽的写入观察** —— 只登记"**哪个 opcode 的 handler 写了哪个 dword 槽、写成什么形态**"，
 *   **语义一律未定**（⛔ 不给含义、不解释用途、不代表它属于哪个子系统）。
 *
 * ## 为什么需要它
 * 启动链前段有一批薄 handler，形状就是"读操作数 → 写引擎的某个 dword"（例：`0x149` 写 `this[97058]`）。
 * 模拟器要跑过去就得**把值存下来**：否则后面若有分支读同一个槽，我们会**静默走错**（不报错，只是路径不同）。
 * 而 `apps/emulator` 里**不许出现偏移**（本仓硬口径）⇒ 偏移留在这里，模型只按 `name` 引用。
 *
 * ## 判据（都可复跑）
 * * `handler`：`pnpm tools opcodes handlers --opcode <opcode>` **现算**所得
 *   —— 守卫 `tools/test/opcodes-handlers.assets.test.mjs` 逐条对账；
 * * `dword`：handler 体（`pnpm tools disasm-at pseudo --sym <handler>`）里**逐字出现** `this[<dword>]`
 *   （一个 handler 写多个槽 ⇒ 多条记录）；
 * * `form`：值的来源 —— `op1` = 操作数 1 的原值、`op2` = 操作数 2、`const` = **写死的常量**（`value` 给出）、
 *   `bool(op1)` = 非 0 归一成 1、
 *   `bswap24(op1)` = 在**低 24 位内**把字节序倒过来（`b0<<16 | b1<<8 | b2`；像 BGR↔RGB）；
 * * `max`：引擎**自己**的范围检查（越界它抛 C++ 异常）—— 值的形态与上界是**事实**；
 *   照抄成抛（⛔ 不许 clamp）是**实现口径**，在 `apps/emulator/src/vm/ops.ts` 的 `scalarHandler`；
 * * ★ **名字前缀区分两种存储**（同一个存储按名字寻址，但语义不同，别混）：
 *   `Engine.dNNN` = `this[NNN]`（引擎对象里的字段）；`Global.dNNNNNN` = **进程全局**（不在 `Engine` 里，
 *   例如 `0x248` 写的 `dword_55052C`）。
 * * `callsAfter`：**写完标量之后**还有哪次子系统调用 —— ★ 这一列是**必需的诚实**：
 *   少了它，"handler 里那次未建模的调用"就不会进保真欠账，日志会显得比实际干净
 *   （本仓踩过：`0x78`/`0x2db` 的体里都有 `sub_459F40(...)`，第一版没记）。
 *   ★★ 2026-10 起它记**三件事**（原先只有一个符号串，于是"会回来"与"不回来"、以及
 *   "传的是操作数还是常量/引擎槽"在欠账里**都分不出来**）：
 *   `callee`（符号 / 调用点描述）+ `transfer`（`'call'` = 会回来 / `'tail'` = **尾跳**，不回来）
 *   + `args`（**实参形态**：操作数 / 常量 / 引擎槽 —— 见 `CallArgForm`）。
 *   ⛔ **没写 `args`** 表示"只取证了 op1 这一个实参"，**不**表示"callee 只收一个实参"。
 *
 * ## ⛔ 它**不是**语义结论
 * `Engine.d97058` 只是一个**稳定身份**（dword 下标），**不是含义**。含义要等有人按 handler 的
 * 调用方 / 读者去核 —— 那才是知识（准入门见 `AGENTS.md` §6）。
 */

/**
 * ★★ **一次未建模调用的实参形态** —— 欠账里必须能分出"这个位置到底传了什么"。
 *
 * ## 为什么需要它（这是**系统性**盲点，不是个别 handler 的疏漏）
 * 只要 handler 给 callee 传了**操作数以外**的东西（写死的常量、引擎字段、`op1 + 12` 这类算术），
 * 本层原先就**看不见**它 —— 而"看不见"会让"这条分支永远不会走"与"走了但没记录"变成同一种表现。
 *
 * ## 三种形态（**至少**要能分开这三样）
 * * `operand`：第 `index` 个操作数（0 基）—— 值是**读**出来的；
 * * `const`：handler 体里**写死的立即数**（例：`0x70` 传给 `sub_45D660` 的第 6 个显式实参 `0`）；
 * * `slot`：**引擎槽**（`Engine.dNNN`）—— 例：`0x71` 把 `Engine.d97055` 当门传下去、
 *   `0x70`/`0x71`/`0x76`/`0x77`/`0x78`/`0x2db` 都把**子对象指针槽** `Engine.d21324`（= `Engine+0x14D30`）
 *   当 callee 的 `this` 传下去。
 * ★ 顺序 = **callee 收到的实参顺序**（`this`/接收者也算一位 —— 因为它同样不是操作数）。
 * ★ 还没取证的实参**不要猜**：宁可少写（见 `callsAfter` 那条"没写 `args`"的口径）。
 */
export type CallArgForm =
  | { kind: 'operand'; index: number }
  | { kind: 'const'; value: number }
  | { kind: 'slot'; name: string };

/** **写完标量之后**那次未建模的子系统调用（`callsAfter` 的一行） */
export interface ScalarCallAfter {
  /** 被调符号；不是直接 `call` 一个符号时用描述（例 `0x2ee` 的 `(vtable+12) …`） */
  callee: string;
  /**
   * ★ **会回来吗**：`'call'` = `call sym`（返回到 handler 的下一条）；
   *   `'tail'` = `jmp sym`（**尾跳**，本 handler 的控制流不再回来）。
   *   逐字判据：`0x78`（`sub_41F450`）`.text:0041F47F jmp sub_459F40`、`0x2db`（`sub_426500`）
   *   `.text:0042652F jmp sub_459F40`；而 `0x76`（`sub_41F390`）`.text:0041F3DB call sub_459F40`、
   *   `0x77`（`sub_41F3F0`）同形 ⇒ 四行原先长得**完全一样**，欠账里分不出这两种。
   */
  transfer: 'call' | 'tail';
  /** 实参形态（按 callee 的实参顺序，含接收者）；**不写** = 只取证了 op1 */
  args?: readonly CallArgForm[];
}

/**
 * `ENGINE_SCALAR_WRITES` 的一行：**哪个 opcode 的 handler 把操作数（或它的一个变体）写进哪个固定槽**。
 * ★ 为什么要有这个接口（原先是没有注解的数组字面量）：`callsAfter` 的元素类型会被**推断**成
 *   `string[]`（旧形态）⇒ "实参形态 / 尾跳 vs 调用"这类**结构**根本没处表达，写错了也没有类型错误。
 */
export interface ScalarWrite {
  name: string;
  dword: number;
  opcode: number;
  handler: string;
  form: 'op1' | 'op2' | 'bool(op1)' | 'bswap24(op1)' | 'const' | 'unsupported';
  /** `form: 'const'` 时写进去的那个常量 */
  value?: number;
  /** 引擎**自己**的范围检查（越界它抛 C++ 异常） */
  max?: number;
  /** 写完标量之后那次未建模的子系统调用 */
  callsAfter?: readonly ScalarCallAfter[];
}

export const ENGINE_SCALAR_WRITES: readonly ScalarWrite[] = [
  { name: 'Engine.d97058', dword: 97058, opcode: 0x149, handler: 'sub_4229A0', form: 'op1' },
  { name: 'Engine.d166965', dword: 166965, opcode: 0x21b, handler: 'sub_423C20', form: 'bool(op1)' },
  { name: 'Engine.d92323', dword: 92323, opcode: 0x252, handler: 'sub_425AB0', form: 'op1' },
  { name: 'Engine.d1415', dword: 1415, opcode: 0x88, handler: 'sub_41FAB0', form: 'op1' },
  { name: 'Engine.d97050', dword: 97050, opcode: 0x88, handler: 'sub_41FAB0', form: 'op1' },
  // ★★ `callsAfter` 的**参数口径**（2026-10 取证）：`sub_459F40` 的 `this` **不是 Engine**，
  //   而是**字体管理器子对象 `Engine+0x14D30`**（槽 `Engine.d21324`）—— 5 个调用方在调用点前逐字
  //   `lea ecx, [..+14D30h]`（`.lst:46607/46647/46670/57911/78916`）。
  //   ★ 所以 `+0x15280/84/8C` 的宿主是**那个子对象**，写这些槽的是"往子对象里写"，
  //   不是"往 Engine 顶层的这三个偏移写"。⇒ 它同时是**实参**（`args: [{kind:'slot', name:'Engine.d21324'}]`）。
  // ★★ `transfer`：`0x78`/`0x2db` 是**尾跳**（逐字 `jmp sub_459F40`，见 `ScalarCallAfter.transfer` 的注释）；
  //   `0x76`/`0x77` 是 `call`（会回来）⇒ 两者在欠账里必须分得开。
  { name: 'Engine.d21667', dword: 21667, opcode: 0x78, handler: 'sub_41F450', form: 'op1', callsAfter: [{ callee: 'sub_459F40', transfer: 'tail', args: [{ kind: 'slot', name: 'Engine.d21324' }] }] },
  { name: 'Engine.d71744', dword: 71744, opcode: 0x2db, handler: 'sub_426500', form: 'op1', callsAfter: [{ callee: 'sub_459F40', transfer: 'tail', args: [{ kind: 'slot', name: 'Engine.d21324' }] }] },
  // ★ `0x76`：写 `Engine+0x15280`（紧邻 `0x78` 的 `+0x1528C`，同一个结构的不同字段）+ 同一次 `sub_459F40`
  { name: 'Engine.d21664', dword: 21664, opcode: 0x76, handler: 'sub_41F390', form: 'bswap24(op1)', callsAfter: [{ callee: 'sub_459F40', transfer: 'call', args: [{ kind: 'slot', name: 'Engine.d21324' }] }] },
  // ★ `0x77` 与 `0x76` **同形**（只差槽：`+0x15284`），体逐字 `this[21665] = BYTE2(v2) + ((BYTE1(v2) + ((u8)v2 << 8)) << 8)`
  { name: 'Engine.d21665', dword: 21665, opcode: 0x77, handler: 'sub_41F3F0', form: 'bswap24(op1)', callsAfter: [{ callee: 'sub_459F40', transfer: 'call', args: [{ kind: 'slot', name: 'Engine.d21324' }] }] },
  // ★ `0x1a4`（argc 2）：**两个槽、两个操作数**（`this[21671] = op2; this[21670] = op1`，无子系统调用）
  { name: 'Engine.d21670', dword: 21670, opcode: 0x1a4, handler: 'sub_41FE60', form: 'op1' },
  { name: 'Engine.d21671', dword: 21671, opcode: 0x1a4, handler: 'sub_41FE60', form: 'op2' },
  // ★ `0x2ee`（argc 1）：`this[80106] = op1` 之后对 `Engine+0xAA514` 那个对象走 **vtable+12**
  //   （实参含静态串 `aMessageMessage_0` 与 op1）——与 `0x1ca` 同形。**返回值不写回操作数**。
  //   ★ **偏移订正**（逐字）：`sub_426650` 的 `this[174405]` ⇒ `174405*4 = 0xAA514`；同族 `0x1ca`
  //     （`sub_420240`）逐字也是 `lea esi, [ecx+0AA514h]` ⇒ 原先的 `0xAA554` / `ops.ts` 的 `0xAA614`
  //     **都是抄错的数字**（514 被写成 554/614）。
  //   ★ 逐字（`0x426676 mov ebx,[esi+0AA514h]` / `0x42668B mov edx,[ebx+0Ch]` / `0x42668E push eax` /
  //     `0x42668F push offset aMessageMessage_0` / `0x426694 lea ecx,[esi+0AA514h]` / `0x42669A call edx`）
  //     ⇒ `(*(vtable+12))(Engine+0xAA514, "message:MessageFade", op1)`。
  //   ★ `args` **故意不写**：那个静态串的地址还没取（只写了形态不完整的两个位置会误导）⇒ 按
  //     `callsAfter` 的口径，欠账里只记 op1 这一个实参。
  { name: 'Engine.d80106', dword: 80106, opcode: 0x2ee, handler: 'sub_426650', form: 'op1', callsAfter: [{ callee: '(vtable+12) on Engine+0xAA514（aMessageMessage_0 = "message:MessageFade"）', transfer: 'call' }] },
  // ★ `0xfe`（argc 1）：**带范围检查**的标量写（`sub_421CA0`）——
  //   `result = op1; if (result > 0x1F) throw (aSetkeytotal, 65541); this[517] = result;`
  //   ⇒ 上界不是"顺手加的守卫"，是**引擎自己抛的异常**（`max` 一字不差照抄 0x1F）。
  { name: 'Engine.d517', dword: 517, opcode: 0xfe, handler: 'sub_421CA0', form: 'op1', max: 0x1f },
  // ★ `0x10f`（argc 1）：普通标量写（`sub_422120`：`this[122369] = op1`）
  { name: 'Engine.d122369', dword: 122369, opcode: 0x10f, handler: 'sub_422120', form: 'op1' },
  // ★ `0x25b`（`sub_425E20`，argc 1）：
  //   `result = op1; this[92379] = 2; this[92381] = result;
  //    if (!this[167990]) { v3 = op1; sub_408440(this, v3); }`
  //   ⇒ 两个固定槽（一个写**常量 2**、一个写 op1）+ 一次**条件**子系统调用
  //   （条件读的是 `Engine.d167990` —— 那个槽谁写的**还没取证**，见 handler 里的说明）。
  { name: 'Engine.d92379', dword: 92379, opcode: 0x25b, handler: 'sub_425E20', form: 'const', value: 2 },
  { name: 'Engine.d92381', dword: 92381, opcode: 0x25b, handler: 'sub_425E20', form: 'op1' },
  // ★ `0x248`（`sub_4252E0`，argc 1）：`result = op1; dword_55052C = result;`
  //   ⇒ 写的是**进程全局**（不在 `Engine` 里）⇒ 名字前缀用 `Global.`，别让"Engine 字段"这个说法变成谎
  { name: 'Global.d55052C', dword: 55052, opcode: 0x248, handler: 'sub_4252E0', form: 'op1' },
  // ★★ `0x110` / `0x111` / `0x112`：**派发表里没有登记**它们（实测：`byOpcode.get(0x110)` 为空）。
  //   而表是 `rep stosd` **预填**成默认 handler 的 —— 那个默认 handler（`sub_418E30`）的体是
  //   抛「このコマンドはサポートされていません．」⇒ **引擎明确不支持这三条命令**。
  //   ⇒ 本层把它们登记成"照抄引擎的抛"，于是"走到这里"就变成一个**可见的分歧信号**
  //   （而不是含混的"本批未实现"）：真实启动流程**不该**执行到这些命令。
  { name: 'Engine.unsupported', dword: -1, opcode: 0x110, handler: 'sub_418E30', form: 'unsupported' },
  { name: 'Engine.unsupported', dword: -1, opcode: 0x111, handler: 'sub_418E30', form: 'unsupported' },
  { name: 'Engine.unsupported', dword: -1, opcode: 0x112, handler: 'sub_418E30', form: 'unsupported' },
];

/** 一条条件副作用的**门**（什么时候发生；`op1` = 第 1 个操作数的值） */
export type ScalarBitWhen = 'op1!=0' | 'op1==0' | 'always';

/** 一条条件副作用的**形态**：`write` = 写死一个常量；`set` = `|= mask`；`clear` = `&= ~mask` */
export type ScalarBitOp = 'write' | 'set' | 'clear';

/**
 * 一条**条件副作用**：`opcode` 的 handler 在 `when` 成立时对 `name` 这个槽做一次 `op`。
 * ★ 与 `ENGINE_SCALAR_WRITES` 的差别写在下面那张表的头注里（值不是操作数的函数 + 带条件）。
 */
export interface ScalarBitEffect {
  opcode: number;
  /** handler 符号（同 `ENGINE_SCALAR_WRITES` 的 `handler` 字段口径） */
  handler: string;
  when: ScalarBitWhen;
  /** 槽名（`Engine.dNNN` = `this[NNN]`） */
  name: string;
  /** `this[NNN]` 的下标（`dword * 4` = 字节偏移 —— 与 `ENGINE_SCALAR_WRITES` 同口径） */
  dword: number;
  op: ScalarBitOp;
  /** `op: 'write'` 时写进去的常量 */
  value?: number;
  /** `op: 'set' | 'clear'` 时的位掩码 */
  mask?: number;
}

/**
 * ★★ **条件副作用**（写常量 / 位置位 / 位清除）—— `ENGINE_SCALAR_WRITES` 表达不了的那一类。
 *
 * ## 为什么单列一张表（而不是往上面那张表加一行）
 * 上面那张表的一条记录 = "**读操作数 → 把它（或它的一个变体）写进某个固定槽**"，值总是操作数的函数。
 * 这里的两条**不是**：`0x88` 的两条副作用**由 `op1` 是否为 0 二选一**，其中一条写的是**常量 1**、
 * 另一条是**位清除**；`0x71` 的那条是**无条件置位**。⇒ 硬塞进上面那张表会让 `form` 变成一个
 * 二义字段（"op1" 还是"常量"还是"位"），而**条件**（什么时候发生）没处放。
 *
 * ## 判据（都可复跑）
 * 每条都能在 handler 体里找到逐字（`EA` 写在行注释里）；`name` 的 `dNNN` = `this[NNN]` 的下标，
 * `dword * 4` 就是字节偏移（`122368*4 = 0x77800`、`174801*4 = 0xAAB44`）。
 * ⇒ 守卫 `tools/test/emulator-engine-scalars.assets.test.mjs` 拿这些 EA 回语料逐字对。
 *
 * ## ⛔ 它**不是**语义结论
 * "这个位现在 0 还是 1"是**状态**；"它意味着什么"（哪个子系统的哪个门）**未取证** ⇒ 不写含义。
 * ★ 已知的**成对关系**（观察，不是解释）：`0x71` 置 `Engine.d174801` 的 bit27，`0x88` 在 `op1 == 0` 时清它。
 */
export const ENGINE_SCALAR_BITS: readonly ScalarBitEffect[] = [
  // `0x88`（handler `sub_41FAB0`，argc 1）**真支**：`0x41FAE4 mov dword ptr [esi+77800h], 1`
  //   （`0x77800 / 4 = 122368`；门是 `0x41FAE0 test eax,eax` + `0x41FAE2 jz`，`eax` = op1）
  { opcode: 0x88, handler: 'sub_41FAB0', when: 'op1!=0', name: 'Engine.d122368', dword: 122368, op: 'write', value: 1 },
  // `0x88` **假支**：`0x41FAF0 and dword ptr [esi+0AAB44h], 0F7FFFFFFh`（`0xAAB44 / 4 = 174801`）
  { opcode: 0x88, handler: 'sub_41FAB0', when: 'op1==0', name: 'Engine.d174801', dword: 174801, op: 'clear', mask: 0x08000000 },
  // `0x71`（handler `sub_41ED80`，argc 1）：`0x41EE8D or dword ptr [ebx+0AAB44h], 8000000h` —— 与上面那一清**成对**
  { opcode: 0x71, handler: 'sub_41ED80', when: 'always', name: 'Engine.d174801', dword: 174801, op: 'set', mask: 0x08000000 },
];

/**
 * **同族但进不了上面那几张表的槽写**（形态不同 ⇒ 表里放不下）—— 观察，仍是引擎事实。
 *
 * 登记它的理由与 `ENGINE_SCALAR_WRITES` 相同：**"哪个 handler 碰了引擎的哪个 dword、写成什么"**
 * 是引擎事实，而 `apps/emulator` 里不许出现偏移 ⇒ 偏移留在本层，模型只按名字引用。
 * ⛔ 这里同样**不给含义**（只有"谁写它、写成什么形态"）。
 *
 * * `0x88`（handler `sub_41FAB0`，argc 1）的两个**条件**副作用：
 *   `if (op1) this[122368] = 1; else this[174801] &= ~0x8000000;`
 *   ★ **已进 `ENGINE_SCALAR_BITS`（上面那张表）并被建模**（2026-10）：槽的读 / 写都由模型经
 *     `EngineScalars.clearBits` / `setBits` 落到状态里 ⇒ "这个位现在 0 还是 1"答得出来。
 *     ⛔ 别再在这里复述那两个偏移 —— 表里有 `dword`/`mask`，两处写必然漂。
 * * `0x212` / `0x25d` / `0x213` 的**对象表**：表基址 = `Engine + 4*21585` = **`Engine+0x15144`**，
 *   按 `op1`（槽号）取指针取对象；**表项为空 ⇒ 什么都不做**（不抛、不报错 —— 又一处"静默"）；
 *   取到对象后写"对象 + 字段偏移"（字段偏移见 `apps/emulator/src/vm/ops.ts` 的 `OBJECT_FIELD_WRITES`
 *   数据行 —— 那几行**暂未**进本层，别在这里重抄一遍）。
 *   ★ "这一族有多大 / 还有几个待核"是**进度**，在需求树里，不在本文件。
 */
