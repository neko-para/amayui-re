/**
 * tools/lib/lst-format.mjs —— **语料格式词汇表**（IDA 导出的 `.lst` 的**行文法**）
 *
 * ## 为什么单独一层
 * 语料是**一次性产物**（IDA 导出），它的行形态（`段:EA 助记符 操作数`）是**格式事实**，不是谁的私有细节：
 * ① `lib/disasm.mjs`（**定位与切片**：EA → 行号 → 有界上下文）要用；
 * ② `lib/catalog.mjs`（**item 目录**：逐函数/逐定义切分 + 两侧关联 + 影响面）也要用。
 * 两边各抄一份的代价**已经实测过**：同一个错误在本仓犯了两次 ——
 * 段名以 `.` 开头（`.text` / `.data`）被 `^[A-Za-z_]` 整段漏掉，而漏掉的表现是
 * **"地址不在语料覆盖范围内"这种看起来像正常边界提示的东西**（不是报错）。
 * ⇒ 词汇表只有一份，谁改谁在这里改，两边同时受益（守卫见 `tools/test/disasm-fn.assets.test.mjs`）。
 *
 * ## 只做文法，不做语义
 * 本文件不认识"函数""全局变量""调用图"这些概念（那些在 `catalog.mjs`）；
 * 它只回答"这一行的 `段:EA` 是什么""这是不是 `proc near` 头""`call` 的目标是不是符号"。
 * 例外只有一处：**IDA 自动名的编码规则**（下面 `AUTO_NAME_RE`）—— 它同样是**格式事实**
 * （名字里编码 EA），而且它是"C 侧回连到 EA"的**唯一钥匙**，必须与行文法放在一起、只有一份。
 */

/** 一行开头的 `段:EA` 前缀（IDA 导出形态）。`body` = 前缀之后的其余部分（助记符 + 操作数） */
export const PREFIX_RE = /^([A-Za-z_.][\w.]{0,30}):([0-9A-Fa-f]{8})\s?(.*)$/;

/** 头部注释里的 Imagebase（EA 是虚拟地址；要读字节得先映射到文件偏移） */
export const IMAGEBASE_RE = /^\S+\s*;\s*Imagebase\s*:\s*([0-9A-Fa-f]+)/;

/**
 * ★ 符号名字符类（**必须含 `@` / `?` / `$`**）：MSVC 的修饰名会长这样 ——
 * `_WinMain@16`、`??2@YAPAXI@Z`、`??_7?$Stack@H@@6B@`。
 *
 * ⚠ **实测踩过**：旧类 `[A-Za-z_][\w.]{0,40}` **不含 `@`** ⇒ `_WinMain@16 proc near` 不被认作函数头，
 * `enclosingFunction()` **静默**返回**前一个**函数（而调用方照样打印出一个函数体）——
 * 那不是"查不到"，是**给了一个看起来对的错答案**。
 */
export const SYM = '[A-Za-z_$?@][\\w.$?@]{0,60}';

/** `段:EA 符号 proc near` —— 函数头（带 EA，用于二分与归属） */
export const PROC_EA_RE = new RegExp(`^([A-Za-z_.][\\w.]*):([0-9A-Fa-f]{8})\\s+(${SYM})\\s+proc near`);
/** `符号 endp` —— 函数尾 */
export const ENDP_RE = new RegExp(`^\\S+\\s+(${SYM})\\s+endp\\b`);
/** `.lst` 每行开头的 `段:EA`（用于算函数体内**最后一个 EA** ⇒ EA 归属要能拒绝"越过函数末尾"的地址） */
export const LINE_EA_RE = /^[A-Za-z_.][\w.]*:([0-9A-Fa-f]{8})\s/;

/** `call sub_XXXXXX` / `jmp sub_XXXXXX`（★ 尾跳也算一条边：它同样把控制权交出去） */
export const CALL_RE = /\b(call|jmp)\s+(sub_[0-9A-F]{6})\b/;
/** 寄存器名（`call eax` / `jmp ebx` 这类**不是符号目标**，别把它当成"调用了叫 eax 的东西"） */
export const REG_RE = /^(?:e?[abcd]x|e?[sd]i|e?[sb]p|e?sp|r\d+|[abcd][lh]|dword|word|byte|ptr|qword|fs|ds|es|ss|cs|gs)$/i;
/** `call/jmp <符号>`（**任意**符号，含 CRT 的 `___report_gsfailure` 这种）—— 行尾或 `;` 收尾才算 */
export const CALL_SYM_RE = /\b(call|jmp)\s+([A-Za-z_$?@][\w.$?@]*)\s*(?:;|$)/;
/** 解不出目标的调用点（`call eax` / `call dword ptr [...]` / `call [esi+4]` / `call ds:dword_X`）—— 只**计数**，不猜目标 */
export const INDIRECT_CALL_RE = /\b(call|jmp)\s+(?:eax|ebx|ecx|edx|esi|edi|ebp|esp|dword ptr|\[|ds:|es:|ss:|cs:)/;

/** 体内是否出现内存操作数（`[...]`）—— "有没有碰结构"的最小机械判据 */
export const MEM_OPERAND_RE = /\[/;
/**
 * ★ **一个内存操作数里的"引擎尺度位移"**（给 `lib/coverage.mjs` 的 `libraryLikely` 用的机械事实
 * —— 判据与"为什么需要它"见那里的长注释）。
 *
 * 实测口径（全语料 53 万行）：
 * * 这份 IDA 导出里，内存操作数的位移**一律 `…h` 后缀**：`[esi+5D880h]` / `[esi+ecx*8+5D8B4h]`；
 *   含 `[…]` 的行里带 `0x…` 常量的 = **0 行** ⇒ 只认 `h` 形态即可，不必再兜一层。
 * * **基址是栈（`ebp`/`esp`）的操作数要排除**：栈帧可以很大（大局部数组 / `__chkstk`），
 *   它不是"在一个大对象上干活"的证据。⇒ 判据 = "基址不是栈 ∧ 位移 ≥ 0x1000"。
 *   ⚠ 代价（照抄，不掩盖）：`[ebp+…]` 里的大位移**看不见**；这是有意的保守方向。
 */
export const MEM_OPERAND_BODY_RE = /\[([^\]]*)\]/g;
/** 操作数的基址是不是栈：`ebp` / `esp` 开头（`[ebp+var_C]` / `[esp+10h]`） */
export const STACK_BASE_RE = /^\s*e?[sb]p\b/i;
/** `h` 后缀十六进制位移（4..8 位）：`1000h` / `5D880h` / `0AA514h` */
export const DISP_H_RE = /\b([0-9A-F]{4,8})h\b/g;

/**
 * ★ **IDA 自动名的编码规则**（本仓最重要的格式事实之一）：名字的后缀**就是 EA 的低位**。
 * `sub_401000` / `dword_559A68` / `off_51D574` / `unk_51D604` / `loc_401013` / `nullsub_3`（**无 EA**）。
 *
 * ⇒ 两条推论（下游每一个 join 都建在它上面）：
 * 1. **`.c` 里没有任何地址**（Hex-Rays 不打印 EA），但 `.c` 用的是**同一套名字**
 *    ⇒ 名字是 C 侧回连到 EA 的**唯一钥匙**；锚点照旧只锚 EA（`AGENTS.md` §6.2）。
 * 2. **不是每个自动名都带 EA**（`nullsub_3` / `j_401000`）⇒ 解析不到 EA 的必须**报空**，不许猜。
 *
 * ★ 前缀表**实测枚举**（全语料，`.lst` 侧计数）：`loc` 42739 · `sub` 27927 · `dword` 2938 · `stru` 2593 ·
 *   `dbl` 597 · `asc` 433 · `byte` 306 · `unk` 296 · `flt` 266 · `def` 261 · `off` 192 · `locret` 153 ·
 *   `jpt` 118 · `xmmword` 71 · `word` 69 · `qword` 59 · `tbyte` 14。
 *   同一轮还验证了**后缀全都是真实 EA**（全部 17 类的 min/max 都落在 `0x401000..0x5C3528` 内，
 *   越界 0 条）⇒ `eaOfAutoName` 可以放心当"名字 → EA"用。
 *   ⛔ 别只抄 `.c` 侧那 11 种：`.c` 里没有 `locret` / `def` / `jpt` / `tbyte` / `xmmword`（实测），
 *   按 `.c` 去写前缀表会漏掉 `.lst` 侧的存在。
 */
export const AUTO_NAME_RE = /\b(sub|loc|nullsub|dword|byte|word|qword|xmmword|off|unk|flt|dbl|stru|asc|jpt|def|locret|tbyte)_([0-9A-F]{6,8})\b/g;
/** 自动名里**带 EA 的后缀**（6..8 位十六进制）；`nullsub_3` 这种十进制序号**不匹配**（它没有 EA） */
export const AUTO_NAME_EA_RE = /^(sub|loc|dword|byte|word|qword|xmmword|off|unk|flt|dbl|stru|asc|jpt|def|locret|tbyte)_([0-9A-F]{6,8})$/;
/** 名字类别：**代码标签**（不是内存对象）—— 分析"碰了哪些全局"时要排除它们 */
export const CODE_KINDS = new Set(['sub', 'loc', 'locret', 'nullsub', 'def']);
/** 名字类别：**内存对象**（全局变量 / 数据表 / 字符串常量） */
export const DATA_KINDS = new Set(['dword', 'byte', 'word', 'qword', 'xmmword', 'off', 'unk', 'flt', 'dbl', 'stru', 'asc', 'jpt', 'tbyte']);

/**
 * 一行的结构化视图：`{seg, ea, body}`（不是 `段:EA` 形态 ⇒ `null`）。
 * ★ 用 `PREFIX_RE` 唯一那份正则 —— 它已经吃过了"段名以 `.` 开头"的亏。
 */
export function parseListingLine(line) {
  const m = PREFIX_RE.exec(line);
  if (!m) return null;
  return { seg: m[1], ea: Number.parseInt(m[2], 16), body: m[3] ?? '' };
}

/**
 * MSVC **修饰名 → 可读名**：`_WinMain@16` ⇒ `WinMain`。
 * ★ Hex-Rays 打印函数**头**时用可读名（`int __stdcall WinMain(...)`），而 `.lst` 的 `proc near` 用修饰名
 * ⇒ 只按符号名查 `.c` 会"找不到定义"，而那**不是**"没有 C 体"（实测 6 个这类函数）。
 * ⛔ 它只做这一种最常见形态；`??2@YAPAXI@Z` 这类**原样返回**（碰不到就如实说"没有定义"，不许猜）。
 */
export function undecorate(sym) {
  const m = /^[_@]([A-Za-z_]\w*)@\d+$/.exec(sym);
  return m ? m[1] : sym;
}

/**
 * 自动名 → EA（不是自动名 ⇒ `null`，**不许**猜）。
 * `loc_401013` 也给 EA：它是**代码内部标签**，属于它所在的那个函数 —— 归属由 `catalog` 用
 * `functionOfEa` 算，不在这里猜。
 */
export function eaOfAutoName(name) {
  const m = AUTO_NAME_EA_RE.exec(name);
  return m ? Number.parseInt(m[2], 16) : null;
}
