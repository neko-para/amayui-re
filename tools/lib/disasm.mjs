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
