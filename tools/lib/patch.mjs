/**
 * tools/lib/patch.mjs — **`data/translations/patch.json` 的领域模型**（schema / 不变量 / 读 / 写 / 自描述 / 提取 / 重放）
 *
 * 分层（见 `tools/README.md` §0）：本文件**不解析 argv、不打印**。CLI 在 `tools/patch.mjs`。
 *
 * ## 一句话
 * 译文不再是一棵文本树，而是**相对原版 BIN 的变更叠加层**；`data`（原版）与 `src`（原版 + patch）
 * 都是**实时算出来的视图**，不入库。设计评估与支撑观测在 `docs/01-translation/patch-design.md`。
 *
 * ## 锚：为什么是"基线行序 + 摘要"
 * 插入一条指令就会改掉后续**所有字节偏移**，而 `label_XXXXXXXX` 本身就是绝对字节偏移
 * （实测 `label_00000320` 处的指令偏移正是 0x320）⇒ 偏移与 label 都是**派生量**。
 * 实测：屏蔽 label 引用后，插入指令不会让其余指令的行序错位（8/8 文件），插入字符串表定义也一样（6/6）。
 * ⇒ 锚 = **基线反汇编的行序 `i`** + `sha8`（该行内容摘要，基线一变即判冲突）。
 *
 * ## 行序空间（`rowsOf()` 的定义）
 * `disassemble()` 的输出 = 4 行头部 + 空行 + 指令行 / label 定义行。行序空间是：
 *
 * ```
 * 丢掉空行与 label 定义行 ⇒ 剩下的每一行 = 一个"行"
 * 行内的 label **引用**遮蔽成 label_?（地址是派生量，进 patch 没意义，重建时由汇编器重算）
 * ```
 *
 * ★ 头部 4 行**不进行序空间**：实测官方集 223 个脚本里，基线与产物的头部 4 行**223/223 完全相同**
 *   （`local_vars` 也没变）；不同就**抛错**而不是硬编 —— 那说明"头部也能变"，锚定假设不成立。
 *
 * ★ `comment` 是**数据不是注释**：它 argc=1、带字符串载荷、占字节。把它当注释滤掉，
 *   实测 6 个样本脚本会少 14 个字符串 / 472 B（重建就不再逐字节相同）。
 *
 * ## 三种操作
 * | op | 语义 | 字段 |
 * |---|---|---|
 * | `replace-line` | 基线第 `i` 行换成新的一行 | `line`（**中文**源行，整行） |
 * | `delete` | 删掉基线第 `i` 行 | 只需 `sha8`（原文随时可从基线取；序号空间就是基线） |
 * | `insert-after` | 在基线第 `i` 行后插入一行（`i = -1` ⇒ 插在最前） | `instr`（**中文**源行） |
 *
 * 行序 `i` 在 ops 里**非递减**；同一个 `i` 最多一条 `replace-line`/`delete`，`insert-after` 可多条且保持顺序。
 *
 * ## 判据：**逐字节**
 * `基线 BIN → 反汇编 → 打 patch → 汇编 ⇒ 逐字节等于产物 BIN`。`resultSha` 是这条判据在旧仓消失之后
 * **唯一还能机械复核的证人**：它由 `extract` 从旧仓产物写入，`verify` 只读不写（否则就是自证循环）。
 */
import fs from 'node:fs';
import path from 'node:path';
import { createHash } from 'node:crypto';

import { assemble, disassemble } from '../../packages/age-format/src/asm/index.mjs';
import { isAgeScript, isBinName, openRoot } from './bin-source.mjs';
import { DEFAULT_SUBS, dictSha, loadDict, makeMapper } from './cn-jp.mjs';
import { sha256buf } from './fsx.mjs';
import { loadManifest } from './manifest.mjs';
import { DEFAULT_MANIFEST, REPO_ROOT } from './paths.mjs';

export { DEFAULT_SUBS, REPO_ROOT };

export const DEFAULT_PATCH = path.join(REPO_ROOT, 'data', 'translations', 'patch.json');

/**
 * **`SPEAKER_FILTER`** —— 旧仓 `scripts/annotate-speaker.js` 挑文件用的那个正则
 * （`/^(SC|SP)/ || /^\$\d+\$(SC|SP)/`）。
 *
 * ★ **它不是 patch 的范围，也不是"一类脚本"。** 它只是**"说话人标注"这个任务**要处理哪些文件的口径。
 *   用户口径（**不是**我从二进制推的结论）：有人说话的剧情来自 `SC`/`SP`；`SN` 是序言（纯旁白）、
 *   `SG` 是系统文案（UI 那部分）、其余多为物品名称之类 —— 所以只有 `SC`/`SP` 需要标说话人。
 *   ⇒ 这两类的"特殊性"完全来自**那个任务**，不构成脚本分类。
 *
 * ★ 曾经我把它当成"翻译管线处理过的脚本集合"并叫它「官方集」——**那是错的**：
 *   实测旧仓 `src/` 有 **941** 个文本（全部脚本），其中**非 SC/SP 的 247 支同样有译文**
 *   （产物与基线不同，且文本里有简体字）。⇒ patch 的范围 = **产物与基线不同的全部脚本**，
 *   与这个正则**无关**。这里留着它，只为给页面打一个"说话人标注过"的标签。
 */
export const SPEAKER_FILTER = /^(\$\d+\$)?(SC|SP)/i;

/** label 引用的形态：`label_` + 8 位十六进制（`labelHex` 定长补零） */
const LABEL_RE = /label_[0-9a-f]{8}/g;
/**
 * label **定义行**的形态：`label_` + 任意位数 hex。
 * ★ 这里**不能**也要求 8 位：`replay()` 放出的是**符号**（`0x100000 + id` ⇒ 6~7 位），
 *   把符号行当成指令行会让"打上 patch 的文本"再也解析不出 label 定义。
 *   （与汇编器同口径：它也是看前缀 `label_` 就当定义。）
 */
const LABEL_LINE_RE = /^label_([0-9a-fA-F]+)$/;

/** 符号 label 的取值空间：避开真实偏移（`0x100000 + id`，最多 7 位 hex）只作唯一标识用 */
const SYM_BASE = 0x100000;
const SYM_LIMIT = 0x10000000; // 真实 label 是 `labelHex` 补零到 8 位；符号必须**不与它同形**
/**
 * **patch 自己的 label** 的编号起点：产物新增的跳转目标在基线里没有身份，
 * 只能给它一个新符号（由 `def` 挂在插入/替换行上）。取一个远离基线 id 空间的起点，
 * 保证 `symOf` 的结果仍然只有 6~7 位 hex（不与 8 位的真实地址同形）。
 */
const LOCAL_ID_BASE = 0x800000;
const symOf = (id) => {
  if (id >= SYM_LIMIT) throw new Error(`label id 超出符号空间（${id}）—— 单文件不可能有这么多 label`);
  return `label_${(SYM_BASE + id).toString(16)}`;
};

/** 工具**拥有**的顶层 `_doc`（写盘时自动注入 ⇒ 指向自描述，不会漂） */
export const PATCH_DOC =
  '本文件是**不透明数据**：字段语义 / 不变量 / 怎么查怎么改 见 `pnpm tools patch describe`（说明书 data/translations/patch.md）。不要手改。';

/** 工具层的自我声明：**我动哪片数据、有哪些操作**（`tools/cli.mjs` 的域地图从它派生） */
export const DOMAIN = {
  id: 'patch',
  title: '翻译 patch（唯一入库物 = 变更叠加层；data / src 都是视图）',
  data: [
    '`data/translations/patch.json`（**唯一写入口就是本工具**）',
    '`data/translations/subs-cn-jp.json`（简→日写法字典；**只读**，是构建的一环 ⇒ 指纹进 patch）',
    '外部只读来源：`gameInstall`（基线根）· 旧仓 `install/`（**仅提取期**读一次）',
  ],
  access: 'rw（唯一写入口；缺省 dry-run，写后回读复验，不绿回滚）',
  tool: 'tools/patch.mjs',
};

export const OPERATIONS = [
  { name: 'describe', argv: ['--describe'], mutates: false, summary: '自描述：字段 / 枚举 / 不变量（含谁在守）/ 操作' },
  { name: 'status', argv: ['--status'], mutates: false, summary: '规模与分布：脚本数 / 操作数 / 体积 / 基线解析来源' },
  { name: 'baseline', argv: ['--baseline'], mutates: false, summary: '某个脚本（或全部）的**基线与产物**从哪来：`[<脚本名>…]`' },
  { name: 'extract', argv: ['--extract'], mutates: true, summary: '从旧仓产物提取 patch（**迁移期一次性**）：`[--base <目录>] [--target <目录>] [--name <脚本>] [--skip <脚本>] [--limit <n>] [--write]`' },
  { name: 'verify', argv: ['--verify'], mutates: false, summary: '判据：基线 + patch ⇒ **逐字节**相同（**缺省自证 resultSha**；`--target <目录>` 才与产物比对）`[--name <脚本>]' },
  { name: 'view', argv: ['--view'], mutates: true, summary: '生成 `data` / `src` **视图**（生成物，不入库）：`[--kind data|src|both] [--name <脚本>] [--limit n] [--out <目录>] [--stdout] [--bin]`' },
  { name: 'edit', argv: ['--edit'], mutates: true, summary: '**改过的 `src` 视图 ⇒ 反解回 patch**（只碰与当前重建结果不同的那些）：`[--name <脚本>] [--out <视图目录>] [--write]`' },
];

/** 字段说明（`--describe` 用；这是**自描述**，不是第二份 schema） */
const FIELD_DOC = [
  ['schemaVersion', '✅', '1', 'schema 版本；不匹配即拒读'],
  ['_doc', '✅', 'string', '由本工具拥有（写盘时自动注入指向本自描述的指针）'],
  ['subsSha', '✅', '64 位 hex', '`subs-cn-jp.json` 的 sha256 —— **构建的一环**：换字典 ⇒ 重建结果会变'],
  ['scripts', '✅', '对象：脚本名 → 条目', '键是 BIN 文件名（含 `$N$` 前缀），全大写比较友好但**按原样存**'],
  ['scripts[].baseSha', '✅', '64 位 hex', '该脚本**基线** BIN 的 sha256（防错误 apply；变即冲突）'],
  ['scripts[].resultSha', '✅', '64 位 hex', '该脚本**产物** BIN 的 sha256（判据的证人；由 extract 写、verify 只读）'],
  ['scripts[].header', '⬜', '4 个 string', '反汇编的头部 4 行。**只在产物与基线不同时出现**（实测 `$1$IMINIT.BIN` 的 `local_vars` 就变了）；重放时用它替掉基线的头部'],
  ['scripts[].ops', '✅', '数组（可为空）', '空数组合法（手写 / 分片场景），但**提取器不会产出空条目**：没有变更的脚本就没有条目'],
  ['ops[].op', '✅', '`replace-line` \\| `delete` \\| `insert-after`', '操作类型'],
  ['ops[].i', '✅', '整数 ≥ -1', '**基线行序**（`-1` 只对 `insert-after` 有意义 = 插在最前）'],
  ['ops[].sha8', '条件', '8 位 hex', '`replace-line` / `delete` 必填：基线第 `i` 行的内容摘要'],
  ['ops[].line', '条件', '非空 string', '`replace-line` 必填：**整行**新内容，取值为**中文**'],
  ['ops[].instr', '条件', '非空 string', '`insert-after` 必填：**整行**新内容，取值为**中文**'],
  ['ops[].def', '⬜', '`[label_<hex>…]`', '该行**自己定义**的 label（patch 局部符号）：只在"产物的新跳转目标落在基线没有身份的行上"时出现。基线那边已有的定义**不重复放**'],
];

/** 不变量 + **谁在守它**（自校验 / 测试 / 守卫） */
const INVARIANTS = [
  ['schemaVersion = 1；每个条目 baseSha/resultSha 是 64 位 hex；op 只取三种枚举，且各自必填字段齐全',
    '本工具写入时自校验（`savePatch` 写后复验，不绿回滚）+ `--verify`'],
  ['行序 `i` 在 ops 里**非递减**；同一个 `i` 最多一条 `replace-line` / `delete`（**不许重叠**）',
    '`structuralProblems()`（读写两侧都跑）'],
  ['`replace-line` / `delete` 的 `i` 落在基线的行数范围内；`insert-after` 的 `i ∈ [-1, rows-1]`',
    '`replay()` 打 patch 时逐条边界检查（越界即抛，不静默截断）'],
  ['`sha8` 必须与基线第 `i` 行的内容**对得上**（基线一换 ⇒ 报冲突，不静默改写）',
    '`replay()` 的正文校验'],
  ['头部 4 行只在条目的 `header` 出现时才用它（且必须是 4 个字符串）；否则一律取基线的头部',
    '`structuralProblems()` + `replay()`'],
  ['**label 指向也要对得上**：匹配上的行若"值相同但 label 指的地方变了"（遮蔽地址之后长得一样），提取时**降级成 `replace-line`**；重建时 `def` 里的符号**不许重复定义**',
    '`extractEntry()` 的 `matchIsFaithful()` + `replay()` 的 `emitDefs()`'],
  ['`scripts` 的键是 `.BIN` 名字，范围 = **基线根里全部能反汇编的 AGE 脚本**（不是"官方集"、也不按名字过滤）',
    '`--verify`（patch 的键必须是该范围的子集；不在范围里的键即报错）'],
  ['**没有变更的脚本不进 patch**（空 patch 不建条目）⇒ "一共有多少脚本 / 其中多少有译文"不能从 patch 反推',
    "`--verify` 的「无条目复核」：range 里没条目的那些，逐支断言**产物与基线逐字节相同**（少了这条，漏提取就会静默）"],
  ['`subsSha` 与当前字典一致（不一致 ⇒ 重建结果可能与 resultSha 不符）',
    '`--verify` 报出（不阻断：换字典的后果要显式看见）'],
  ['`resultSha` **只由 extract 写、verify 只读**（否则就是自证循环）',
    '`savePatch()` 不接受"verify 之后回写 resultSha"这条路径 —— 写入只发生在 extract'],
];

// ─────────────────────────────────────────────────────────── 行序空间

/**
 * 把反汇编文本切成"头部 + 行序空间"。
 * @param {string} text `disassemble()` 的输出
 * @returns {{header:string[], raw:string[], masked:string[], defsBefore:number[][],
 *            addrToId:Map<string,number>, idToAddr:Map<number,string>, idOrdinal:Map<number,number>}}
 *   `raw[i]` 未遮蔽（label 仍是地址）；`masked[i]` 把 label 引用遮蔽成 `label_?`；
 *   `defsBefore[i]` = 紧邻第 `i` 行之前的 label 定义 id 列表（`defsBefore[rows]` = 文件末尾的）。
 */
export function rowsOf(text) {
  const lines = text.split('\n');
  const header = lines.slice(0, 4);
  const addrToId = new Map();
  const idToAddr = new Map();
  const idOrdinal = new Map();
  const defsBefore = [];
  const raw = [];
  let pending = [];
  for (const line of lines.slice(4)) {
    // 行尾注释由反汇编器写成 `  // …`（带前导空白）；`comment "…"` 是**指令**，不会命中这里
    const t = line.replace(/\s+\/\/.*$/, '').trim();
    if (t === '') continue;
    const m = LABEL_LINE_RE.exec(t);
    if (m) {
      const key = m[1].toLowerCase();
      if (!addrToId.has(key)) {
        const id = addrToId.size;
        addrToId.set(key, id);
        idToAddr.set(id, key);
      }
      pending.push(addrToId.get(key));
      continue;
    }
    for (const id of pending) idOrdinal.set(id, raw.length);
    defsBefore.push(pending);
    pending = [];
    raw.push(t);
  }
  defsBefore.push(pending);
  const masked = raw.map((l) => l.replace(LABEL_RE, 'label_?'));
  return { header, raw, masked, defsBefore, addrToId, idToAddr, idOrdinal };
}

/** 把一行里的 label 地址换成**符号**（地址是派生量；符号才是可重算的标识） */
export function symbolize(line, addrToId) {
  return line.replace(LABEL_RE, (full) => {
    const id = addrToId.get(full.slice(6).toLowerCase());
    if (id === undefined) throw new Error(`行里有未定义的 label 引用：${full} —— 基线里没有这个地址`);
    return symOf(id);
  });
}

export const sha8 = (s) => createHash('sha256').update(s).digest('hex').slice(0, 8);

// ─────────────────────────────────────────────────────────── 差分（基线行序 ↔ 产物行序）

/**
 * 逐行对齐两侧（屏蔽 label 后比较），产出三种操作。
 *
 * ## 为什么不是"窗口前瞻"那么简单
 * 朴素的"在窗口里找到下一个相同的行就当作插入/删除"会**被远处的巧合骗到**：实测 `$1$SCJUMP.BIN`
 * 只在中间动了 3 行，却因为 32 行外有个恰好相同的 `jcc (local-int 5) …` 而被判成"删 47 行 + 插 49 行"，
 * 顺带把产物的跳转目标挤进了"插入出来的行"（那种位置在基线里**没有身份**，patch 表达不了）。
 *
 * ## 现在的算法（三段式，逐级收窄）
 * 1. **剥公共前后缀**：两侧连续相同的行直接跳过（不产 op）；
 * 2. **patience 锚**：只认"在两侧都**恰好出现一次**"的行，取它们的最长递增配对做锚，
 *    在锚之间递归 —— 这一步把"局部小改动"从整段里**隔离**出来，远处巧合因此够不着；
 * 3. **兜底**：段内做"找下一个能连上 `run` 行"的配对（`run ≥ 2` ⇒ 单行巧合不算数），
 *    找不到就 `replace-line`。
 *
 * ★ 锚是**行内容**（已屏蔽 label 引用）—— 字节偏移与 label 都是派生量，当不了锚。
 * @param {string[]} a 基线（masked）
 * @param {string[]} b 产物（masked）
 * @returns {{ops:Array<{op:string,i:number,j?:number}>, align:Map<number,number>}}
 *   `ops` 的 `j` 只在提取期有意义（产物的行号），写完就丢；
 *   `align` 是**对齐图**（产物行序 → 基线行序），提取期靠它把产物的 label 引用翻译回基线空间的符号。
 *   ★ 它必须由**这里**给出：事后从 ops 反推对齐要区分三种 op、还要补回"隐式匹配的行"，很容易错。
 */
export function diffRows(a, b, opts = {}) {
  const { run = 3, maxLook = 4096, maxCandidates = 48 } = opts;
  const ops = [];
  /** 对齐图：产物行序 → 基线行序（**只有这里知道**隐式匹配了哪些行） */
  const align = new Map();
  /** 行内容 → 升序位置表（兜底那一步靠它做二分，不必线性扫） */
  const indexRows = (rows) => {
    const m = new Map();
    for (let k = 0; k < rows.length; k += 1) {
      if (!m.has(rows[k])) m.set(rows[k], []);
      m.get(rows[k]).push(k);
    }
    return m;
  };
  const bPos = indexRows(b);
  const aPos = indexRows(a);

  /** 在 `o` 的位置表里找最小的 k ≥ 1，使 `s[from] == o[otherFrom+k]` 且其后还能连上 run 行 */
  function findRun(pos, from, otherFrom, s, o, sEnd, oEnd) {
    const list = pos.get(s[from]);
    if (!list) return -1;
    let lo = 0;
    let hi = list.length;
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      if (list[mid] <= otherFrom) lo = mid + 1;
      else hi = mid;
    }
    let tried = 0;
    for (let p = lo; p < list.length && tried < maxCandidates; p += 1) {
      const at = list[p];
      // ★ 位置表是**全局**的；候选必须落在当前段内，否则会越过段尾、把中间的行整段丢掉
      if (at >= oEnd) break;
      const k = at - otherFrom;
      if (k > maxLook) break;
      tried += 1;
      let t = 1;
      while (t < run && from + t < sEnd && at + t < oEnd && s[from + t] === o[at + t]) t += 1;
      const remain = Math.min(run, sEnd - from, oEnd - at);
      if (t >= remain) return k;
    }
    return -1;
  }

  /** patience 锚：两侧都唯一出现的行 → 最长递增配对 */
  function uniqueAnchors(a0, a1, b0, b1) {
    const ca = new Map();
    const cb = new Map();
    const posB = new Map();
    for (let i = a0; i < a1; i += 1) ca.set(a[i], (ca.get(a[i]) ?? 0) + 1);
    for (let j = b0; j < b1; j += 1) {
      cb.set(b[j], (cb.get(b[j]) ?? 0) + 1);
      if (!posB.has(b[j])) posB.set(b[j], j);
    }
    const pairs = [];
    for (let i = a0; i < a1; i += 1) {
      const s = a[i];
      if (ca.get(s) === 1 && cb.get(s) === 1) pairs.push([i, posB.get(s)]);
    }
    // bi 的最长递增子序列（保持 ai 升序）
    const tails = [];
    const back = new Array(pairs.length).fill(-1);
    for (let p = 0; p < pairs.length; p += 1) {
      const v = pairs[p][1];
      let lo = 0;
      let hi = tails.length;
      while (lo < hi) {
        const mid = (lo + hi) >> 1;
        if (pairs[tails[mid]][1] < v) lo = mid + 1;
        else hi = mid;
      }
      if (lo > 0) back[p] = tails[lo - 1];
      tails[lo] = p;
    }
    const out = [];
    for (let p = tails.length ? tails[tails.length - 1] : -1; p >= 0; p = back[p]) out.push(pairs[p]);
    return out.reverse();
  }

  function walk(a0, a1, b0, b1) {
    while (a0 < a1 && b0 < b1 && a[a0] === b[b0]) { align.set(b0, a0); a0 += 1; b0 += 1; }
    while (a1 > a0 && b1 > b0 && a[a1 - 1] === b[b1 - 1]) { a1 -= 1; b1 -= 1; align.set(b1, a1); }
    if (a0 === a1) { for (let j = b0; j < b1; j += 1) ops.push({ op: 'insert-after', i: a0 - 1, j }); return; }
    if (b0 === b1) { for (let i = a0; i < a1; i += 1) ops.push({ op: 'delete', i }); return; }

    const anchors = uniqueAnchors(a0, a1, b0, b1);
    if (anchors.length) {
      let pa = a0;
      let pb = b0;
      for (const [ai, bi] of anchors) {
        walk(pa, ai, pb, bi);
        align.set(bi, ai);
        pa = ai + 1;
        pb = bi + 1;
      }
      walk(pa, a1, pb, b1);
      return;
    }

    let i = a0;
    let j = b0;
    while (i < a1 && j < b1) {
      if (a[i] === b[j]) { align.set(j, i); i += 1; j += 1; continue; }
      const forward = findRun(bPos, i, j, a, b, a1, b1);
      const drop = findRun(aPos, j, i, b, a, b1, a1);
      if (forward !== -1 && (drop === -1 || forward <= drop)) {
        for (let k = 0; k < forward; k += 1) ops.push({ op: 'insert-after', i: i - 1, j: j + k });
        j += forward;
        continue;
      }
      if (drop !== -1) {
        for (let k = 0; k < drop; k += 1) ops.push({ op: 'delete', i: i + k });
        i += drop;
        continue;
      }
      ops.push({ op: 'replace-line', i, j });
      i += 1;
      j += 1;
    }
    while (j < b1) { ops.push({ op: 'insert-after', i: a1 - 1, j }); j += 1; }
    while (i < a1) { ops.push({ op: 'delete', i }); i += 1; }
  }

  walk(0, a.length, 0, b.length);
  // 行序必须非递减（格式的不变量）；同 `i` 内部保持产出顺序 ⇒ 用稳定排序兜底
  ops.sort((p, q) => p.i - q.i);
  return { ops, align };
}

// ─────────────────────────────────────────────────────────── 提取

/**
 * 由「基线 BIN + 产物 BIN」提取一个条目的操作。
 *
 * 产物的行序**不**进 patch：`replace-line` / `insert-after` 的载荷在离开这里之前就被翻译成
 * **基线空间**的写法（label 用基线的符号、字符串用中文），这样 patch 才与"产物当年长什么样"解耦。
 *
 * @param {Buffer} baseBuf 基线 BIN
 * @param {Buffer} tgtBuf  产物 BIN
 * @param {{lineToCn:(line:string)=>string}} mapper 只改字符串常量（BIN 写法 → 中文）
 * @returns {{baseSha:string, resultSha:string, ops:Array<object>, stats:object}}
 */
export function extractEntry(baseBuf, tgtBuf, mapper) {
  const B = rowsOf(disassemble(baseBuf));
  const T = rowsOf(disassemble(tgtBuf));

  // ★ 头部 4 行**通常**相同（实测 941 支里 940 支如此），但它**可以**变：
  //   `$1$IMINIT.BIN` 的 `local_vars` 就从 `{ 3 1 1 2 1 1 }` 变成了 `{ 1 1 1 1 1 1 }`。
  //   ⇒ 不同时**不要抛**，把整块头部带进 patch（`header`），重放时用它替掉基线的那 4 行。
  const headerChanged = B.header.join('\n') === T.header.join('\n') ? null : T.header;

  const { ops: rawOps, align } = diffRows(B.masked, T.masked);
  const reverse = new Map(); // 基线行序 → 产物行序（只为"降级成 replace-line"时取载荷）
  for (const [j, i] of align) reverse.set(i, j);

  const stats = { labelRefs: 0, forcedReplace: 0, localLabels: 0 };

  /** 某行引用的 label，解析成**该文件自己的行序**列表（按出现顺序） */
  const refOrdinals = (line, R) =>
    [...line.matchAll(LABEL_RE)].map((m) => {
      const id = R.addrToId.get(m[0].slice(6).toLowerCase());
      if (id === undefined) throw new Error(`出现了未定义的 label 引用：${m[0]}`);
      return R.idOrdinal.get(id);
    });

  /**
   * 匹配上的行是否**真的**没变：值相同还不够，label 指的地方也必须按对齐关系对得上。
   * （产物在该行新加了一个 label 定义时也算"变了" —— 否则那个定义没有地方挂。）
   */
  const matchIsFaithful = (j, i) => {
    if (T.defsBefore[j].length > B.defsBefore[i].length) return false;
    if (!/label_/.test(B.raw[i]) && !/label_/.test(T.raw[j])) return true;
    if (B.raw[i].replace(LABEL_RE, '\u0000') !== T.raw[j].replace(LABEL_RE, '\u0000')) return false;
    const a = refOrdinals(B.raw[i], B);
    const bRefs = refOrdinals(T.raw[j], T);
    if (a.length !== bRefs.length) return false;
    for (let k = 0; k < a.length; k += 1) if (align.get(bRefs[k]) !== a[k]) return false;
    return true;
  };

  const forced = new Set();
  for (const [j, i] of align) if (!matchIsFaithful(j, i)) forced.add(i);

  // ── 局部 label 符号：产物的某个行序在基线里没有身份（插入出来的 / 基线那一行没有定义）
  const localSymbol = new Map(); // 产物行序 → 符号
  let nextLocal = 0;
  const needsLocal = (tOrd) => {
    const bOrd = align.get(tOrd);
    return bOrd === undefined || (B.defsBefore[bOrd] ?? []).length === 0;
  };
  const symbolForOrdinal = (tOrd) => {
    if (tOrd === undefined) throw new Error('产物里出现了指向不存在行的 label 引用');
    if (!needsLocal(tOrd)) return symOf(B.defsBefore[align.get(tOrd)][0]);
    if (!localSymbol.has(tOrd)) {
      localSymbol.set(tOrd, symOf(LOCAL_ID_BASE + nextLocal));
      nextLocal += 1;
      stats.localLabels += 1;
    }
    return localSymbol.get(tOrd);
  };

  /** 产物的一行 → 基线空间的写法（label 换成符号、字符串换成中文） */
  const toBaseSpace = (line) => {
    const withLabels = line.replace(LABEL_RE, (full) => {
      stats.labelRefs += 1;
      const id = T.addrToId.get(full.slice(6).toLowerCase());
      if (id === undefined) throw new Error(`产物的行里有未定义的 label 引用：${full}`);
      return symbolForOrdinal(T.idOrdinal.get(id));
    });
    return mapper.lineToCn(withLabels);
  };

  /** 该行需要放出的**局部** label 定义（基线那边已经有的不用重复放） */
  const defOf = (j) => {
    if (!(T.defsBefore[j] ?? []).length) return undefined;
    return needsLocal(j) ? [symbolForOrdinal(j)] : undefined;
  };

  const ops = [];
  for (const o of rawOps) {
    if (o.op === 'replace-line') {
      const op = { op: 'replace-line', i: o.i, sha8: sha8(B.masked[o.i]), line: toBaseSpace(T.raw[o.j]) };
      const def = defOf(o.j);
      if (def) op.def = def;
      ops.push(op);
    } else if (o.op === 'delete') {
      ops.push({ op: 'delete', i: o.i, sha8: sha8(B.masked[o.i]) });
    } else {
      const op = { op: 'insert-after', i: o.i, instr: toBaseSpace(T.raw[o.j]) };
      const def = defOf(o.j);
      if (def) op.def = def;
      ops.push(op);
    }
  }
  // ── 把"看着一样、其实 label 目标变了"的匹配行降级成 replace-line
  for (const i of forced) {
    const j = reverse.get(i);
    const op = { op: 'replace-line', i, sha8: sha8(B.masked[i]), line: toBaseSpace(T.raw[j]) };
    const def = defOf(j);
    if (def) op.def = def;
    ops.push(op);
  }
  stats.forcedReplace = forced.size;
  ops.sort((p, q) => p.i - q.i);

  return {
    baseSha: sha256buf(baseBuf),
    resultSha: sha256buf(tgtBuf),
    ...(headerChanged ? { header: headerChanged } : {}),
    ops,
    stats: { baseRows: B.raw.length, resultRows: T.raw.length, ...stats },
  };
}

// ─────────────────────────────────────────────────────────── 重放（打 patch）

/**
 * 把 patch 作用在基线反汇编文本上 ⇒ 新的反汇编文本（头部原样保留）。
 *
 * 逐条做边界与 `sha8` 校验：**基线一变就抛**，不静默按位置硬套（那正是"静默改写"的来源）。
 *
 * @param {string} baseText `disassemble(基线 BIN)`
 * @param {Array<object>} ops
 * @param {{lineToBin?:(line:string)=>string, verifySha?:boolean}} [opts]
 * @returns {{text:string, rows:number, applied:object}}
 */
export function replay(baseText, ops, opts = {}) {
  const { lineToBin = (l) => l, verifySha = true, header } = opts;
  const B = rowsOf(baseText);
  const n = B.raw.length;
  // 头部**通常**取基线的；条目带了 `header`（产物那 4 行与基线不同）时用它
  const headerLines = header ?? B.header;

  const byI = new Map();
  const dropped = new Set();
  for (const o of ops) {
    if (!Number.isInteger(o.i)) throw new Error(`op.i 必须是整数：${JSON.stringify(o)}`);
    if (o.op === 'insert-after') {
      if (o.i < -1 || o.i > n - 1) throw new Error(`insert-after 的行序越界：i=${o.i}（基线 ${n} 行）`);
    } else if (o.op === 'replace-line' || o.op === 'delete') {
      if (o.i < 0 || o.i >= n) throw new Error(`${o.op} 的行序越界：i=${o.i}（基线 ${n} 行）`);
      if (verifySha && o.sha8 !== sha8(B.masked[o.i])) {
        throw new Error(
          `${o.op} 在第 ${o.i} 行与基线**对不上**（sha8 ${o.sha8} ≠ 基线 ${sha8(B.masked[o.i])}）\n` +
            `  基线该行：${B.raw[o.i]}\n  ⇒ 基线换过了，或被 patch 锚到了错误的位置。**不要**按位置硬套。`,
        );
      }
    } else {
      throw new Error(`不认识的 op：${JSON.stringify(o.op)}`);
    }
    if (!byI.has(o.i)) byI.set(o.i, []);
    byI.get(o.i).push(o);
    if (o.op === 'delete') dropped.add(o.i);
  }

  const prep = (line) => lineToBin(line); // 载荷**已经是符号空间 + 中文**，只需把中文落成 BIN 写法
  const DEF_RE = /^label_[0-9a-f]+$/;
  const defs = new Set();
  /** 放出 `def` 里的**局部 label 定义**；重复定义即抛（否则汇编器会静默用后一个 ⇒ 地址错） */
  const emitDefs = (op, sink) => {
    for (const s of op.def ?? []) {
      if (typeof s !== 'string' || !DEF_RE.test(s)) throw new Error(`def 里的 label 形态非法：${JSON.stringify(s)}`);
      if (defs.has(s)) throw new Error(`label 被定义了两次：${s}（符号必须唯一）`);
      defs.add(s);
      sink.push(s);
    }
  };

  const out = [];
  for (const o of byI.get(-1) ?? []) if (o.op === 'insert-after') { emitDefs(o, out); out.push(prep(o.instr)); }

  let applied = { replaceLine: 0, delete: 0, insert: 0 };
  for (let i = 0; i < n; i += 1) {
    if (!dropped.has(i)) for (const id of B.defsBefore[i]) out.push(symOf(id));
    const list = byI.get(i) ?? [];
    const rep = list.find((o) => o.op === 'replace-line');
    if (rep) { emitDefs(rep, out); out.push(prep(rep.line)); applied.replaceLine += 1; }
    else if (!dropped.has(i)) out.push(symbolize(B.raw[i], B.addrToId));
    for (const o of list) if (o.op === 'insert-after') { emitDefs(o, out); out.push(prep(o.instr)); applied.insert += 1; }
  }
  for (const id of B.defsBefore[n]) out.push(symOf(id));
  if (dropped.size) applied.delete = dropped.size;

  return { text: `${headerLines.join('\n')}\n\n${out.join('\n')}\n`, rows: out.length, applied, header: headerLines };
}

/** 基线 BIN + 条目 ⇒ 重建出来的 BIN（判据的机械版） */
export function rebuildBin(baseBuf, entry, opts = {}) {
  const baseText = disassemble(baseBuf);
  const { text } = replay(baseText, entry.ops, { header: entry.header, ...opts });
  return assemble(text);
}

/**
 * **没有条目的脚本 = 它没有变更**（提取器不为空 patch 建条目）。
 * 视图侧拿它当"空叠加层"用：`src` 视图于是等于 `data` 视图。
 * ★ 它只是**调用方**的便利构造，不会被写进 patch —— 空 ops 的条目不再入库。
 */
export const NO_OPS_ENTRY = (baseBuf) => ({
  baseSha: sha256buf(baseBuf),
  resultSha: sha256buf(baseBuf),
  ops: [],
});

/**
 * 视图文本 → **汇编能吃**的文本（把字符串里的中文落成 BIN 写法），头部 4 行原样。
 *
 * 这是"改过的视图 ⇒ 反解回 patch"的第一步：视图里人是**用中文改**的，而汇编器只认 BIN 写法。
 * 不改 label / 指令名 / 操作数形态 —— 那些归格式层。
 */
export function viewToBinText(viewText, mapper) {
  return viewText
    .split('\n')
    .map((line, i) => (i < 4 ? line : mapper.lineToBin(line)))
    .join('\n');
}

/**
 * **改过的 `src` 视图 ⇒ 新的条目**（判据 3 的后半：编辑回路）。
 *
 * ```
 * 基线 BIN ─┐
 *           ├─ 逐指令对齐 ─▶ ops（锚仍在**基线行序**上，与第一次提取同一条路）
 * 改过的视图 ─┘（先映射成 BIN 写法 → 汇编 → 反汇编，回到"一份 BIN"这个共同面）
 * ```
 *
 * ★ **不叠加**：patch 永远相对**基线**提取；编辑过的视图只是"这一次的产物"，
 *   所以反解出来的 ops 是**全量替换**（不是往旧 ops 上加）——层级要显式才做 patch 链。
 * ★ 头部 4 行不同就抛（`extractEntry` 的既有守卫）：改了 `local_vars` 之类不属于本方案能表达的编辑。
 *
 * @param {Buffer} baseBuf 基线 BIN
 * @param {string} viewText 改过的 `src` 视图文本（`buildView('src', …)` 的同形文本）
 * @param {{lineToBin:(line:string)=>string, mapper:object}} ctx
 * @returns {{entry:object, rebuilt:Buffer}}
 */
export function entryFromView(baseBuf, viewText, ctx) {
  const asmText = viewToBinText(viewText, ctx.mapper);
  const bin = assemble(asmText);
  const entry = extractEntry(baseBuf, bin, ctx.mapper);
  const rebuilt = rebuildBin(baseBuf, entry, { lineToBin: ctx.mapper.lineToBin });
  if (rebuilt.length !== bin.length || !rebuilt.equals(bin)) {
    throw new Error(
      `反解后重建不逐字节相同（重建 ${rebuilt.length} B / 目标 ${bin.length} B）—— ` +
        '这是工具的问题，不是视图的问题；不要写盘',
    );
  }
  return { entry, rebuilt };
}

/**
 * **视图**：`data` 与 `src` 都是**文本**，且两者**同构**（同样的行数、同样的真实地址）。
 *
 * ```
 * data = 基线 BIN 的反汇编
 * src  = 重建出来的 BIN 的反汇编，但**字符串取 patch 里的中文**
 * ```
 *
 * ★ 为什么不能直接把 `replay()` 的文本当 `src`：那份文本里的 label 是**符号**（`label_100000`），
 *   而 `data` 里是**真实地址** —— 形状不同就没法并排读，也没法做"两棵同构树"的同步工具。
 * ★ 为什么不能直接用"重建 BIN 的反汇编"当 `src`：**BIN 里存的是占位写法不是中文**
 *   （简体字多半编不进 cp932，要靠同码位日文写法占位；BIN→中文**不可逆**，见 §2.5）。
 *   中文只存在于 patch 里 ⇒ 视图必须把**字符串从重放流里取**。
 * ★ 两条流长度必然相同（同一次重放、同一次汇编），不同就是**工具错了** ⇒ 抛，不硬凑。
 * ★ 视图**不入库**（生成物）：真值只有基线与 patch；视图可无限重算。
 *
 * @param {'data'|'src'} kind
 * @returns {{text:string, bin:Buffer, stats:object}}
 */
export function buildView(kind, baseBuf, entry, opts = {}) {
  const baseText = disassemble(baseBuf);
  if (kind === 'data') return { text: baseText, bin: baseBuf, stats: { rows: 0, stringSubstitutions: 0 } };

  const { text: replayed } = replay(baseText, entry.ops, { header: entry.header, ...opts });
  const bin = assemble(replayed);
  const direct = disassemble(bin);
  // ★ 载荷里存的是**中文**，而"汇编用的那份文本"已经被映射成 BIN 写法（占位字）——
  //   要拿到中文就得再重放一次（这次不做映射）。两次重放的行结构必然一致（只有字符串不同）。
  const display = rowsOf(replay(baseText, entry.ops, { header: entry.header, ...opts, lineToBin: (l) => l }).text).raw;
  const R = rowsOf(replayed);
  const D = rowsOf(direct);
  if (R.raw.length !== D.raw.length || display.length !== D.raw.length) {
    throw new Error(
      `视图：重放流与重建 BIN 的指令行数不一致（重放 ${R.raw.length} / 展示 ${display.length} / 重建 ${D.raw.length}）` +
        ' —— 这只可能是工具错了（同一次重放、同一次汇编）',
    );
  }

  const stats = { rows: D.raw.length, stringSubstitutions: 0, stringMismatch: 0 };
  // ★ 直接在**重建 BIN 的反汇编文本**上换字符串，而不是自己重排一遍文本：
  //   `disassemble()` 的输出有它自己的形状（label 定义行前留空行等），自己拼必然差一点点。
  let r = 0;
  const lines = direct.split('\n');
  const text = lines
    .map((line, i) => {
      if (i < 4) return line; // 头部 4 行不是指令（与 `rowsOf` 同口径）
      const t = line.replace(/\s+\/\/.*$/, '').trim();
      if (t === '' || LABEL_LINE_RE.test(t)) return line;
      const mergedLine = mergeStringLiterals(line, display[r] ?? line, stats);
      r += 1;
      return mergedLine;
    })
    .join('\n');
  if (r !== R.raw.length) {
    throw new Error(`视图：重建 BIN 的反汇编里数出 ${r} 条指令，重放流里是 ${R.raw.length} 条 —— 工具错了`);
  }
  return { text, bin, stats };
}

/** 行里的字符串常量（与汇编器的 `RE_PARSE_ARGS` 同形：非贪婪、支持 `\x` 转义） */
const QUOTED_G = /"((?:[^"\\]|\\.)*)"/g;

/**
 * 把 `truth` 行里的**字符串常量**搬到 `line` 上（其余部分保持 `line` 原样）。
 * `line` 来自重建 BIN 的反汇编（地址正确、字符串是占位写法），`truth` 来自重放流（字符串是中文）。
 */
export function mergeStringLiterals(line, truth, stats) {
  if (!line.includes('"') && !truth.includes('"')) return line;
  const want = [...truth.matchAll(QUOTED_G)].map((m) => m[0]);
  const have = [...line.matchAll(QUOTED_G)].map((m) => m[0]);
  if (want.length !== have.length) {
    if (stats) stats.stringMismatch += 1;
    return line;
  }
  if (want.every((w, k) => w === have[k])) return line;
  let k = 0;
  const text = line.replace(QUOTED_G, () => want[k++]);
  if (stats) stats.stringSubstitutions += 1;
  return text;
}

// ─────────────────────────────────────────────────────────── 读 / 规范化 / 写

export function loadPatch(p = DEFAULT_PATCH) {
  return JSON.parse(fs.readFileSync(p, 'utf8'));
}

/**
 * **规范形态**是否成立：盘上的字节 == `serializePatch(解析出来的文档)`。
 *
 * 顺序是规范形态的一部分，而且它**不只是好看**：
 *   · `scripts` 的键按**字典序**（脚本名里有 `$`，不排序就会随文件系统给出的顺序漂）；
 *   · 每个条目的 `ops` 按**基线行序非递减**（提取时排过）；
 *   · **一行一个 op**。
 * ⇒ 规范形态成立时，重跑一次提取必得**同字节**的文件，`git diff` 里出现的每一行都是真的改动。
 * ★ 所以"文件被格式化过"也要能红 —— 那会让下一次 diff 混进几千行无意义噪声。
 */
export function canonicalFormProblems(p = DEFAULT_PATCH) {
  const raw = fs.readFileSync(p, 'utf8');
  const again = serializePatch(JSON.parse(raw));
  if (again === raw) return [];
  const a = raw.split('\n');
  const b = again.split('\n');
  const at = a.findIndex((l, i) => l !== b[i]);
  return [
    `不是规范形态（第 ${Math.max(at, 0) + 1} 行起不同）` +
      `\n      盘上: ${JSON.stringify(a[at] ?? '(缺)')}\n      规范: ${JSON.stringify(b[at] ?? '(缺)')}`,
  ];
}

const OP_KEYS = {
  'replace-line': ['op', 'i', 'sha8', 'line', 'def'],
  delete: ['op', 'i', 'sha8'],
  'insert-after': ['op', 'i', 'instr', 'def'],
};

/** 规范化：脚本名排序 + 每个 op 的键序固定 + `_doc` 由工具拥有 ⇒ diff 干净、写入确定 */
export function canonicalPatch(doc) {
  const scripts = {};
  for (const name of Object.keys(doc.scripts ?? {}).sort()) {
    const e = doc.scripts[name];
    scripts[name] = {
      baseSha: e.baseSha,
      resultSha: e.resultSha,
      ...(e.header ? { header: e.header } : {}),
      ops: (e.ops ?? []).map((o) => {
        const out = {};
        for (const k of OP_KEYS[o.op] ?? Object.keys(o)) if (k in o) out[k] = o[k];
        for (const k of Object.keys(o)) if (!(k in out)) out[k] = o[k];
        return out;
      }),
    };
  }
  const out = {
    schemaVersion: doc.schemaVersion,
    _doc: PATCH_DOC,
    subsSha: doc.subsSha,
    scripts,
  };
  for (const k of Object.keys(doc)) if (!(k in out)) out[k] = doc[k];
  return out;
}

/**
 * 行式序列化：**一行一个 op**。`JSON.stringify(doc, null, 1)` 会把一个 op 拆成 7 行，
 * 那样 `git diff` / `git blame` 就退化成一团 —— 而 patch 是**唯一入库的资产**，可 diff 是硬需求。
 */
export function serializePatch(doc) {
  const c = canonicalPatch(doc);
  const L = [];
  L.push('{');
  L.push(` "schemaVersion": ${c.schemaVersion},`);
  L.push(` "_doc": ${JSON.stringify(c._doc)},`);
  L.push(` "subsSha": ${JSON.stringify(c.subsSha)},`);
  L.push(' "scripts": {');
  // ★ 顺序**在这里再排一次**，不靠 `canonicalPatch` 的插入顺序：脚本名里有 `$`（0x24）这类字符，
  //   把顺序交给"对象键的插入顺序"就等于交给一个隐式约定；显式排一次，输出顺序与输入键序无关。
  const names = Object.keys(c.scripts).sort();
  names.forEach((name, ni) => {
    const e = c.scripts[name];
    L.push(`  ${JSON.stringify(name)}: {`);
    L.push(`   "baseSha": ${JSON.stringify(e.baseSha)},`);
    L.push(`   "resultSha": ${JSON.stringify(e.resultSha)},`);
    if (e.header) L.push(`   "header": ${JSON.stringify(e.header)},`);
    if (e.ops.length === 0) L.push('   "ops": []');
    else {
      L.push('   "ops": [');
      e.ops.forEach((o, oi) => L.push(`    ${JSON.stringify(o)}${oi === e.ops.length - 1 ? '' : ','}`));
      L.push('   ]');
    }
    L.push(`  }${ni === names.length - 1 ? '' : ','}`);
  });
  L.push(' }');
  L.push('}');
  return `${L.join('\n')}\n`;
}

const HEX64 = /^[0-9a-f]{64}$/;
const HEX8 = /^[0-9a-f]{8}$/;

/** 结构不变量（不碰基线；基线相关的校验在 `replay()` 与 `verify`） */
export function structuralProblems(doc) {
  const bad = [];
  if (doc?.schemaVersion !== 1) bad.push(`schemaVersion 必须是 1，实际 ${JSON.stringify(doc?.schemaVersion)}`);
  if (!HEX64.test(doc?.subsSha ?? '')) bad.push('subsSha 必须是 64 位小写 hex');
  if (doc?.scripts === null || typeof doc?.scripts !== 'object' || Array.isArray(doc?.scripts)) {
    return [...bad, 'scripts 必须是对象'];
  }
  for (const [name, e] of Object.entries(doc.scripts)) {
    const at = `scripts["${name}"]`;
    if (!isBinName(name)) bad.push(`${at}: 键必须是 .BIN 名字`);
    if (!HEX64.test(e?.baseSha ?? '')) bad.push(`${at}: baseSha 必须是 64 位小写 hex`);
    if (!HEX64.test(e?.resultSha ?? '')) bad.push(`${at}: resultSha 必须是 64 位小写 hex`);
    if ('header' in (e ?? {})) {
      if (!Array.isArray(e.header) || e.header.length !== 4 || !e.header.every((l) => typeof l === 'string')) {
        bad.push(`${at}: header 必须是 4 个字符串（反汇编的头部 4 行）；只在**与基线不同**时才该出现`);
      }
    }
    if (!Array.isArray(e?.ops)) { bad.push(`${at}: ops 必须是数组`); continue; }
    let last = -2;
    let prevNonInsertI = -2;
    for (const [k, o] of e.ops.entries()) {
      const oat = `${at}.ops[${k}]`;
      if (!(o?.op in OP_KEYS)) { bad.push(`${oat}: op 非法 ${JSON.stringify(o?.op)}`); continue; }
      if (!Number.isInteger(o.i)) { bad.push(`${oat}: i 必须是整数`); continue; }
      if (o.op === 'insert-after') {
        if (o.i < -1) bad.push(`${oat}: insert-after 的 i 不得小于 -1`);
      } else if (o.i < 0) bad.push(`${oat}: ${o.op} 的 i 不得为负`);
      if (o.i < last) bad.push(`${oat}: 行序必须非递减（${o.i} < 上一条 ${last}）`);
      if (o.op !== 'insert-after') {
        if (o.i === prevNonInsertI) bad.push(`${oat}: 第 ${o.i} 行上有多条 replace-line/delete（重叠）`);
        prevNonInsertI = o.i;
        if (!HEX8.test(o.sha8 ?? '')) bad.push(`${oat}: sha8 必须是 8 位小写 hex`);
      }
      if (o.op === 'replace-line' && (typeof o.line !== 'string' || o.line === '')) bad.push(`${oat}: line 必须是非空字符串`);
      if (o.op === 'insert-after' && (typeof o.instr !== 'string' || o.instr === '')) bad.push(`${oat}: instr 必须是非空字符串`);
      if (o.op === 'delete' && ('line' in o || 'instr' in o)) bad.push(`${oat}: delete 不该带 line/instr`);
      if ('def' in o) {
        if (o.op === 'delete') bad.push(`${oat}: delete 不该带 def`);
        else if (!Array.isArray(o.def) || o.def.length === 0) bad.push(`${oat}: def 必须是非空数组`);
        else for (const s of o.def) if (typeof s !== 'string' || !/^label_[0-9a-f]+$/.test(s)) bad.push(`${oat}: def 里的 ${JSON.stringify(s)} 不是 label 形态`);
      }
      const extra = Object.keys(o).filter((x) => !OP_KEYS[o.op].includes(x));
      if (extra.length) bad.push(`${oat}: 多余字段 ${extra.join(', ')}（schema 之外）`);
      last = o.i;
    }
  }
  return bad;
}

/** 顶层键（其余一律算多余；`canonicalPatch` 会把未知键挪到尾部而不丢） */
export const KNOWN_TOP_KEYS = ['schemaVersion', '_doc', 'subsSha', 'scripts'];

/**
 * 落盘 + 回读 + 复验；不绿则回滚。是 patch.json 的**唯一写入口**。
 * @returns {{ok:boolean, bytes?:number, restored?:boolean, reason?:string}}
 */
export function savePatch(doc, p = DEFAULT_PATCH) {
  const pre = structuralProblems(doc);
  if (pre.length) return { ok: false, restored: false, reason: `写前预验未通过（一个字都没写）：\n  - ${pre.join('\n  - ')}` };
  const backup = fs.existsSync(p) ? fs.readFileSync(p, 'utf8') : null;
  const text = serializePatch(doc);
  const tmp = `${p}.tmp-${process.pid}`;
  try {
    fs.mkdirSync(path.dirname(p), { recursive: true });
    fs.writeFileSync(tmp, text, 'utf8');
    fs.renameSync(tmp, p);
    const reread = loadPatch(p);
    const post = structuralProblems(reread);
    if (post.length) throw new Error(post.join('\n  - '));
    if (JSON.stringify(canonicalPatch(reread)) !== JSON.stringify(canonicalPatch(doc))) {
      throw new Error('回读后与写前不等价（序列化丢了东西）');
    }
  } catch (err) {
    if (backup === null) fs.rmSync(p, { force: true });
    else fs.writeFileSync(p, backup, 'utf8');
    return { ok: false, restored: true, reason: `写/复验失败（已回滚）：${err.message}` };
  }
  return { ok: true, bytes: Buffer.byteLength(text) };
}

// ─────────────────────────────────────────────────────────── 两侧的根（清单是唯一写绝对路径的地方）

/** 从清单 `roots` 取根；`--base` / `--target` 可覆盖（诊断用） */
export function rootFromManifest(manifest, key) {
  const v = manifest?.roots?.[key];
  if (typeof v !== 'string' || v === '') throw new Error(`清单 roots 里没有 ${key}`);
  return path.isAbsolute(v) ? v : path.resolve(REPO_ROOT, v);
}

/**
 * **产物根**的缺省落点：旧仓 `install/`。
 * ★ 它只在**提取期**被读一次；patch 落库之后运行时只认 `gameInstall/` 与 patch 本身（见 patch-design.md §5）。
 */
export function defaultTargetDir(manifest) {
  return path.join(rootFromManifest(manifest, 'oldRepo'), 'install');
}

/**
 * 官方**标注**集名单（只用于打标签，**不是** patch 的范围）。去重、按名排序。
 * @param {ReturnType<typeof openRoot>} root
 */
export function annotatedNames(root) {
  const out = [];
  for (const n of root.names()) if (SPEAKER_FILTER.test(n) && isBinName(n)) out.push(n);
  return out.sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
}

/**
 * **patch 的范围**：基线根里全部**能反汇编的 AGE 脚本**（不是"官方集"，也不是"有译文的那些"）。
 *
 * ★ 为什么用这个范围而不是按名字筛：译文的存在与否**只能由"产物 ≠ 基线"判定**；
 *   任何名字过滤都会**静默漏掉真译文**（实测就是这样漏掉了 247 支）。
 *   所以这里给出**全部**脚本，交给 `extract` 用"有没有产生 op"来判——没有变更的脚本自然不会进 patch。
 * @param {ReturnType<typeof openRoot>} root
 */
export function allScriptNames(root) {
  const out = [];
  const nonScript = [];
  for (const n of root.names()) {
    if (!isBinName(n)) continue;
    const hit = root.resolve(n);
    if (!hit) continue;
    if (isAgeScript(hit.buf)) out.push(n);
    else nonScript.push(n);
  }
  return { names: out.sort((a, b) => (a < b ? -1 : a > b ? 1 : 0)), nonScript: nonScript.sort() };
}

/**
 * 造一对"基线 / 产物"解析器。
 * @param {{baseDir?:string, targetDir?:string|null}} [opts] `targetDir` 显式给 `null` = 不解析产物（只做自证）
 */
export function openSides(opts = {}) {
  const manifest = loadManifest(DEFAULT_MANIFEST);
  // `targetDir === null` = 显式"不解析产物"；`undefined` = 用缺省落点（旧仓 install/）
  const targetDir = opts.targetDir === undefined ? defaultTargetDir(manifest) : opts.targetDir;
  return {
    manifest,
    base: openRoot(opts.baseDir ?? rootFromManifest(manifest, 'gameInstall')),
    target: targetDir ? openRoot(targetDir) : null,
  };
}

/** 提取期的上下文：字典映射器 + 字典指纹（写进 patch 的那一个） */
export function mapperContext(subsPath = DEFAULT_SUBS) {
  return { mapper: makeMapper(loadDict(subsPath)), subsSha: dictSha(subsPath) };
}

// ─────────────────────────────────────────────────────────── 判据：逐字节

/**
 * 对一个条目跑判据：**基线 + patch ⇒ 逐字节**。
 *
 * 两层，缺一不可：
 *   ① **自证**（离线可跑）：重建的 sha256 == `resultSha`；
 *   ② **对产物**（有产物根时）：重建的字节 == 产物字节。
 * `resultSha` 由 `extract` 从旧仓产物写入 ⇒ ① 不是自证循环，而是"对历史产物的复核"。
 *
 * @returns {{name:string, ok:boolean, problems:string[], stats?:object}}
 */
export function verifyEntry(name, entry, { base, target, lineToBin }) {
  const problems = [];
  const hit = base.resolve(name);
  if (!hit) return { name, ok: false, problems: ['基线无法解析（散装没有、ALF 里也没有）'] };
  const baseSha = sha256buf(hit.buf);
  if (baseSha !== entry.baseSha) {
    return {
      name,
      ok: false,
      problems: [`基线指纹不符：patch ${entry.baseSha.slice(0, 12)}… / 盘上 ${baseSha.slice(0, 12)}…（来源 ${hit.from}）`],
    };
  }
  let rebuilt;
  try {
    rebuilt = rebuildBin(hit.buf, entry, { lineToBin });
  } catch (err) {
    return { name, ok: false, problems: [`重放失败：${err.message}`], baseFrom: hit.from };
  }
  const got = sha256buf(rebuilt);
  if (got !== entry.resultSha) {
    problems.push(`重建结果与 resultSha 不符：重建 ${got.slice(0, 12)}… / 记录 ${entry.resultSha.slice(0, 12)}…`);
  }
  if (target) {
    const t = target.resolve(name);
    if (!t) problems.push('产物无法解析（散装没有、ALF 里也没有）');
    else if (rebuilt.length !== t.buf.length || !rebuilt.equals(t.buf)) {
      problems.push(`与产物不逐字节相同：重建 ${rebuilt.length} B / 产物 ${t.buf.length} B（产物来源 ${t.from}）`);
    }
  }
  return {
    name,
    ok: problems.length === 0,
    problems,
    baseFrom: hit.from,
    stats: { baseBytes: hit.buf.length, resultBytes: rebuilt.length },
  };
}

// ─────────────────────────────────────────────────────────── 自描述

export function describe() {
  return {
    file: 'data/translations/patch.json',
    purpose:
      '**唯一入库的翻译资产**：相对原版 BIN 的变更叠加层。`data`（原版）与 `src`（原版 + patch）都是实时视图，不入库。',
    notHere:
      '基线 BIN 本身、产出的汉化 BIN、`src`/`data` 视图**都不在这里** —— ' +
      '基线由安装目录按「散装优先 → ALF」实时解析（`pnpm tools patch baseline`），视图由 patch 重放得到。',
    topLevel: {
      schemaVersion: 1,
      _doc: '由本工具拥有（写盘时自动注入指向本自描述的指针）',
      subsSha: '简→日写法字典的指纹（构建的一环：换字典 ⇒ 重建结果会变）',
      scripts: '见下方字段表；★ **只收录有变更的脚本** —— 没有变更的脚本**不进 patch**（"没改"不需要记录）',
    },
    fields: FIELD_DOC.map(([name, req, type, desc]) => ({ name, req, type, desc })),
    invariants: INVARIANTS.map(([text, enforcedBy], i) => ({ id: i + 1, text, enforcedBy })),
    operations: OPERATIONS,
    annotatedSet: {
      filter: String(SPEAKER_FILTER),
      note:
        '★ **这不是 patch 的范围**：它是旧仓 `scripts/annotate-speaker.js` 挑文件用的正则，' +
        '含义只有"旧管线给这批做过**页 / 说话人标注**"。实测旧仓 `src/` 有 941 个文本（全部脚本），' +
        '其中**非 SC/SP 的也有译文** ⇒ 曾经的「官方集 = 有译文的脚本」是错的，patch 因此漏过 247 支。' +
        '现在 patch 的范围 = 基线根里全部能反汇编的脚本，有变更才进 patch；这个正则只用来打标签。',
    },
    canonicalForm: {
      indent: '每层 1 个空格',
      scripts: '**按脚本名字典序**（`Object.keys` 的插入顺序不参与：序列化时显式排一次）',
      ops: '每个条目内按**基线行序 `i` 非递减**；同一个 `i` 上 `replace-line`/`delete` 至多一条，`insert-after` 保持产出顺序',
      lines: '**一行一个 op**（`JSON.stringify(…, 1)` 会把一个 op 拆成 7 行，git blame 就废了）',
      invariant: '「盘上字节 == `serializePatch(解析出来的文档)`」由 `canonicalFormProblems()` 判；成立 ⇒ 重跑必得同字节、diff 里出现的每一行都是真改动',
    },
    writePath:
      '唯一写入口是本工具，且只有两条写路径：`extract`（从旧仓产物重取）与 `edit`（把改过的 `src` 视图反解回来）；' +
      '两者都缺省 dry-run，--write 才落盘；写后回读复验，不绿回滚。' +
      '`verify` 只读不写 —— `resultSha` 若能在 verify 里被改写，判据就变成自证循环。**不要手改 JSON**。',
  };
}

export function describeText(d = describe()) {
  const L = [];
  L.push(`# ${d.file} —— 自描述（由控制脚本给出，文档不复述 schema）`);
  L.push('');
  L.push(`用途：${d.purpose}`);
  L.push(`★ ${d.notHere}`);
  L.push('');
  L.push('## 顶层');
  for (const [k, v] of Object.entries(d.topLevel)) L.push(`* \`${k}\`: ${v}`);
  L.push('');
  L.push('## 字段');
  L.push('| 字段 | 必填 | 类型 / 枚举 | 说明 |');
  L.push('|---|---|---|---|');
  for (const f of d.fields) L.push(`| \`${f.name}\` | ${f.req} | ${f.type} | ${f.desc} |`);
  L.push('');
  L.push(`## 范围与标签（\`scripts\` 的键从哪来）`);
  L.push(`**范围**：基线根里全部能反汇编的 AGE 脚本；**有变更才进 patch**（没变更的不进）。`);
  L.push(`**标签** \`annotated\`：\`SPEAKER_FILTER\`（说话人标注的口径 ${d.annotatedSet.filter}）—— 只表示"做过说话人标注"，**不是一类脚本**。`);
  L.push(d.annotatedSet.note);
  L.push('');
  L.push('## 规范形态（顺序是它的一部分）');
  for (const [k, v] of Object.entries(d.canonicalForm)) L.push(`* ${k}：${v}`);
  L.push('');
  L.push('## 不变量（含"谁在守它"）');
  for (const c of d.invariants) L.push(`${c.id}. ${c.text}　—　${c.enforcedBy}`);
  L.push('');
  L.push('## 操作');
  for (const o of d.operations) L.push(`* \`${o.name}\`${o.mutates ? '（会写）' : ''} —— ${o.summary}　→ \`pnpm tools patch ${o.name}\``);
  L.push('');
  L.push(`写入口：${d.writePath}`);
  return `${L.join('\n')}\n`;
}
