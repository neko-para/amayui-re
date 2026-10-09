/**
 * tools/lib/disasm.mjs —— **反汇编语料的机械提取**（EA → 行号 → 有界上下文）
 *
 * ## 它解决什么问题
 * 语料是**一次性产物**（IDA 导出）：`.text` 36.5 万行、`.data` 7.6 万行、共 53 万行 / 17.6 MB。
 * 让分析**在这么大一份文件里随意漫游**是错的 —— 既不可复现、也无法复核、还会烧掉上下文。
 * 所以先做**机械提取**：给定 **EA**（或一个名字 / 一个字符串），确定性地取出**一小段**上下文。
 *
 * ## 三条口径（决定了本模块的每个函数签名）
 * 1. **锚点锚 EA，不锚行号**：本模块**现算** `EA → 行号` 的映射。换一次反汇编只重建映射，不改任何锚。
 *    ★ 所以 `index` 是**派生缓存**（落 `.cache/`、gitignore、可删可重建），**不是**真源。
 * 2. **上下文是派生的内存/文件视图**：语料**只读**，任何"解析友好化"都不得落回语料文件（`AGENTS.md` §1 第 5 条）。
 * 3. **有界**：每次提取都必须有上限（`lines`），并在被截断时**明说截断了** ——
 *    绝不"读不到就当空"、也绝不悄悄给出不完整的切片。
 *
 * ## 语料的盘上事实（IDC 导出形态，实测）
 * * 每行前缀是 **`.text:00401000`** 这种 `段:EA` 形式；**同一 EA 会连续出现在多行**上。
 * * `Imagebase 400000` 写在头部注释里 ⇒ **EA 是虚拟地址**，要读字节得先映射到文件偏移（PE 节表，见 `ledger.mjs` 的 `peOffsetOf`）。
 * * 实测段：`.text` / `.data` / `seg002` / `.idata` / `poly` / `seg003`（**不止 .text**）⇒ 索引必须按段分组。
 */
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';

export const DOMAIN = {
  // ★ 域 id 不叫 `disasm` —— 那个已被 `tools/disasm-recode.mjs`（语料**保真**：转写/打 zip/反解）占用。
  //   同一个词在下游是两件事，所以两个域各自有名字：`disasm`=保真，`disasm-at`=**只读定位与切片**。
  id: 'disasm-at',
  title: '反汇编语料的机械提取（EA → 行号 → 有界上下文；**只读**，不改语料）',
  data: [
    '`corpus/disasm/files/*.lst` / `*.c`（**只读**语料；解压产物，gitignore）',
    '`.cache/disasm/*.idx.txt`（**派生**索引：段 → `[起始行, EA]` 稀疏表；可删可重建）',
  ],
  access: 'r（**只读**：本工具不改语料、只读它；索引写 `.cache/`，不入库）',
  tool: 'tools/disasm.mjs',
};

export const OPERATIONS = [
  { name: 'stats', argv: ['--stats'], mutates: false, summary: '语料盘点：段 / 行数 / EA 范围 / 行号→EA 是否单调' },
  { name: 'at', argv: ['--at'], mutates: false, summary: '★ **取有界上下文**：`--ea <EA> [--lines N]`（被截断会明说）' },
  { name: 'span', argv: ['--span'], mutates: false, summary: '一个名字/串的**行号区间**：`--match <子串>`（或 `--regex`）' },
  { name: 'search', argv: ['--search'], mutates: false, summary: '机械检索：`--match <子串>` 找出现的**全部位置**（带行号，可 `--limit`）' },
  { name: 'cases', argv: ['--cases'], mutates: false, summary: '★ **枚举函数里的 switch / 跳转表**：`--ea <EA>` ⇒ case 数 + 每个 case 的目标（类型分派 / opcode 分派全靠它）' },
  { name: 'pseudo', argv: ['--pseudo'], mutates: false, summary: '★ **一层工作流**：`--ea <EA>`（或 `--sym sub_XXXXXX`）⇒ 它**所属函数**的 Hex-Rays **C 体** + `.lst` 行区间（`--ea` 不是函数起点时会明说）' },
  { name: 'index', argv: ['--index'], mutates: true, summary: '建/刷新派生索引到 `.cache/`（缺省 dry-run）[--write]' },
];

/**
 * 一行开头的 `段:EA` 前缀（IDA 导出形态）。
 * ★ **段名可以以 `.` 开头**（`.text` / `.data` / `.idata`）—— 第一版把首字符限成 `[A-Za-z_]`，
 *   于是 `.text` 36.5 万行 + `.data` 7.6 万行**整段漏掉**（实测：索引里只剩 seg002/seg003/poly）。
 *   这条错误很危险：漏掉的段在 `--at` 里表现为"地址不在语料覆盖范围内"，**看起来像正常的边界提示**。
 */
const PREFIX_RE = /^([A-Za-z_.][\w.]{0,30}):([0-9A-Fa-f]{8})\s?(.*)$/;
/** 头部注释里的 Imagebase */
const IMAGEBASE_RE = /^\S+\s*;\s*Imagebase\s*:\s*([0-9A-Fa-f]+)/;

export const DEFAULT_MAX_LINES = 120;

// ─────────────────────────────────────────────────────────── 解析

/** 把用户给的 EA 解析成整数：接受 `0x401000` / `401000` / `text:401000` 的尾段 */
export function parseEa(s) {
  if (typeof s === 'number') return s >>> 0;
  const t = String(s).trim();
  const m = /^(?:[A-Za-z_][\w.]{0,30}:)?(0[xX])?([0-9A-Fa-f]+)$/.exec(t);
  if (!m) throw new Error(`EA 形态非法：${JSON.stringify(s)}（接受 0x401000 / 401000）`);
  const v = Number.parseInt(m[2], 16);
  if (!Number.isSafeInteger(v)) throw new Error(`EA 超出范围：${s}`);
  return v >>> 0;
}

/**
 * 扫一遍语料，建**索引**：每条被识别为"带地址的行"的记录。
 * @returns {{file:string, bytes:number, sha256:string, lines:number, imagebase:number|null,
 *            segs:Array<{name:string, firstLine:number, lastLine:number, lo:number, hi:number, rows:number}>,
 *            marks:Array<{line:number, seg:string, ea:number}>,   // 只在 EA **变化**时压一条（稀疏表）
 *            monotonic:boolean}}
 */
export function buildIndex(file) {
  const buf = fs.readFileSync(file);
  const sha256 = crypto.createHash('sha256').update(buf).digest('hex');
  const text = buf.toString('utf8');
  const rows = text.split('\n');
  const marks = [];
  const segs = new Map();
  let imagebase = null;
  let lastSeg = null;
  let lastEa = null;

  for (let i = 0; i < rows.length; i += 1) {
    const line = rows[i];
    if (imagebase === null) {
      const ib = IMAGEBASE_RE.exec(line);
      if (ib) imagebase = Number.parseInt(ib[1], 16);
    }
    const m = PREFIX_RE.exec(line);
    if (!m) continue;
    const seg = m[1];
    const ea = Number.parseInt(m[2], 16);
    const lineNo = i + 1;
    let s = segs.get(seg);
    if (!s) {
      s = { name: seg, firstLine: lineNo, lastLine: lineNo, lo: ea, hi: ea, rows: 0 };
      segs.set(seg, s);
    }
    s.lastLine = lineNo;
    s.rows += 1;
    if (ea < s.lo) s.lo = ea;
    if (ea > s.hi) s.hi = ea;
    // 稀疏表：只在"段变了 或 EA 变了"时压一条 ⇒ 53 万行压成几万条
    if (seg !== lastSeg || ea !== lastEa) {
      marks.push({ line: lineNo, seg, ea });
      lastSeg = seg;
      lastEa = ea;
    }
  }

  // 单调性检查：同一段内，压出来的 marks 的 EA 是否单调不降（IDA 导出若重排，这里会红）
  let monotonic = true;
  let prevSeg = null;
  let prevEa = -1;
  for (const k of marks) {
    if (k.seg !== prevSeg) {
      prevSeg = k.seg;
      prevEa = -1;
    }
    if (k.ea < prevEa) monotonic = false;
    prevEa = k.ea;
  }

  return {
    file,
    bytes: buf.length,
    sha256,
    lines: rows.length,
    imagebase,
    // ★ 段表按**文件顺序**排（不是字典序），并补上各自的 marks 下标区间 —— 二者缺一，`--at` 会静默给错
    segs: withSegRanges([...segs.values()].sort((a, b) => a.firstLine - b.firstLine), marks),
    marks,
    monotonic,
  };
}

/**
 * 给段表补上**每段在 marks 里的下标区间**（`[markLo, markHi]`）。
 *
 * ★ 为什么需要它（实测踩过）：marks 是**按文件顺序**压出来的，段在文件里的出现顺序是
 *   `.text → .data → seg002 → seg003 → .idata → poly` —— **不是字典序**。
 *   于是"用 `(seg, ea)` 的字典序当排序键做二分"这个谓词**不单调**：搜索会收敛到 `.text`，
 *   得出"段 `.data` 里没有 ≤ 该 EA 的标记"，而 `.data` 的标记明明就在数组后半段。
 *   ⇒ 正确做法：**先定位段的切片，再在切片内二分**。
 */
export function withSegRanges(segs, marks) {
  const out = segs.map((s) => ({ ...s, markLo: -1, markHi: -1 }));
  const byName = new Map(out.map((s) => [s.name, s]));
  marks.forEach((m, i) => {
    const s = byName.get(m.seg);
    if (!s) return;
    if (s.markLo < 0) s.markLo = i;
    s.markHi = i;
  });
  return out;
}
/** 索引摘要（不含 `marks` —— 那份可能几十万条，别塞进 JSON 报告） */
export const indexSummary = (idx) => ({
  file: idx.file,
  bytes: idx.bytes,
  sha256: idx.sha256,
  lines: idx.lines,
  imagebase: idx.imagebase,
  monotonic: idx.monotonic,
  segs: idx.segs,
  marks: idx.marks.length,
});

/**
 * ★ **索引自洽校验**：解析回来的索引必须"段表与标记对得上"。
 *
 * 为什么非有不可（实测踩过两次，都是段名以 `.` 开头引起的）：
 * 一份**能写出、却读不回来**的索引会静默降级 —— 段表里写着 `.text` 有 40 万行，
 * 而标记里 `.text` 一条都没有，于是 `--at <某个 .text 地址>` 报"没有 ≤ 该 EA 的标记"。
 * 那句提示**听起来像正常的边界情况**，实际是"索引坏了"。
 * ⇒ 校验条件不能只看"非空"，要看**每个段在标记里都出现过**。
 */
export function indexProblems(idx, { maxMarks = 0 } = {}) {
  const p = [];
  if (!idx.segs?.length) p.push('索引里没有段表（# segs 缺失或解析不出）');
  if (!idx.marks?.length) p.push('索引里没有标记');
  const seen = new Set(idx.marks.map((m) => m.seg));
  for (const s of idx.segs ?? []) {
    if (!seen.has(s.name)) p.push(`段表说 ${s.name} 有 ${s.rows} 行，但标记里**一条都没有**（段名/正则不匹配？）`);
  }
  for (const seg of seen) {
    if (!(idx.segs ?? []).some((s) => s.name === seg)) p.push(`标记里有段 ${seg}，但段表里没有它`);
  }
  if (maxMarks && idx.marks.length > maxMarks) p.push(`标记数 ${idx.marks.length} > 上限 ${maxMarks}`);
  return p;
}

/** 落盘形态：段表（一行）+ 稀疏标记（一行一条）—— 文本、可 diff、可重建 */
export function serializeIndex(idx) {
  const L = [
    `# disasm index —— **派生缓存**，可删可重建（真源是语料本身；锚点锚 EA，不锚这里的行号）`,
    `# file ${path.basename(idx.file)}`,
    `# sha256 ${idx.sha256}`,
    `# bytes ${idx.bytes}`,
    `# lines ${idx.lines}`,
    `# imagebase ${idx.imagebase === null ? '?' : `0x${idx.imagebase.toString(16)}`}`,
    // ★ 段表必须写进来：不写的话，从缓存读回来的索引**不知道每个段的 EA 范围**，
    //   于是 `--at <EA>` 判不出"这个地址落在哪个段"（实测踩过：返回 0 行）。
    `# segs ${idx.segs.map((s) => [s.name, s.lo.toString(16), s.hi.toString(16), s.rows, s.firstLine, s.lastLine].join(':')).join(' ')}`,
    ...idx.marks.map((k) => `${k.seg}:${k.ea.toString(16).padStart(8, '0')} ${k.line}`),
  ];
  return `${L.join('\n')}\n`;
}

export function parseIndexText(text) {
  const marks = [];
  let sha256 = null;
  let imagebase = null;
  let lines = null;
  let bytes = null;
  let segs = [];
  for (const line of text.split('\n')) {
    if (line.startsWith('# sha256 ')) sha256 = line.slice(9).trim();
    else if (line.startsWith('# lines ')) lines = Number(line.slice(8).trim());
    else if (line.startsWith('# bytes ')) bytes = Number(line.slice(8).trim());
    else if (line.startsWith('# imagebase ')) imagebase = line.slice(12).trim() === '?' ? null : Number.parseInt(line.slice(12).trim(), 16);
    else if (line.startsWith('# segs ')) {
      segs = line
        .slice(7)
        .trim()
        .split(/\s+/)
        .filter(Boolean)
        .map((t) => {
          const [name, lo, hi, rows, firstLine, lastLine] = t.split(':');
          return {
            name,
            lo: Number.parseInt(lo, 16),
            hi: Number.parseInt(hi, 16),
            rows: Number(rows),
            firstLine: Number(firstLine),
            lastLine: Number(lastLine),
          };
        });
    } else if (line && !line.startsWith('#')) {
      // ★ 段名可以以 `.` 开头（`.text` / `.data`）—— 第一版这里写成 `^([A-Za-z_]…`，
      //   于是**索引能写出、却读不回来**：`.text` 30 万条 mark 静默丢失，`--at` 对 .text 地址
      //   报"没有 ≤ 该 EA 的标记"（听起来像正常边界提示）。同一个错误在本文件里犯了两次（见 PREFIX_RE）。
      const m = /^([A-Za-z_.][\w.]{0,30}):([0-9A-Fa-f]{8}) (\d+)$/.exec(line.trim());
      if (m) marks.push({ seg: m[1], ea: Number.parseInt(m[2], 16), line: Number(m[3]) });
    }
  }
  return { marks, sha256, imagebase, lines, bytes, segs };
}

// ─────────────────────────────────────────────────────────── 查询

/** 二分：在 marks 的 `[from, to]` 闭区间里找最后一个 `ea <= target`（-1 = 没有） */
function floorMark(marks, target, from, to) {
  let lo = from;
  let hi = to;
  let hit = -1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (marks[mid].ea <= target) {
      hit = mid;
      lo = mid + 1;
    } else hi = mid - 1;
  }
  return hit;
}

/**
 * 取一个 **EA 的有界上下文**。★ 被截断时**明说**（不静默给不完整切片）。
 * @returns {{seg:string|null, ea:number, fromLine:number, toLine:number, lines:string[],
 *            truncated:{head:boolean, tail:boolean}, hint:string|null}}
 */
export function contextAt(file, idx, ea, { lines = DEFAULT_MAX_LINES, seg = null } = {}) {
  const target = parseEa(ea);
  const marks = idx.marks;
  // 段：给了就用，没给就找第一个 lo<=ea<=hi 的段
  let useSeg = seg;
  if (!useSeg) {
    const hit = idx.segs.find((s) => target >= s.lo && target <= s.hi);
    if (!hit) {
      return {
        seg: null, ea: target, fromLine: 0, toLine: 0, lines: [],
        truncated: { head: false, tail: false },
        hint: `EA 0x${target.toString(16)} 不落在任何已索引段内（段的 EA 范围见 \`--stats\`）—— 这**不是**"读不到就当空"，是地址本身不在语料覆盖范围内`,
      };
    }
    useSeg = hit.name;
  }
  const segInfo = idx.segs.find((s) => s.name === useSeg);
  // ★ 落地前必须验"这个 EA 真的落在该段范围内"：否则会拿"本段最后一条"当它（静默给错切片）
  if (segInfo && (target < segInfo.lo || target > segInfo.hi)) {
    return {
      seg: useSeg, ea: target, fromLine: 0, toLine: 0, lines: [],
      truncated: { head: false, tail: false },
      hint:
        `EA 0x${target.toString(16)} 不在段 ${useSeg} 的范围内（该段 0x${segInfo.lo.toString(16)}-0x${segInfo.hi.toString(16)}）` +
        ` —— **不许**把"本段最后一条"当它`,
    };
  }
  // ★ 在**段的切片内**二分：marks 按文件顺序排，段的出现顺序（.text→.data→seg002→…）**不是字典序**
  //   ⇒ 用 (seg, ea) 当排序键去全局二分，谓词不单调，会静默得出"没有 ≤ 该 EA 的标记"。
  if (!segInfo || segInfo.markLo === undefined || segInfo.markLo < 0) {
    return {
      seg: useSeg, ea: target, fromLine: 0, toLine: 0, lines: [],
      truncated: { head: false, tail: false },
      hint: `索引里没有段 ${useSeg} 的标记区间 ⇒ 请 \`pnpm tools disasm-at index --write\` 重建索引`,
    };
  }
  const at = floorMark(marks, target, segInfo.markLo, segInfo.markHi);
  if (at < 0) {
    return {
      seg: useSeg, ea: target, fromLine: 0, toLine: 0, lines: [],
      truncated: { head: false, tail: false },
      hint: `段 ${useSeg} 里没有 ≤ 0x${target.toString(16)} 的标记（该 EA 早于本段第一个地址）`,
    };
  }
  const startLine = marks[at].line;
  // 窗口：从该 EA 那一行起，向后取 `lines` 行（**以 EA 变化为界**会更干净，但先给最简单的行窗）
  const all = fs.readFileSync(file, 'utf8').split('\n');
  const from = startLine; // 1-based
  const to = Math.min(all.length, startLine + lines - 1);
  return {
    seg: useSeg,
    ea: target,
    fromLine: from,
    toLine: to,
    lines: all.slice(from - 1, to),
    truncated: { head: from > 1, tail: to < all.length },
    hint: null,
  };
}

/** 机械检索：给一个子串（或正则），返回**全部**命中位置（带行号与所在段/EA） */
export function search(file, { match, regex = false, limit = 200, ctx = 0 } = {}) {
  if (!match) throw new Error('--search 需要 --match <子串>（加 --regex 才当正则）');
  const re = regex ? new RegExp(match) : null;
  const all = fs.readFileSync(file, 'utf8').split('\n');
  const hits = [];
  let total = 0;
  for (let i = 0; i < all.length; i += 1) {
    const line = all[i];
    const ok = re ? re.test(line) : line.includes(match);
    if (!ok) continue;
    total += 1;
    if (hits.length >= limit) continue;
    const m = PREFIX_RE.exec(line);
    hits.push({
      line: i + 1,
      seg: m ? m[1] : null,
      ea: m ? Number.parseInt(m[2], 16) : null,
      text: line.length > 200 ? `${line.slice(0, 200)}…` : line,
    });
  }
  return { total, shown: hits.length, hits, truncated: total > hits.length };
}

/**
 * 机械枚举一个函数里的 **switch / 跳转表**（IDA 导出形态）。
 *
 * ★ 为什么这是"机械提取"的关键一环：语料里所有"按类型分派""按 opcode 分派"都是
 *   `jmp ds:jpt_XXXXXX[reg*4]`（跳转指令）+ **另一个段里**的 `jpt_XXXXXX dd offset loc_XXXXXX`（表体）。
 *   二者都是**纯文本、可枚举**的 ⇒ 不需要读懂任何语义就能拿到"这个 switch 有几个 case、分别去哪"。
 *
 * ★ 实测坑：表体**不在函数的行区间内**（它在 `.data` / `.rdata` 段，离函数几万行）⇒
 *   查表必须按**全文**查，不能在函数 span 里查（第一版就是这么写的，于是 `cases` 恒为 0）。
 *
 * @param {string[]} [fileLines] 可选的全文行缓存（批量分析时复用，省掉重复读 17 MB）
 * @returns {Array<{jptAt:number, insnLine:number, jumpAt:number, tableLine:number, tableSeg:string|null,
 *                  cases:number, targets:Array<{i:number, loc:string, atLine:number}>}>}
 */
export function switchTables(file, idx, ea, { maxLines = 6000, fileLines = null } = {}) {
  const span = spanOfFunction(file, idx, ea, { maxLines });
  const lines = span.lines;
  const all = fileLines ?? fs.readFileSync(file, 'utf8').split('\n');
  const out = [];
  for (let i = 0; i < lines.length; i += 1) {
    const m = /^(\S+):([0-9A-F]{8})\s+jmp\s+ds:jpt_([0-9A-F]{6,8})\[/.exec(lines[i]);
    if (!m) continue;
    const jpt = m[3];
    const jumpAt = Number.parseInt(m[2], 16);
    let tableLine = -1;
    let tableSeg = null;
    const targets = [];
    // ★ 只有表的**第一行**带 `jpt_XXXXXX` 标签，后续行是裸 `dd offset loc_XXXXXX`
    //   （实测：jpt_42AF16 共 12 行，第 1 行带标签、其余 11 行不带）
    //   ⇒ 判据 = "第一行锚定，然后连续吃 `dd offset`，遇到非该形态就停"
    const headRe = new RegExp(`^(\\S+):([0-9A-F]{8})\\s+jpt_${jpt}\\s+dd\\s+offset\\s+(\\S+)`);
    const contRe = /^(\S+):([0-9A-F]{8})\s+dd\s+offset\s+(\S+)/;
    const start = all.findIndex((l) => headRe.test(l));
    if (start >= 0) {
      tableLine = start + 1;
      tableSeg = headRe.exec(all[start])[1];
      for (let k = start; k < all.length; k += 1) {
        const mm = (k === start ? headRe : contRe).exec(all[k]);
        if (!mm) break;
        if (mm[1] !== tableSeg) break; // 跨段即结束（表在段内连续）
        targets.push({ i: targets.length, loc: mm[3], atLine: k + 1 });
      }
    }
    out.push({ jptAt: Number.parseInt(jpt, 16), insnLine: span.fromLine + i, jumpAt, tableLine, tableSeg, cases: targets.length, targets });
  }
  return out;
}

/** 给一个 EA，求它所在**函数的行区间**（`proc near` … `endp`；IDA 的 `; END OF FUNCTION CHUNK` 也认） */
/**
 * 取一个符号/EA 所在函数的**行区间**。
 *
 * ⚠️ **只对"函数起点"的 EA 正确**：它从 EA 那一行**向后**找第一个 `proc near`。
 * EA 落在函数体内时，它找到的是**后一个**函数（`--at --fn` 原先就是这么用它的 ⇒ 那是错的）。
 * ★ 要"**包含**这个 EA 的函数"（EA 在体内也算）请用 `enclosingFunction()`。
 */
export function spanOfFunction(file, idx, ea, { maxLines = 4000 } = {}) {
  const ctx = contextAt(file, idx, ea, { lines: maxLines });
  if (!ctx.seg) return ctx;
  const all = ctx.lines;
  // 起点：往上找最近的 `sub_XXXXXX` 定义行
  const defRe = /^\S+\s+([A-Za-z_][\w.]{0,40})\s+proc near/;
  let head = 0;
  for (let i = 0; i < Math.min(all.length, 400); i += 1) {
    if (defRe.test(all[i])) {
      head = i;
      break;
    }
  }
  // 终点：往下找 `... endp`
  let tail = all.length - 1;
  for (let i = head; i < all.length; i += 1) {
    if (/^\S+\s+[A-Za-z_][\w.]{0,40}\s+endp\b/.test(all[i])) {
      tail = i;
      break;
    }
  }
  return {
    ...ctx,
    fromLine: ctx.fromLine + head,
    toLine: ctx.fromLine + tail,
    lines: all.slice(head, tail + 1),
    truncated: { head: head === 0, tail: tail === all.length - 1 },
  };
}

/**
 * ★ 符号名字符类（三处正则共用一份）：**必须含 `@` / `?` / `$`** —— MSVC 的修饰名会长这样：
 * `_WinMain@16`、`??2@YAPAXI@Z`、`??_7?$Stack@H@@6B@`。
 *
 * ⚠ **实测踩过**（本轮）：旧类 `[A-Za-z_][\w.]{0,40}` **不含 `@`** ⇒ `_WinMain@16 proc near` 不被认作函数头，
 * `enclosingFunction(0x4BAA3A)` **静默**返回**前一个**函数 `sub_4BA6B0`（而 `--pseudo` 还照样打出 C 体）——
 * 那不是"查不到"，是**给了一个看起来对的错答案**，正好违反本层"缺事实要报错"的口径。
 * 判据：`node tools/disasm.mjs --pseudo --ea 0x4BAA3A` 必须报 `_WinMain@16`（`.lst:295818-296907`）。
 */
const SYM = '[A-Za-z_$?@][\\w.$?@]{0,60}';
/**
 * MSVC **修饰名 → `.c` 里的可读名**：`_WinMain@16` ⇒ `WinMain`（`--pseudo` 的 C 体查找用它兜一层）。
 * ★ Hex-Rays 打印函数**头**时用可读名（`int __stdcall WinMain(...)`），而 `.lst` 的 `proc near` 用修饰名
 * ⇒ 只按符号名查 `.c` 会"找不到定义"，而那**不是**"没有 C 体"。
 * ⛔ 它只做这一种最常见形态（`_Name@NN`）；`??2@YAPAXI@Z` 这种留给 `.c` 索引自己去碰（碰不到就如实说"没有定义"）。
 */
const undecorate = (sym) => {
  const m = /^[_@]([A-Za-z_]\w*)@\d+$/.exec(sym);
  return m ? m[1] : sym;
};
const PROC_RE = new RegExp(`^(\\S+)\\s+(${SYM})\\s+proc near`);
/** ★ 段名可以以 `.` 开头（`.text` / `.data`）—— 首字符限 `[A-Za-z_]` 会让 `.text` 整段漏掉（踩过两次） */
const PROC_EA_RE = new RegExp(`^([A-Za-z_.][\\w.]*):([0-9A-Fa-f]{8})\\s+(${SYM})\\s+proc near`);
const ENDP_RE = new RegExp(`^\\S+\\s+(${SYM})\\s+endp\\b`);

/**
 * ★ **包含**这个 EA 的函数（EA 落在函数体内也算），以及它的符号名。
 *
 * 为什么不能拿 `spanOfFunction` 凑合：它只向后看 400 行，且从 EA 那一行起找 ——
 * 实测 `0x40D500`（DEC 公式的锚）**不是函数起点**：它属于 `sub_40CD10`，而那个函数起点在它**前面 683 行**
 * ⇒ 向后找只会撞上**后一个**函数，给出"看起来对、其实错"的区间。
 *
 * @returns {{seg, ea, fromLine, toLine, lines, symbol, symbolEa, truncated}} 或 `contextAt` 的边界提示
 */
export function enclosingFunction(file, idx, ea, { maxLines = 20000 } = {}) {
  const target = parseEa(ea);
  // 先在段表上挡掉"这个 EA 根本不在语料里"（不必读 18 MB 才发现）
  const seg = idx.segs.find((s) => target >= s.lo && target <= s.hi);
  if (!seg) {
    return { seg: null, ea: target, fromLine: 0, toLine: 0, lines: [], symbol: null, symbolEa: null, truncated: { head: false, tail: false }, hint: `EA 0x${target.toString(16)} 不落在任何已索引段内（段的 EA 范围见 \`--stats\`）` };
  }
  const all = fs.readFileSync(file, 'utf8').split('\n');
  // ★ **不许**用 `contextAt` 定位行：它给的是"最近的**前一个 mark**"的行，而 marks 是**稀疏**的
  //   ⇒ 对"函数起点这个 EA"，它会落到**前一个函数体内**，再往回找就把 `sub_40CB00` 当成 `sub_40CD10`
  //   （实测踩过：守卫 `disasm-pseudo.assets.test.mjs` 抓出来的）。
  //   ⇒ 正确地扫一遍：取**最后一个 EA ≤ target** 的 `proc near`。
  let head = -1;
  for (let i = all.length - 1; i >= 0; i -= 1) {
    const m = PROC_EA_RE.exec(all[i]);
    if (m && Number.parseInt(m[2], 16) <= target) {
      head = i;
      break;
    }
  }
  if (head < 0) {
    return { seg: seg.name, ea: target, fromLine: 0, toLine: 0, lines: [], symbol: null, symbolEa: null, truncated: { head: false, tail: false }, hint: `0x${target.toString(16)} 之前没有任何 \`proc near\` ⇒ 它不在函数里` };
  }
  let tail = all.length - 1;
  for (let i = head; i < all.length; i += 1) {
    if (ENDP_RE.test(all[i])) {
      tail = i;
      break;
    }
  }
  const m = PROC_EA_RE.exec(all[head]);
  return {
    seg: seg.name,
    ea: target,
    fromLine: head + 1,
    toLine: tail + 1,
    lines: all.slice(head, Math.min(tail + 1, head + maxLines)),
    symbol: m[3],
    symbolEa: Number.parseInt(m[2], 16),
    truncated: { head: head > 0, tail: tail === all.length - 1 },
  };
}

// ───────────────────────────────────────────────────── Hex-Rays 伪代码（.c）

/**
 * `.c` 里的一行**函数定义**（`int __thiscall sub_42CA50(char *this)`）。
 * ★ 只认行首（`^[A-Za-z_]`）⇒ 缩进的**调用**（`  return sub_42CA50(a1);`）不会被当成定义。
 */
export const C_DEF_RE = /^[A-Za-z_][\w \t*]*?\b(sub_[0-9A-F]{6})\s*\(/;

// ─────────────────────────────────────────────────────────── 函数清单 / 调用图（**派生视图**）

/** `call sub_XXXXXX` / `jmp sub_XXXXXX`（★ 尾跳也算一条边：它同样把控制权交出去） */
const CALL_RE = /\b(call|jmp)\s+(sub_[0-9A-F]{6})\b/;
/** 寄存器名（`call eax` / `jmp ebx` 这类**不是符号目标**，别把它当成"调用了叫 eax 的东西"） */
const REG_RE = /^(?:e?[abcd]x|e?[sd]i|e?[sb]p|e?sp|r\d+|[abcd][lh]|dword|word|byte|ptr|qword|fs|ds|es|ss|cs|gs)$/i;
/** `call/jmp <符号>`（**任意**符号，含 CRT 的 `___report_gsfailure` 这种）—— 行尾或 `;` 收尾才算 */
const CALL_SYM_RE = /\b(call|jmp)\s+([A-Za-z_$?@][\w.$?@]*)\s*(?:;|$)/;
/** 体内是否出现内存操作数（`[...]`）—— "有没有碰结构"的最小机械判据 */
const MEM_OPERAND_RE = /\[/;
/** `.lst` 每行开头的 `段:EA`（用于算函数体内**最后一个 EA** ⇒ EA 归属要能拒绝"越过函数末尾"的地址） */
const LINE_EA_RE = /^[A-Za-z_.][\w.]*:([0-9A-Fa-f]{8})\s/;
/** 解不出目标的调用点（`call eax` / `call dword ptr [...]` / `call [esi+4]`）—— 只**计数**，不猜目标 */
const INDIRECT_CALL_RE = /\b(call|jmp)\s+(?:eax|ebx|ecx|edx|esi|edi|ebp|esp|dword ptr|\[)/;

/**
 * 一遍扫出**全部 `proc near` 的函数清单**与**调用图**（`call`/`jmp sub_XXXXXX`）。
 *
 * ★ 这是**派生视图**：不落盘、不缓存。语料是只读真源；换一次导出重建一次即可（实测 ~0.2 s / 53 万行）。
 * ★ 它只做**机械提取**：不解语义、不判"分析过没有"。谁"分析过"由台账（锚）回答 —— 两者在本模块外合流
 *   （见 `lib/coverage.mjs`：锚 EA → 包含它的函数）。
 * ★ `indirectCallSites` 必须**报出来**：解不出目标的调用点意味着任何"调用闭包"都只是**下界**
 *   （实测全语料 3000+ 个）—— 不报出来，读者会把"闭包完整"当成事实。
 *
 * @returns {{file:string, dataEas:Set<number>, functions:Array<{sym:string,ea:number,fromLine:number,toLine:number,lastEa:number,callees:string[],calleeHits:Map<string,number>,externalTargets:string[],hasMemoryOperand:boolean,callSites:number,indirectCallSites:number}>, bySym:Map<string,object>, starts:number[], indirectCallSites:number}}
 */
export function functionInventory(lstFile) {
  /** ★ 非 `.text` 段里的行 EA（= **数据/全局**的定义点）：锚落在这些地址上**不是**"归属失败" */
  const dataEas = new Set();
  const lines = fs.readFileSync(lstFile, 'utf8').split('\n');
  const functions = [];
  const bySym = new Map();
  let cur = null;
  let indirectCallSites = 0;
  for (let i = 0; i < lines.length; i += 1) {
    const l = lines[i];
    // 段名不是 `.text` ⇒ 记下它的 EA（`seg002:00559A68 …` / `.data:0051E988 …`）
    {
      const seg = /^([^.\s:][\w.]*|\.(?!text)[\w]+):([0-9A-Fa-f]{8})\s/.exec(l);
      if (seg) dataEas.add(Number.parseInt(seg[2], 16));
    }
    const p = PROC_EA_RE.exec(l);
    if (p) {
      cur = {
        sym: p[3],
        ea: Number.parseInt(p[2], 16),
        fromLine: i + 1,
        toLine: null,
        lastEa: Number.parseInt(p[2], 16),
        callees: new Set(),
        calleeHits: new Map(),
        externalTargets: new Set(),
        hasMemoryOperand: false,
        callSites: 0,
        indirectCallSites: 0,
      };
      functions.push(cur);
      // ★ 同名重复（IDA 偶有）**保留第一个**，与"取第一个"以外的做法不同：这里明确写下来，不静默
      if (!bySym.has(cur.sym)) bySym.set(cur.sym, cur);
      continue;
    }
    if (!cur) continue;
    const le = LINE_EA_RE.exec(l);
    if (le) {
      const v = Number.parseInt(le[1], 16);
      if (v > cur.lastEa) cur.lastEa = v;
    }
    if (ENDP_RE.test(l)) {
      cur.toLine = i + 1;
      cur = null;
      continue;
    }
    if (MEM_OPERAND_RE.test(l)) cur.hasMemoryOperand = true;
    const c = CALL_RE.exec(l);
    if (c) {
      cur.callSites += 1;
      cur.callees.add(c[2]);
      // ★ 逐目标计数（不只是去重集合）：前沿要区分"110 个调用方"与"205 处调用"（叶子助手两者差得远）
      cur.calleeHits.set(c[2], (cur.calleeHits.get(c[2]) ?? 0) + 1);
      continue;
    }
    // ★ 非 `sub_` 的转移目标（CRT / 导入）：`jmp ___report_gsfailure` 这种是"库桩"的指纹
    const e = CALL_SYM_RE.exec(l);
    if (e && !REG_RE.test(e[2])) {
      cur.callSites += 1;
      cur.externalTargets.add(e[2]);
      continue;
    }
    if (INDIRECT_CALL_RE.test(l)) {
      cur.callSites += 1;
      cur.indirectCallSites += 1;
      indirectCallSites += 1;
    }
  }
  for (const f of functions) {
    if (f.toLine === null) f.toLine = f.fromLine; // 未闭合（异常导出）⇒ 只说"只有头一行"
    f.callees = [...f.callees].sort();
    f.calleeHits = new Map([...f.calleeHits].sort((a, b) => (a[0] < b[0] ? -1 : 1)));
    f.externalTargets = [...f.externalTargets].sort();
  }
  const starts = functions.map((f) => f.ea); // ★ 段内 EA 单调（`--stats` 有单调性断言）⇒ 可直接二分
  return { file: lstFile, functions, bySym, starts, dataEas, indirectCallSites };
}

/**
 * **EA → 包含它的函数**（二分；不在任何函数里 ⇒ `null`）。
 *
 * ★ 与 `enclosingFunction` 的分工：那个要**读文件**（用于取上下文 / C 体，一次一个 EA 还带区间）；
 *   这个用在"**几百个锚 EA 一次性归属**"的场景（覆盖度查询），只吃一份已建好的清单。
 * ★ `span` 判据：`起点 ≤ EA ≤ 该函数最后一条有 EA 的行`，且 `EA < 下一个函数的起点`。
 *   用"最后一条 EA"而不是 `endp` 行号 —— 没有 `endp` 的函数段（异常导出）也要能归属；
 *   而**只看下一个函数的起点**会把"最后一个函数之后的地址"（`.data` / 段尾）错误归属给它。
 */
export function functionOfEa(inv, ea) {
  const { starts, functions } = inv;
  if (functions.length === 0) return null;
  let lo = 0;
  let hi = starts.length - 1;
  let hit = -1;
  while (lo <= hi) {
    const m = (lo + hi) >> 1;
    if (starts[m] <= ea) {
      hit = m;
      lo = m + 1;
    } else hi = m - 1;
  }
  if (hit < 0) return null;
  const f = functions[hit];
  // ★ 上界两个都要：**下一个函数的起点**（函数体不许与它重叠）与**本函数最后一条有 EA 的行**
  //   ——只看前者会把"最后一个函数之后的地址"（`.data` / 段尾）错误归属给最后一个函数（实测踩过）。
  const next = functions[hit + 1];
  if (next && ea >= next.ea) return null;
  if (ea > f.lastEa) return null;
  return f.sym;
}

/** `corpus/disasm/files/` → 那份 `.lst`（按名字排序取第一份；不在场 ⇒ `null`，**调用方必须明说**） */
export function pickListing(filesDir) {
  if (!fs.existsSync(filesDir)) return null;
  const hit = fs
    .readdirSync(filesDir)
    .filter((f) => f.endsWith('.lst'))
    .sort()
    .map((f) => path.join(filesDir, f))[0];
  return hit ?? null;
}

/** `.lst` → **同名的 `.c`**（Hex-Rays 伪代码）；不在场 ⇒ `null`（**调用方必须明说**，不许静默降级） */
export function pickDecompiled(lstFile) {
  const c = String(lstFile).replace(/\.lst$/, '.c');
  return fs.existsSync(c) ? c : null;
}

/**
 * 扫一遍 `.c`，建**符号 → 函数体行区间**的表。
 *
 * ★ **不写缓存**（与 `.lst` 的索引相反）：`.c` 只有 5 MB / 17.8 万行，扫一遍 ~0.5 s；
 *   为它引第二份派生缓存（还要自己校验陈旧）不划算。
 *
 * @returns {{file:string, lines:string[], defs:Map<string,{sym:string,from:number,to:number}>, protos:Map<string,number>}}
 */
export function buildSymbolIndex(cFile) {
  const lines = fs.readFileSync(cFile, 'utf8').split('\n');
  const defs = new Map();
  const protos = new Map();
  let cur = null;
  for (let i = 0; i < lines.length; i += 1) {
    const l = lines[i];
    // 段名不是 `.text` ⇒ 记下它的 EA（`seg002:00559A68 …` / `.data:0051E988 …`）
    {
      const seg = /^([^.\s:][\w.]*|\.(?!text)[\w]+):([0-9A-Fa-f]{8})\s/.exec(l);
      if (seg) dataEas.add(Number.parseInt(seg[2], 16));
    }
    const m = C_DEF_RE.exec(l);
    if (m) {
      if (/;\s*$/.test(l)) {
        if (!protos.has(m[1])) protos.set(m[1], i + 1);
        continue;
      }
      if (!defs.has(m[1])) {
        cur = { sym: m[1], from: i + 1, to: null };
        defs.set(m[1], cur);
      }
      continue;
    }
    // ★ 函数体的收尾是**第 0 列的** `}`（内层块都缩进）—— 这个判据必须钉住，否则会把
    //   "下一个函数的结尾"当成自己的结尾，给出**看起来对、其实错**的行区间。
    if (cur && l === '}') {
      cur.to = i + 1;
      cur = null;
    }
  }
  for (const d of defs.values()) if (d.to === null) d.to = d.from; // 未闭合（异常导出）⇒ 只说"只有定义行"
  return { file: cFile, lines, defs, protos };
}

/**
 * ★ **一层工作流的机械核心**：给一个 **EA**（或符号），一路上取
 * ① 它**所属函数**（由 `.lst` 的 `proc near`/`endp` 定）② 该函数的 **Hex-Rays C 体**（在 `.c` 里）。
 *
 * 为什么必须机械：`.c` **一处地址都没有**（实测 `0x00xxxxxx` 计数 = 0）⇒ 从 C 里回不到 EA；
 * 而大量被引用的 EA **根本不是函数起点**（实测：`0x40D500` 是 `loc_40D500`，属于 `sub_40CD10` 体内的一块）
 * ⇒ 靠人肉 grep + 猜行区间必然出错。本函数把"是不是函数起点"和"归属于哪个符号"变成**算出来的**。
 *
 * @param lstFile  `.lst` 路径
 * @param idx      `--lst` 的索引
 * @param o.sym    `sub_XXXXXX`
 * @param o.ea     EA（与 `sym` 二选一；给了 `ea` 就顺带判定"是不是函数起点"）
 * @param o.lines  C 体的**有界**上限（被截断必须明说）
 */
export function pseudoOfFunction(lstFile, idx, { sym = null, ea = null, lines = DEFAULT_MAX_LINES } = {}) {
  let wantSym = sym;
  let lstSpan = null;
  let containing = null;
  let isFunctionStart = null;
  if (ea !== null) {
    const eaNum = parseEa(ea);
    // ★ 用 `enclosingFunction`（能处理"EA 在函数体内"）—— `spanOfFunction` 只对函数起点正确
    lstSpan = enclosingFunction(lstFile, idx, eaNum);
    containing = lstSpan.symbol ?? null;
    if (containing) {
      // ★ 起点判定用 `symbolEa`，**不许**再 `slice(4)` 猜 —— 符号名不一定是 `sub_XXXXXX`
      //   （`_WinMain@16` 之类会让 `slice` 切出垃圾）
      isFunctionStart = lstSpan.symbolEa !== null && lstSpan.symbolEa !== undefined ? lstSpan.symbolEa === eaNum : null;
      if (!wantSym) wantSym = containing;
    }
  }
  if (!wantSym || !new RegExp(`^${SYM}$`).test(wantSym)) {
    throw new Error(
      `定位不到函数符号（给的是 ${sym ? JSON.stringify(sym) : 'EA ' + ea}）—— ` +
        'EA 不在任何 `proc near` 里、或符号形态不认识（支持 `sub_XXXXXX` 与 MSVC 修饰名如 `_WinMain@16`）。',
    );
  }
  const mSub = /^sub_([0-9A-F]{6})$/i.exec(wantSym);
  const fnEa = mSub ? Number.parseInt(mSub[1], 16) : (lstSpan?.symbolEa ?? null);
  if (fnEa === null) {
    throw new Error(
      `定位不到函数符号（给的是 ${sym ? JSON.stringify(sym) : 'EA ' + ea}）—— ` +
        `“${wantSym}”不是 \`sub_XXXXXX\` ⇒ 必须**同时给 \`--ea <它所在函数里的某个 EA>\`**（否则算不出函数起点）。`,
    );
  }
  // 给了符号（而不是 EA）时，那个符号名**本身就是函数起点** ⇒ `spanOfFunction` 这时是对的
  if (!lstSpan) lstSpan = spanOfFunction(lstFile, idx, fnEa);

  const cFile = pickDecompiled(lstFile);
  const out = {
    sym: wantSym,
    fnEa,
    isFunctionStart,
    containing,
    lst: { fromLine: lstSpan.fromLine, toLine: lstSpan.toLine, lines: lstSpan.toLine - lstSpan.fromLine + 1 },
    c: null,
    note: null,
  };
  if (!cFile) {
    out.note = `语料里没有配套的 .c（只看 .lst）：${path.basename(String(lstFile).replace(/\.lst$/, '.c'))} 不在场`;
    return out;
  }
  const cidx = buildSymbolIndex(cFile);
  // ★ `.lst` 用修饰名、`.c` 头用可读名 ⇒ 按原名找不到时再按去修饰名找一次（别把"有 C 体"误报成"没有定义"）
  const def = cidx.defs.get(wantSym) ?? (undecorate(wantSym) === wantSym ? null : cidx.defs.get(undecorate(wantSym)));
  if (!def) {
    const und = undecorate(wantSym);
    out.note =
      `★ \`${wantSym}\` 在 .c 里**没有定义**（实测 3807 个 \`proc near\` 里有 77 个如此）` +
      '—— Hex-Rays 没反编译它。只能读 .lst（下面给了行区间）。' +
      (und === wantSym
        ? ''
        : `★ 注意这是**修饰名**（去修饰 = \`${und}\`），而本索引**只索引 \`sub_XXXXXX\`** ⇒ ` +
          '具名函数（`WinMain` / `operator new` / 带 vtable 的方法…）本来就不进索引；要它的 C 体就直接去 `.c` 里按名读。');
    return out;
  }
  const body = cidx.lines.slice(def.from - 1, Math.min(def.to, def.from - 1 + lines));
  out.c = {
    file: cFile,
    fromLine: def.from,
    toLine: def.to,
    bodyLines: def.to - def.from + 1,
    body,
    truncated: body.length < def.to - def.from + 1,
  };
  if (isFunctionStart === false) {
    out.note = `★ 该 EA **不是函数起点**：它属于 \`${containing}\`（函数起点 0x${(lstSpan.symbolEa ?? fnEa).toString(16)}）⇒ 下面给的是**整个函数**的 C 体`;
  }
  return out;
}

// ─────────────────────────────────────────────────────────── 自描述

const CONDITIONAL = {
  match: '`--match <子串>`；加 `--regex` 才按正则解释。★ 默认**字面量**（正则容易在语料里误伤）。',
  limit: '`--limit N`（缺省 200）：命中超过 N 时**明说截断**，不静默少报。',
  lines: `\`--lines N\`（缺省 ${DEFAULT_MAX_LINES}）：上下文窗口上限。★ 必须有上限 —— 本工具存在的理由就是"不许漫游"。`,
  write: '`--write` 才落盘（缺省 dry-run）；写后回读复验，不绿回滚。',
};

export function describe() {
  return {
    file: 'corpus/disasm/files/*.lst（**只读**语料；解压产物，不入库）',
    purpose:
      '把"大文件里漫游"换成**机械提取**：给定 **EA** 或一个字符串，确定性地取出**一小段**上下文。' +
      '这是所有引擎分析的第一步 —— 分析不该以"读几十 MB 反汇编"开始。',
    notHere:
      '索引**不是真源**（它落 `.cache/`、可删可重建）；语料**不得就地修改**（任何友好化只许是派生的内存视图）。' +
      '也不是"语义层"：本工具只做**定位与切片**，不做解释（解释是分析的事，要过 `AGENTS.md` §6 准入门）。',
    fields: [
      ['ea', '✅', 'EA（虚拟地址）', '★ **锚点锚 EA，不锚行号**：`EA → 行号` 是本工具**现算**的映射。换一次反汇编只重建映射。'],
      ['seg', '⬜', 'IDA 段名', '实测有 `.text` / `.data` / `seg002` / `.idata` / `poly` / `seg003`。不给就按 EA 范围自动选段。'],
      ['match', 'search 必填', '字面量子串', CONDITIONAL.match],
      ['limit', '⬜', '整数', CONDITIONAL.limit],
      ['lines', '⬜', '整数', CONDITIONAL.lines],
      ['write', '⬜', 'flag', CONDITIONAL.write],
    ],
    invariants: [
      { id: 1, text: '`EA → 行号` 映射**自洽**：段内 EA 单调不降（`--stats` 报 `monotonic`）', enforcedBy: '本工具 --stats + tools/test/disasm.test.mjs' },
      { id: 2, text: '**索引可删可重建**：同一份语料、同一份索引 ⇒ 同字节（派生缓存不是真源）', enforcedBy: 'tools/test/disasm.test.mjs' },
      { id: 3, text: '**截断必须明说**：`--lines` / `--limit` 触顶时如实报 `truncated`', enforcedBy: 'tools/test/disasm.test.mjs' },
      { id: 4, text: '**语料只读**：本工具只读语料；任何产物落 `.cache/` 或 stdout', enforcedBy: 'tools/test/disasm.test.mjs + corpus/assets.json 的 readOnly' },
    ],
    operations: OPERATIONS,
  };
}

export function describeText(d = describe()) {
  const L = [];
  L.push(`# ${d.file} —— 自描述（由控制脚本给出，文档不复述 schema）`);
  L.push('');
  L.push(`用途：${d.purpose}`);
  L.push(`★ 这里**不**放：${d.notHere}`);
  L.push('');
  L.push('## 字段 / 参数');
  L.push('| 名 | 必填 | 取值 | 说明 |');
  L.push('|---|---|---|---|');
  for (const [name, req, type, desc] of d.fields) L.push(`| \`${name}\` | ${req} | ${type} | ${desc} |`);
  L.push('');
  L.push('## 不变量（含"谁在守它"）');
  for (const i of d.invariants) L.push(`${i.id}. ${i.text}　—　${i.enforcedBy}`);
  L.push('');
  L.push('## 操作');
  for (const o of OPERATIONS) L.push(`* \`${o.name}\`${o.mutates ? '（会写）' : ''} —— ${o.summary}　→ \`pnpm tools disasm ${o.name}\``);
  return `${L.join('\n')}\n`;
}
