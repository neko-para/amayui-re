/**
 * tools/lib/anchor-lst.mjs —— 台账 `bin` 锚的**站点口径**：机器可执行的判据
 *
 * ## 口径（用户已批准，2026-10-11）
 * * **默认**：`bin` 锚只许落**站点起点** = `.lst` 里**行首那条真指令**的地址
 *   （另有"折叠站点"：该 EA 处只有 `;` 注释行 / 孤立函数块 —— 它是 listing 的**位置**，同样算站点起点）。
 * * **例外**：**落在指令中间**的锚必须（a）在 claim 里**说清是哪条指令的第几个字节**，
 *   且（b）带 `len` + `sha256` —— 满足者登记进 `AUDIT_ALLOWLIST`（数据，逐条带 `len`/`sha256`）。
 *   ⇒ 本判据的落点：**B 类锚不在 allowlist 里 ⇒ 红**。
 * * 为什么默认只许站点起点：锚 EA 与 claim 引用的地址错位（0x1000 级 / 指令中间）在只看"EA 能不能
 *   映射成偏移"时**完全看不出来**（映照旧成立、字节也读得到）—— 那是需求单
 *   `REQ-01M4N3QC4SYG4C36QDBQCFRMSW` 的题面。判据必须落在"这个 EA 在语料里**是什么**"上。
 *
 * ## 分类（`cls`）—— 照普查踩出来的坑写，别简化
 * `cls = 不在语料?'D' : 是行首真指令?'A' : 有 listing 行但无真指令?'S' : 落在前驱真指令的字节区间内?'B' : 'E'`
 * * `len = nextRealInsn(ea) − ea`（**取不到 ⇒ null**，不猜）；
 * * ★ **合并同一 EA 的多行**：IDA 会在同一个 EA 上打 `proc near` / 标签行 / 栈变量定义 / 指令 —— 分类必须
 *   把该 EA 的**所有**行列在一起判，不是逐行判（实测：`sub_48F000` 那个 EA 上有 9 行）；
 * * ★ **假指令行要排除**：`proc near` / `endp` / `名字= dword ptr 8`（栈变量定义）/ 段与汇编指示符
 *   （`assume` / `segment` / `.686p` / `align` / `db` …）/ 纯标签行 —— 它们**不是指令**，
 *   把它们算成"行首真指令"会让"锚落在指令中间"整类消失（实测盲点）；
 * * ★ **折叠站点是真站点**：`.text:004F0F4F ; [00000106 BYTES: COLLAPSED FUNCTION ___report_gsfailure…]`
 *   与孤立 CHUNK（`; START OF FUNCTION CHUNK FOR sub_XXXXXX`，没有 `proc near` 头）都**保守**判成 `S`
 *   （= 站点），不算"落在指令中间"；`len` 取到 listing 的下一条位置（= 折叠块的范围）；
 * * `D` = EA 落在语料地址范围之外（**不在语料**）；`E` = 在范围里，但既不是行首也不是任何前驱指令的
 *   字节（数据 / 空隙）—— 两者都是**红**（锚必须落站点）。
 *
 * ★ 本模块**不碰 fs**：listing 文本、锚点、摘要函数都由调用方给 ⇒ 合成语料与真语料走**同一份判据**。
 */

/** 假指令行（不是指令的 listing 行）—— 逐条都有实测理由，见文件头 */
const FAKE_LINE_RES = [
  /\bproc\s+(near|far)\b/i, // sub_48F000 proc near
  /^\S*\s*\bendp\b/i, // sub_48F000 endp
  /^[\w.$?@]+\s*=\s*(byte|word|dword|qword|tbyte|flt|dbl|struc|db|dw|dd)\b/i, // Buffer= byte ptr -10h / arg_0= dword ptr 8
  /^(assume|align|public|extrn|include|segment|ends|db|dd|dw|dt|org|end|use32|even|ends)\b/i,
  /^\S+\s+(segment|ends)\b/i, // _text segment para public 'CODE' use32 / _text ends
  /^\.\w/, // .686p / .model flat / .mmx
  /^[\w.$?@]+:$/, // 纯标签行（loc_48F02F:）
];

/** listing 行首的形态：`<段>:<8 位十六进制地址><空白><正文>` */
export const LST_LOC_RE = /^([A-Za-z_.][\w.]*):([0-9A-F]{8})\s+(.*)$/;

export const isCommentLine = (body) => body.startsWith(';');

/** 该行是不是**真指令**（排掉假指令行）。导出是为了让守卫能逐条钉住这张表。 */
export function isRealInsnLine(body) {
  if (typeof body !== 'string' || isCommentLine(body)) return false;
  return !FAKE_LINE_RES.some((re) => re.test(body));
}

/** 折叠 / 孤立块的注释行（保守判成站点，不算"落在指令中间"） */
export const COLLAPSED_RE = /COLLAPSED FUNCTION|START OF FUNCTION CHUNK/i;

/**
 * 解析 listing 文本 ⇒ 索引。
 * @returns {{byEa:Map<number,{bodies:string[],real:boolean,collapsed:boolean}>,
 *            realStarts:number[], realSet:Set<number>, procStarts:number[],
 *            minStart:number|null, maxStart:number|null, lines:number}}
 */
export function parseListingText(text) {
  const byEa = new Map();
  const procStarts = [];
  let lines = 0;
  for (const raw of String(text).split(/\r?\n/)) {
    const m = LST_LOC_RE.exec(raw);
    if (!m) continue;
    lines += 1;
    const ea = parseInt(m[2], 16);
    const body = m[3].trim();
    let e = byEa.get(ea);
    if (!e) {
      e = { bodies: [], real: false, collapsed: false };
      byEa.set(ea, e);
    }
    e.bodies.push(body);
    if (isRealInsnLine(body)) e.real = true;
    if (isCommentLine(body) && COLLAPSED_RE.test(body)) e.collapsed = true;
    if (/\bproc\s+(near|far)\b/i.test(body)) procStarts.push(ea);
  }
  const realStarts = [...byEa.entries()].filter(([, v]) => v.real).map(([ea]) => ea).sort((a, b) => a - b);
  procStarts.sort((a, b) => a - b);
  return {
    byEa,
    realStarts,
    realSet: new Set(realStarts),
    procStarts,
    minStart: realStarts.length ? realStarts[0] : null,
    maxStart: realStarts.length ? realStarts[realStarts.length - 1] : null,
    lines,
  };
}

/** 第一个 **严格大于** `ea` 的真指令地址（没有 ⇒ null） */
export function nextRealInsn(idx, ea) {
  const a = idx.realStarts;
  let lo = 0;
  let hi = a.length;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (a[mid] <= ea) lo = mid + 1;
    else hi = mid;
  }
  return lo < a.length ? a[lo] : null;
}

/** 最后一个 **严格小于** `ea` 的真指令地址（没有 ⇒ null） */
export function prevRealInsn(idx, ea) {
  const a = idx.realStarts;
  let lo = 0;
  let hi = a.length;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (a[mid] < ea) lo = mid + 1;
    else hi = mid;
  }
  return lo > 0 ? a[lo - 1] : null;
}

/** 该 EA 起那条指令的字节数（`nextRealInsn(ea) − ea`）；取不到 ⇒ null（不猜） */
export function insnLenAt(idx, ea) {
  const n = nextRealInsn(idx, ea);
  return n === null ? null : n - ea;
}

/** 包含该 EA 的函数（listing 语义：最后一个 `proc near` 起、且地址 ≤ EA）—— 没有 ⇒ null */
export function enclosingProc(idx, ea) {
  const a = idx.procStarts;
  let lo = 0;
  let hi = a.length;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (a[mid] <= ea) lo = mid + 1;
    else hi = mid;
  }
  return lo > 0 ? a[lo - 1] : null;
}

/** 该指令的上界（= 下一条真指令），取不到 ⇒ EA + 15（x86 单指令最长 15 字节 ⇒ 保守上界） */
export function insnEnd(idx, ea) {
  const n = nextRealInsn(idx, ea);
  return n === null ? ea + 15 : n;
}

/**
 * 包含该 EA 的函数的**范围** `[start, end)`（listing 语义：`end` = 下一个 `proc near` 的地址）。
 * ★ 别拿 `insnEnd(proc)` 当函数体上界（那是"proc 行的下一条指令"，实测把整类 C 判据都放行了 ⇒ 误报 118 条）。
 */
export function procRange(idx, ea) {
  const start = enclosingProc(idx, ea);
  if (start === null) return null;
  const i = idx.procStarts.indexOf(start);
  const end = i + 1 < idx.procStarts.length ? idx.procStarts[i + 1] : start + 0x4000;
  return { start, end };
}

/**
 * 给一个 EA 分类。
 * @returns {{cls:'A'|'S'|'B'|'D'|'E', len:number|null, prev?:number, prevLen?:number, off?:number, bodies?:string[]}}
 */
export function classifyEa(idx, ea) {
  if (idx.realSet.has(ea)) return { cls: 'A', len: insnLenAt(idx, ea) };
  const e = idx.byEa.get(ea);
  if (e && e.bodies.length) {
    // 有 listing 行、但一行真指令都没有：折叠块 / 孤立 CHUNK / 纯数据标签 —— ★ 保守判成站点
    return { cls: 'S', len: insnLenAt(idx, ea), bodies: e.bodies.slice(0, 4) };
  }
  if (idx.minStart === null || ea < idx.minStart || ea > idx.maxStart + 15) {
    return { cls: 'D', len: null }; // 不在语料的地址范围里
  }
  const p = prevRealInsn(idx, ea);
  if (p !== null) {
    const n = nextRealInsn(idx, p);
    const plen = n === null ? null : n - p;
    if (plen !== null && plen <= 15 && ea < p + plen) {
      return { cls: 'B', len: insnLenAt(idx, ea), prev: p, prevLen: plen, off: ea - p };
    }
  }
  return { cls: 'E', len: null };
}

/** claim 里**逐字点名**的代码地址（`0x…` / `sub_…` / `loc_…` —— 台账既有的引用口径） */
export function citedAddrs(text) {
  const out = new Set();
  for (const m of String(text ?? '').matchAll(/(?:0[xX]|_)([0-9A-Fa-f]{6})(?![0-9A-Fa-f])/g)) {
    out.add(parseInt(m[1], 16));
  }
  return out;
}

/**
 * claim 是否在**解释某条指令的第 N 个字节**（只有这种情况才允许锚落指令中间）。
 * ★ 它只是**人读的旁证**：判据的落点是 `AUDIT_ALLOWLIST`（数据）—— 不许拿正则当准入。
 */
export const BYTE_PHRASE_RE = /第\s*[0-9一二三四五六七八九十]+\s*(?:个)?字节|位移字节|\+\s*[0-9]+\s*的(?:那个)?字节/;

/**
 * 0x1000 级错位 = **恰好差一页**（`|a − b| === 0x1000`）。
 * ★ 为什么不是"差在一页到两页之间"（第一版，实测**误报 118 条**）：`.text` 有 1.1 MB、一条 claim 常点名
 * 十几个地址 ⇒ "某个点名地址与某个锚 EA 差在一页上下"在真数据里**到处都是**（0x45D660 与 0x45EC60 就是
 * 两个不相干的函数，差 0x1600）。抄错一页的**签名**是**恰好** ±0x1000 —— 实测那条真错
 * （`0x432A70` → claim 点名的 `sub_433A70`）正是如此。判据收紧到"恰好一页"后，误报从 118 条降到可裁决的量级。
 */
export const isPageLevelOffset = (a, b) => Math.abs(a - b) === 0x1000;

/**
 * ★ **刻意字节锚的白名单**（口径的第 2 类）—— 落指令中间**且** claim 逐字说清了"第 N 个字节"。
 *
 * 每条的字段（**逐条都要有**，缺一个就等于"这条观察验不了"）：
 *   `{ id, subject, path, ea, len, sha256, byteOf: { insn, off, text }, why }`
 * * `ea` / `len` / `sha256` —— 与 `bin` 锚逐字相同（守卫按**现算**复核：EA 挪一字节就红）；
 * * `byteOf.insn` = 它落在哪条指令（行首 EA）；`byteOf.off` = 第几个字节（0 基）；`byteOf.text` = claim 里
 *   说这句话的**逐字片段**（人复核用 —— 没有这句话就不该在白名单里）。
 *
 * ★ **实测（2026-10-11 机械复核）本表为空**：在场 345 个 `reference` `bin` 锚里 `cls=B` 的 **0** 个
 *   （344 个行首真指令 + 1 个折叠站点）。31 个"曾经落指令中间"的锚，逐条读 `replaces` 链上的**原始
 *   claim** 后**没有一条**点名字节 ⇒ 全部按口径第 3 步吸附到行首（r02–r11 的订正版），一条都不进本表。
 *   ⇒ 表空不是"没做"，是**判据跑完的结果**；加条目要同时给出上面每一列。
 */
export const AUDIT_ALLOWLIST = [];

/**
 * ★ **具名例外**（口径第 3 步之外的"保留"）：EA 已是站点起点、但 claim **没有**点名它
 * （= 旧口径下的"待裁决"）。新口径下**站点起点默认合法**，所以它们不需要订正；
 * 这里逐条登记**裁决理由**，守卫按"每条在场锚最多只能命中一条例外"机械复核。
 *
 * 字段：`{ id, ea, why }`（`why` 必须具体到"这条锚与 claim 的哪句话有关"，不许写"不确定"）。
 */
export const AUDIT_SITE_EXCEPTIONS = [
  // ── 旧 EA 落指令中间 ⇒ 按口径第 3 步吸附到行首（15 条）──
  { id: '01M4NEX2AM1H3Q4P6P5D5V4J12', ea: 0x43485d, was: 0x434860, verdict: '订正', why: '旧 EA 0x434860 落在 0x43485D 的第 3 个字节（口径第 3 步：吸附到该指令行首）⇒ 落成 `0x43485D`；claim 未点名字节 ⇒ 不进 allowlist' },
  { id: '01M4NEXBT9105R475V0B7S3Z5X', ea: 0x4205fe, was: 0x420600, verdict: '订正', why: '旧 EA 0x420600 落在 0x4205FE 的第 2 个字节（口径第 3 步：吸附到该指令行首）⇒ 落成 `0x4205FE`；claim 未点名字节 ⇒ 不进 allowlist' },
  { id: '01M4NEXJV8687M1B4N6C2R1Q5G', ea: 0x417756, was: 0x417759, verdict: '订正', why: '旧 EA 0x417759 落在 0x417756 的第 3 个字节（口径第 3 步：吸附到该指令行首）⇒ 落成 `0x417756`；claim 未点名字节 ⇒ 不进 allowlist' },
  { id: '01M4NEYKXH362H4F5A550J1H4J', ea: 0x418a39, was: 0x418a3a, verdict: '订正', why: '旧 EA 0x418A3A 落在 0x418A39 的第 1 个字节（口径第 3 步：吸附到该指令行首）⇒ 落成 `0x418A39`；claim 未点名字节 ⇒ 不进 allowlist' },
  { id: '01M4NEZEX95R36543X1J4C5A0R', ea: 0x4910f0, was: 0x4910f2, verdict: '订正', why: '旧 EA 0x4910F2 落在 0x4910F0 的第 2 个字节（口径第 3 步：吸附到该指令行首）⇒ 落成 `0x4910F0`；claim 未点名字节 ⇒ 不进 allowlist' },
  { id: '01M4NEZEX95R36543X1J4C5A0R', ea: 0x49113d, was: 0x491140, verdict: '订正', why: '旧 EA 0x491140 落在 0x49113D 的第 3 个字节（口径第 3 步：吸附到该指令行首）⇒ 落成 `0x49113D`；claim 未点名字节 ⇒ 不进 allowlist' },
  { id: '01M4NEZEX95R36543X1J4C5A0R', ea: 0x491161, was: 0x491165, verdict: '订正', why: '旧 EA 0x491165 落在 0x491161 的第 4 个字节（口径第 3 步：吸附到该指令行首）⇒ 落成 `0x491161`；claim 未点名字节 ⇒ 不进 allowlist' },
  { id: '01M4NEZEX95R36543X1J4C5A0R', ea: 0x491167, was: 0x491168, verdict: '订正', why: '旧 EA 0x491168 落在 0x491167 的第 1 个字节（口径第 3 步：吸附到该指令行首）⇒ 落成 `0x491167`；claim 未点名字节 ⇒ 不进 allowlist' },
  { id: '01M4NEZEX95R36543X1J4C5A0R', ea: 0x418a3d, was: 0x418a3e, verdict: '订正', why: '旧 EA 0x418A3E 落在 0x418A3D 的第 1 个字节（口径第 3 步：吸附到该指令行首）⇒ 落成 `0x418A3D`；claim 未点名字节 ⇒ 不进 allowlist' },
  { id: '01M4NEZEX95R36543X1J4C5A0R', ea: 0x43505d, was: 0x435060, verdict: '订正', why: '旧 EA 0x435060 落在 0x43505D 的第 3 个字节（口径第 3 步：吸附到该指令行首）⇒ 落成 `0x43505D`；claim 未点名字节 ⇒ 不进 allowlist' },
  { id: '01M4NEZEX95R36543X1J4C5A0R', ea: 0x43506f, was: 0x435070, verdict: '订正', why: '旧 EA 0x435070 落在 0x43506F 的第 1 个字节（口径第 3 步：吸附到该指令行首）⇒ 落成 `0x43506F`；claim 未点名字节 ⇒ 不进 allowlist' },
  { id: '01M4NEZEX95R36543X1J4C5A0R', ea: 0x42cfde, was: 0x42cfe0, verdict: '订正', why: '旧 EA 0x42CFE0 落在 0x42CFDE 的第 2 个字节（口径第 3 步：吸附到该指令行首）⇒ 落成 `0x42CFDE`；claim 未点名字节 ⇒ 不进 allowlist' },
  { id: '01M4NEZQ5Q1J262S7044034S2P', ea: 0x407eff, was: 0x407f00, verdict: '订正', why: '旧 EA 0x407F00 落在 0x407EFF 的第 1 个字节（口径第 3 步：吸附到该指令行首）⇒ 落成 `0x407EFF`；claim 未点名字节 ⇒ 不进 allowlist' },
  { id: '01M4NEZQ5Q1J262S7044034S2P', ea: 0x407ef9, was: 0x407efa, verdict: '订正', why: '旧 EA 0x407EFA 落在 0x407EF9 的第 1 个字节（口径第 3 步：吸附到该指令行首）⇒ 落成 `0x407EF9`；claim 未点名字节 ⇒ 不进 allowlist' },
  { id: '01M4NEZZRE732Q5N0D4874693E', ea: 0x4154bf, was: 0x4154c0, verdict: '订正', why: '旧 EA 0x4154C0 落在 0x4154BF 的第 1 个字节（口径第 3 步：吸附到该指令行首）⇒ 落成 `0x4154BF`；claim 未点名字节 ⇒ 不进 allowlist' },
  // ── 本来就是站点起点、claim 未逐字引用 ⇒ 口径下**默认合法**，逐条登记裁决理由（10 条）──
  { id: '01M4NEXT8Z7Y2E3G2P2B160B0M', ea: 0x48f032, was: 0x48f032, verdict: '保留', why: 'EA 是 `.lst` 行首真指令（`push eax`，`_sprintf_s` 那个实参序列里的一条）；claim 只逐字列了同段另外几条指令 —— 口径下**站点起点默认合法**，不订正' },
  { id: '01M4NEXT8Z7Y2E3G2P2B160B0M', ea: 0x48f03d, was: 0x48f03d, verdict: '保留', why: 'EA 是 `.lst` 行首真指令（`push ecx ; Buffer`，同一条实参序列）；claim 未逐字引用它 —— 口径下**站点起点默认合法**，不订正' },
  { id: '01M4NEZ2XH7P0H3V5R436P5F5N', ea: 0x45d284, was: 0x45d284, verdict: '保留', why: 'EA 是 `.lst` 行首真指令（`pop edi`，落在 claim 点名的 `sub_45D240` 体尾 0x45D280+ 之后）；claim 未逐字引用它 —— 口径下**站点起点默认合法**，不订正' },
  { id: '01M4NEZ2XH7P0H3V5R436P5F5N', ea: 0x45d2c1, was: 0x45d2c1, verdict: '保留', why: 'EA 是 `.lst` 行首真指令（`mov ebp, esp`，SEH 序言）；claim 未逐字引用它 —— 口径下**站点起点默认合法**，不订正' },
  { id: '01M4NEZEX95R36543X1J4C5A0R', ea: 0x49115b, was: 0x49115b, verdict: '保留', why: 'EA 是 `.lst` 行首真指令（`mov ecx, dword_5298C0`，在 `sub_491060` 体内）；claim 未逐字引用它 —— 口径下**站点起点默认合法**，不订正' },
  { id: '01M4NEZEX95R36543X1J4C5A0R', ea: 0x434f60, was: 0x434f60, verdict: '保留', why: 'EA 是 `sub_434F60` 的起点（`push ebp`）—— claim 点名的 `0x434F8F` 就在这个函数体内（`0x1a2 save-int` 的调用点）⇒ 与 claim 直接相关；站点起点默认合法' },
  { id: '01M4NEZEX95R36543X1J4C5A0R', ea: 0x42cffd, was: 0x42cffd, verdict: '保留', why: 'EA 是 `.lst` 行首真指令（`call sub_4E73EB`，/GS cookie 校验桩的尾调用）；claim 未逐字引用它 —— 口径下**站点起点默认合法**，不订正' },
  { id: '01M4NEZQ5Q1J262S7044034S2P', ea: 0x407fbd, was: 0x407fbd, verdict: '保留', why: 'EA 是 `.lst` 行首真指令（`jnz short loc_407FC5`）；claim 未逐字引用它 —— 口径下**站点起点默认合法**，不订正' },
  { id: '01M4NEZQ5Q1J262S7044034S2P', ea: 0x407fc0, was: 0x407fc0, verdict: '保留', why: 'EA 是 `.lst` 行首真指令（`mov eax, 8007007Ah`）；claim 未逐字引用它 —— 口径下**站点起点默认合法**，不订正' },
  { id: '01M4NF775K281W2T4M5M5N0X6C', ea: 0x40ef56, was: 0x40ef56, verdict: '保留', why: 'EA 是 `.lst` 行首真指令（`call sub_4E73EB`，/GS cookie 校验桩的尾调用，在 `sub_40ED40` 体内）；claim 未逐字引用它 —— 口径下**站点起点默认合法**，不订正' },
];

/**
 * ★ C 类（0x1000 级错位）的**具名例外**：claim 里点名的某个地址与锚 EA 差一页、而锚 EA 不在那个函数体内。
 * 它**不是**免检：`why` 要写清为什么这条不是"抄错一页"。
 * 字段：`{ id, ea, cited, why }`。
 */
export const AUDIT_C_EXCEPTIONS = [
  {
    id: '01M4N3PG90355K240A78703C08',
    ea: 0x497620,
    cited: 0x496620,
    why:
      '**这条记录的正文就是**在讲"锚 EA 与工具报的偏移差 0x1000 其实是**两个不同的 EA**"（`0x497620` vs `0x496620`；' +
      '后者是 `0x49661F jz short` 的第 2 个字节）⇒ claim 里同时出现两个 EA 是**题面本身**，锚 `0x497620` 是它对的那一个（`sha256` 可复核）。' +
      '★ 不是"抄错一页"：这条记录正是**发现并订正**那一类的地方（订正版 `KN-01M4N3NTQG2S4X6T0R430F3G04`）。',
  },
  {
    id: '01M4NEZEX95R36543X1J4C5A0R',
    ea: 0x4910f0,
    cited: 0x4900f0,
    why:
      '**巧合**：claim 点名的 `0x4900F0` = `SYS4REG.INI` 存在性探针 `sub_4900F0`（体 0x4900F0–0x4903D3）；' +
      '锚 `0x4910F0` 落在**另一个函数** `sub_491060`（注册表读值助手 —— 栈帧里有 `phkResult`/`cchValueName`/`cbData`/`SubKey`/`ValueName`）体内，' +
      '两者恰好差 0x1000。⇒ 不是抄错一页的判据有两条：① 同一记录的锚 `0x418A30` **被 claim 逐字引用** ⇒ 这条记录**没有整体平移**；' +
      '② 若真"少算一页"，`0x4910F0` 的原文 `0x4900F0 + 0xF0` 会落在探针体内（探针体只有 0x2E3 字节，`+0x1000` 早已越出）⇒ 平移读法**不自洽**。',
  },
];

const findAllow = (allowlist, ea, path) =>
  allowlist.find((x) => x.ea === ea && (x.path === undefined || path === undefined || x.path === path));

/**
 * ★ 门禁核心：把站点口径施加到一批记录上。
 *
 * @param {Array<{id:string, subject?:string, effective?:string, anchor?:object[], claim?:string,
 *                substantiveClaim?:string}>} entries
 * @param {object} o
 * @param {object} o.idx            `parseListingText()` 的结果
 * @param {Array}  [o.allowlist]    默认 `AUDIT_ALLOWLIST`
 * @param {Array}  [o.siteExceptions]
 * @param {Array}  [o.cExceptions]
 * @param {(path:string, ea:number, len:number)=>string|null} [o.digestOf]
 *        给 `bin` 锚现算摘要的回调（`null` = 该文件取不到 ⇒ **降级**：只判 EA 成员关系，不判摘要）
 * @returns {{problems:Array<{code:string,id:string,ea:number,why:string}>,
 *            counts:Record<string,number>, cCandidates:Array, allowlistChecked:number,
 *            allowlistStale:Array, degraded:boolean}}
 */
export function auditEntries(entries, o) {
  const {
    idx,
    allowlist = AUDIT_ALLOWLIST,
    siteExceptions = AUDIT_SITE_EXCEPTIONS,
    cExceptions = AUDIT_C_EXCEPTIONS,
    digestOf = null,
  } = o;
  const problems = [];
  const counts = { A: 0, S: 0, B: 0, D: 0, E: 0, anchors: 0, records: 0 };
  const cCandidates = [];
  const hitAllow = new Set();
  let degraded = false;
  const allowlistChecked = [];

  for (const e of entries) {
    if (!e || e.effective === 'retracted') continue;
    counts.records += 1;
    const claimText = e.substantiveClaim ?? e.claim ?? '';
    const cited = citedAddrs(claimText);
    for (const a of e.anchor ?? []) {
      if (!a || a.type !== 'bin') continue;
      counts.anchors += 1;
      const c = classifyEa(idx, a.ea);
      counts[c.cls] += 1;
      const allow = findAllow(allowlist, a.ea, a.path);
      if (allow) {
        hitAllow.add(allow.ea);
        allowlistChecked.push(allow.ea);
        // ★ allowlist 条目本身要能复核：EA 必须仍在指令中间，`len` 必须等于现算值
        if (c.cls !== 'B') {
          problems.push({ code: 'allow-stale', id: e.id, ea: a.ea, why: `allowlist 里的 EA 现在分类是 ${c.cls}（不再落在指令中间）⇒ 陈旧条目，删掉它（站点起点不需要豁免）` });
        }
        if (c.len !== null && allow.len !== c.len) {
          problems.push({ code: 'allow-len', id: e.id, ea: a.ea, why: `allowlist 的 len=${allow.len} 与现算 ${c.len} 不一致（指令长度变了 ⇒ 那不是同一条指令）` });
        }
        if (digestOf && a.path !== undefined) {
          const got = digestOf(a.path, allow.ea, allow.len);
          if (got === null) degraded = true;
          else if (got !== allow.sha256) {
            problems.push({ code: 'allow-sha', id: e.id, ea: a.ea, why: `allowlist 的 sha256=${String(allow.sha256).slice(0, 16)}… 与现算 ${String(got).slice(0, 16)}… 不一致（字节被换过 / EA 挪过）` });
          }
        }
        continue; // 已在白名单 ⇒ 不再当"落指令中间"报红
      }
      if (c.cls === 'B') {
        problems.push({
          code: 'mid-insn',
          id: e.id,
          ea: a.ea,
          why: `锚落在指令中间（0x${(c.prev ?? 0).toString(16)} 的第 ${c.off} 个字节）且不在 allowlist 里 —— ` +
            '口径：落指令中间必须在 claim 里说清"是哪条指令的第几个字节"并带 len+sha256（登记进 AUDIT_ALLOWLIST），否则吸附到行首',
        });
      } else if (c.cls === 'D') {
        problems.push({ code: 'not-in-corpus', id: e.id, ea: a.ea, why: 'EA 不在 `.lst` 的地址范围里（口径：锚必须落站点）' });
      } else if (c.cls === 'E') {
        problems.push({ code: 'not-a-site', id: e.id, ea: a.ea, why: 'EA 既不是行首真指令、也不落在任何前驱指令的字节区间内（数据 / 空隙）⇒ 不是站点' });
      }
      // ── C 类：claim 点名的地址与锚 EA 差 0x1000 级，且锚 EA 不在那个函数体内
      for (const x of cited) {
        if (x === a.ea || !isPageLevelOffset(x, a.ea)) continue;
        const pr = procRange(idx, x);
        const inThatProc = pr !== null && a.ea >= pr.start && a.ea < pr.end;
        if (inThatProc) continue;
        // ★ 候选**一律**记下来（连已裁决的例外一起）：守卫要拿"实测候选集合"和"例外表"做**集合相等**断言
        cCandidates.push({ id: e.id, ea: a.ea, cited: x, subject: e.subject });
        if (cExceptions.some((y) => y.id === e.id && y.ea === a.ea && y.cited === x)) continue;
        problems.push({
          code: 'page-offset',
          id: e.id,
          ea: a.ea,
          why: `claim 点名的 0x${x.toString(16)} 与锚 EA 差 ${Math.abs(a.ea - x)}（0x1000 级），且 EA 不在 0x${(pr?.start ?? 0).toString(16)}–0x${(pr?.end ?? 0).toString(16)} 体内 ⇒ 疑似抄错一页`,
        });
      }
    }
  }
  const allowlistStale = allowlist.filter((x) => !hitAllow.has(x.ea));
  for (const s of allowlistStale) {
    problems.push({ code: 'allow-unused', id: s.id ?? '(allowlist)', ea: s.ea, why: 'allowlist 里这条没有对应的在场锚 ⇒ 陈旧（白名单不许留尸体）' });
  }
  return { problems, counts, cCandidates, allowlistChecked, allowlistStale, degraded };
}
