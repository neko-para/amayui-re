/**
 * tools/lib/catalog.mjs —— **语料 item 目录与影响面**（派生视图；真源是语料本身）
 *
 * ## 它回答什么问题
 * 语料是**两侧**的：`.lst`（IDA 反汇编，**带地址**）与 `.c`（Hex-Rays 伪代码，**一处地址都没有**）。
 * 分析真正面对的单位不是"文件里的某一段行"，而是 **item**：
 * * **函数** —— `.lst` 的 `proc near … endp`（身份 = 函数起点 **EA**，名字 = `sub_XXXXXX` 等）
 * * **定义** —— `.lst` 里带名字的数据标签（`dword_559A68` / `off_51D574` / `jpt_418BC7` …）
 * 本模块把两侧**逐 item 切开、按名字关联**，并在同一遍扫描里提取**影响面**需要的机械事实：
 * 调用边（含边来源）、全局变量的读/写、参数的栈偏移 / 寄存器、函数指针表。
 *
 * ## 四条口径（决定了每个函数签名）
 * 1. **身份是 EA，不是行号**。两侧的行区间是**派生值**：换一次导出就全变（`AGENTS.md` §6.2 的锚只锚 EA）。
 *    所以本模块算出来的 `{lstFrom,lstTo,cFrom,cTo}` **不许进台账当锚** —— 它们只用于"打印哪几行"。
 * 2. **不是每个 EA 都是函数起点、也不是每个函数都有 C**。前者用 `functionOfEa`（二分 + `lastEa` 上界）算，
 *    后者**明说**（`c: null` + `cNote`），绝不把"没读到 C"读成"这段没有逻辑"。
 * 3. **缺事实要报错，不许静默降级**。`findFunction` 定位不到就抛；解不出目标的调用点**只计数**、不猜目标；
 *    对不上 `.lst` 的 C 定义**列出来**（修饰名要 demangle 才能对，本模块**不做** demangle）。
 * 4. **诚实项与结论一起出**：任何"调用闭包"都只是**下界**（间接调用 / 取址后动态分派）；
 *    所以 `impact()` 的返回值里**必须**带上未解析调用点数与"可能调用方"的另册。
 *
 * ## 边（edge）的来源是**闭集合**，每条边都带来源
 * | kind | 从哪来 | 可信度 |
 * |---|---|---|
 * | `call` | `.lst` 体内 `call sub_XXXXXX` | 权威 |
 * | `tail` | `.lst` 体内 `jmp sub_XXXXXX`（尾跳同样是"把控制权交出去"） | 权威 |
 * | `c-call` | `.c` 体内 `sub_XXXXXX(` | 补充（Hex-Rays 会内联 ⇒ 与 lst 会不一致，**不一致要报**） |
 * | `c-addr` | `.c` 体内 `sub_XXXXXX` **不带** `(`（取址：函数指针 / vtable 槽） | 只是"可能调用方" |
 * | `table:<EA>` | 数据 item 里的 `dd offset sub_XXXXXX`（函数指针表 / 跳转表） | 只是"可能调用方" |
 *
 * ## 已知的机械边界（**照抄，不掩盖**）
 * * **间接调用解不出目标**：`call eax` / `call [esi+4]` / `call ds:dword_X`（实测 3144 处）⇒ 闭包是下界。
 *   ★ 其中 `call ds:dword_X` 是**旧实现整类漏掉**的形态（实测 69 处被静默丢出调用图）—— 格式词汇表的
 *   `INDIRECT_CALL_RE` 现在含 `ds:`/`es:`；这条教训与"段名以 `.` 开头"同类：**漏掉的表现是数字变小，不是报错**。
 * * **全局变量的读/写是按操作数位置判定的**，不是数据流分析（`x = x + 1` 记 `rw`）。
 * * **参数偏移**：`.lst` 的声明行给偏移与宽度；`.c` 给名字与类型。两者的配对是**按位置**的
 *   （第 i 个 C 参数 ↔ `.lst` 的 `arg_{4i}` = `[ebp+8+4i]`；`__thiscall` 的 `this` 在 `ecx`）。
 *   `__usercall` / `__fastcall` 的寄存器分配是 Hex-Rays 自定的 ⇒ 只标注、**不猜偏移**。
 * * **`.` 结尾不是 `.`**：`.c` 里的结构偏移是**十进制**（`this + 280` = `.lst` 的 `[esi+118h]`），
 *   跨侧比偏移必须数值归一化；本模块只把两侧的字面形态各自原样给出。
 */
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';

import {
  AUTO_NAME_RE,
  CALL_RE,
  CALL_SYM_RE,
  DATA_KINDS,
  DISP_H_RE,
  ENDP_RE,
  INDIRECT_CALL_RE,
  MEM_OPERAND_BODY_RE,
  MEM_OPERAND_RE,
  PROC_EA_RE,
  REG_RE,
  STACK_BASE_RE,
  eaOfAutoName,
  parseListingLine,
  undecorate,
} from './lst-format.mjs';
import { pickDecompiled } from './disasm.mjs';

// ─────────────────────────────────────────────────────────── 语料对（模块）解析

/** 基线模块的键（`AGE.EXE__dumped.sectfix.EXE` 那一对；另一对是 `AGERC.DLL`） */
export const BASELINE_KEY = 'age';

/**
 * `corpus/disasm/files/` 里的语料对（**一对 = 一个 `.lst` + 同名 `.c`**）。
 * ★ 不写死模块表：目录里有什么就有什么（写死了必与目录漂）。`baseline` 判据 = 文件名含 `dumped`
 *   （即清单里那份 `AGE.EXE__dumped.sectfix.EXE`），**不是**字典序第一（字典序会把 AGERC 排前面）。
 */
export function listModules(filesDir) {
  if (!fs.existsSync(filesDir)) return [];
  return fs
    .readdirSync(filesDir)
    .filter((f) => f.endsWith('.lst'))
    .sort()
    .map((f) => {
      const lstFile = path.join(filesDir, f);
      const cFile = pickDecompiled(lstFile);
      const base = f.replace(/\.lst$/, '');
      const baseline = /dumped/i.test(base);
      return {
        key: baseline ? BASELINE_KEY : base.replace(/[^A-Za-z0-9]+/g, '-').replace(/^-|-$/g, '').toLowerCase(),
        base,
        lstFile,
        cFile,
        baseline,
      };
    });
}

/** 键 / 文件名前缀 → 一个语料对；找不到 ⇒ `null`（**调用方必须明说**，不许静默取第一个） */
export function resolveModule(filesDir, key = null) {
  const mods = listModules(filesDir);
  if (mods.length === 0) return null;
  if (key === null || key === undefined || key === '') return mods.find((m) => m.baseline) ?? mods[0];
  const k = String(key).toLowerCase();
  return mods.find((m) => m.key === k || m.base.toLowerCase() === k || m.base.toLowerCase().startsWith(k)) ?? null;
}

// ─────────────────────────────────────────────────────────── `.lst` 文法补充

/** 助记符 + 操作数（`.lst` 的正文列：助记符小写、与操作数之间是空格） */
const MNEMONIC_RE = /^([a-z][a-z0-9]*)\s*(.*)$/;
/**
 * 栈槽声明行：`arg_0= dword ptr  8` / `var_D45C= dword ptr -0D45Ch` / `Src= dword ptr  8`。
 * ★ 名字**不是** `arg_XX` 也照样出现（IDA 允许改名：实测 `Src=` / `hMem=` / `Buffer=`）
 *   ⇒ 判据只能是"**偏移**"，不是"名字以 arg_ 开头"。
 */
const DECL_RE = /^([A-Za-z_$?@][\w.$?@]*)=\s*([A-Za-z_]\w*)\s+ptr\s+(-?)([0-9A-Fa-f]+)(h?)\s*$/;
/** 数据标签行：`unk_51D520 db    0` / `First dd 0`（名字 + 指令） */
const DATA_LABEL_RE = /^([A-Za-z_$?@][\w.$?@]*)\s+(db|dw|dd|dq|dt|unicode|align)\b(.*)$/;
/** 数据续行：`db    0` / `dd offset sub_4EE07A`（**无名字**，属于上一个标签） */
const DATA_CONT_RE = /^(db|dw|dd|dq|dt|unicode|align)\b(.*)$/;
/** 每个数据指令的**元素宽度**（字节）。`align` 是填充、不算元素（`0` ⇒ 不累加长度）。 */
const DIR_BYTES = { db: 1, dw: 2, dd: 4, dq: 8, dt: 10, tbyte: 10, unicode: 2, align: 0 };
/** `8 dup(0)` ⇒ 8 个元素 */
const DUP_RE = /^(\d+)\s+dup\(/i;
/** 不修改内存的一元助记符（用于把"名字在第一个操作数"判成读）。其余一元指令保守地判成写。 */
const READ_ONLY_ONE_OP = new Set(['push', 'call', 'jmp', 'jz', 'jnz', 'nop', 'retn', 'leave', 'test', 'cmp']);
/** 读-改-写助记符：名字在第一个操作数 ⇒ `rw`（不是纯写） */
const RMW_MNEMONICS = new Set([
  'add', 'sub', 'adc', 'sbb', 'and', 'or', 'xor', 'inc', 'dec', 'neg', 'not',
  'shl', 'shr', 'sar', 'sal', 'rol', 'ror', 'rcl', 'rcr', 'xchg', 'bts', 'btr', 'btc',
]);
/** 栈槽引用：`[ebp+arg_4]` / `[ebp+hMem]` / `[esp+var_10]`（名字要按声明表反查，不能只认 `arg_`） */
const STACK_REF_RE = /\[(e?[sb]p)\+([A-Za-z_$?@][\w.$?@]*)\]/g;

/** 把一行正文切成"助记符 + 操作数数组"（操作数按**顶层逗号**切；括号内的逗号不算） */
function splitOperands(rest) {
  const out = [];
  let depth = 0;
  let cur = '';
  for (const ch of rest) {
    if (ch === '[' || ch === '(') depth += 1;
    else if (ch === ']' || ch === ')') depth -= 1;
    if (ch === ',' && depth <= 0) {
      out.push(cur.trim());
      cur = '';
    } else cur += ch;
  }
  if (cur.trim() !== '') out.push(cur.trim());
  return out;
}

/**
 * 数据指令的**元素个数**（`db 0` ⇒ 1；`dd 1, 2` ⇒ 2；`8 dup(0)` ⇒ 8）；解不出 ⇒ 1。
 *
 * ★ 两个实测坑（都会让"约多少字节"这个数**静默偏小或偏大**）：
 * 1. **引号里的逗号**：`db '1,4,8,24,32',0` 按顶层逗号切会数成 6 个元素（真实 14 字节）；
 * 2. **字符串是一个元素、却占很多字节**：`db 'ERROR',0` 是 2 个"元素"而**真的 6 字节** ——
 *    元素数要把**字符串的字符数**算进去（否则 `.lst` 给 2、`.c` 写着 `char aError[6]`，两侧对不上）。
 */
function dataElementCount(operands) {
  if (!operands) return 1;
  const s = operands.trim();
  const dup = DUP_RE.exec(s);
  if (dup) return Number.parseInt(dup[1], 10);
  let strBytes = 0;
  let strSlots = 0;
  const masked = s
    .replace(/'((?:[^'\\]|\\.)*)'/g, (_m, inner) => {
      strBytes += inner.replace(/\\./g, 'X').length;
      strSlots += 1;
      return 'X';
    })
    .replace(/"((?:[^"\\]|\\.)*)"/g, (_m, inner) => {
      strBytes += inner.replace(/\\./g, 'X').length;
      strSlots += 1;
      return 'X';
    });
  const parts = splitOperands(masked);
  if (parts.length === 0) return 1;
  // 字符串各占 1 个"元素位"，其**字符数**加到元素总数上（再减掉那 1 个占位）
  return parts.length + strBytes - strSlots;
}

// ─────────────────────────────────────────────────────────── `.lst` 扫描（一遍）

/**
 * **一遍**扫完 `.lst`：函数清单 + 数据 item + 调用边 + 全局访问 + 栈槽声明。
 *
 * 为什么要"一遍"：语料 53 万行 / 18 MB，扫一遍实测 ~0.4 s。旧实现为了拿齐这些事实要扫三遍
 * （callee 一遍、全局一遍、参数一遍），而每一遍都会各自实现一份判据 ⇒ 判据分叉。
 *
 * @param {string[]} lines  `.lst` 的行数组（末尾通常有一个空串）
 * @returns {{functions:object[], data:object[], tableEdges:object[], dataEas:Set<number>, indirectCallSites:number}}
 */
export function scanListing(lines) {
  /** 非 `.text` 段里的行 EA（= 数据/全局的定义点）：锚落在这些地址上**不是**"归属失败" */
  const dataEas = new Set();
  const functions = [];
  const data = [];
  const tableEdges = [];
  let cur = null; // 当前函数
  let curData = null; // 当前数据 item
  let indirectCallSites = 0;

  for (let i = 0; i < lines.length; i += 1) {
    const line = lines[i];
    const pl = parseListingLine(line);
    if (!pl) continue;
    const { seg, ea, body } = pl;
    // 段名不是 `.text` ⇒ 记下它的 EA
    if (seg !== '.text') dataEas.add(ea);

    const p = PROC_EA_RE.exec(line);
    if (p) {
      // ★ 函数头**先于**数据判定：`proc near` 也可能落在非 `.text` 段（本语料没有，但不假设）
      if (curData) curData = null;
      cur = {
        sym: p[3],
        ea: Number.parseInt(p[2], 16),
        seg,
        fromLine: i + 1,
        toLine: null,
        lastEa: Number.parseInt(p[2], 16),
        // ★ 扫描期用 `*Hits` 这组 Map 攒计数；收尾时**同时**给出两样东西（对外形状见文件头"item 字段"）：
        //   · `callees` / `tailCalls` / `externalTargets` = **符号数组**（下游最常要的是"集合"）
        //   · `calleeHits` / `externalHits` = **命中次数的 Map**（"110 个调用方"与"205 处调用"要分得开）
        callHits: new Map(),
        tailHits: new Map(),
        externalHits: new Map(),
        callSites: 0,
        indirectCallSites: 0,
        hasMemoryOperand: false,
        maxNonStackDisp: 0,
        globals: new Map(), // 全局名 → {name,ea,kind,rw,hits}
        decls: new Map(), // 栈槽名 → {name,offset,width,kind}
        stackRefs: new Map(), // 栈槽名 → 引用次数
        declPhase: true,
      };
      functions.push(cur);
      continue;
    }
    if (!cur) {
      // 不在函数里 ⇒ 数据 item 的地盘
      const lab = DATA_LABEL_RE.exec(body);
      if (lab) {
        const name = lab[1];
        const dir = lab[2];
        curData = {
          name,
          ea,
          seg,
          dir,
          fromLine: i + 1,
          toLine: i + 1,
          lastEa: ea,
          sizeBytes: DIR_BYTES[dir] === 0 ? 0 : dataElementCount(lab[3]) * DIR_BYTES[dir],
          tableTargets: [],
          kind: eaOfAutoName(name) !== null ? 'auto' : 'named',
        };
        data.push(curData);
        if (dir !== 'align') collectTableTargets(curData, ea, lab[3], tableEdges);
        continue;
      }
      const cont = DATA_CONT_RE.exec(body);
      if (cont && curData) {
        const dir = cont[1];
        curData.toLine = i + 1;
        curData.lastEa = ea;
        if (dir !== 'align') {
          curData.sizeBytes += dataElementCount(cont[2]) * DIR_BYTES[dir];
          collectTableTargets(curData, ea, cont[2], tableEdges);
        }
        continue;
      }
      continue;
    }

    // ── 在函数体内 ──
    const le = /^[A-Za-z_.][\w.]*:([0-9A-Fa-f]{8})\s/.exec(line);
    if (le) {
      const v = Number.parseInt(le[1], 16);
      if (v > cur.lastEa) cur.lastEa = v;
    }
    if (ENDP_RE.test(line)) {
      cur.toLine = i + 1;
      cur = null;
      continue;
    }
    const decl = DECL_RE.exec(body);
    if (decl && cur.declPhase) {
      const sign = decl[3] === '-' ? -1 : 1;
      const raw = Number.parseInt(decl[4], 16); // ★ 两种形态都按十六进制读：`8` / `0Ch` / `-0D45Ch`
      const offset = sign * raw;
      const width = { db: 1, dw: 2, dd: 4, dq: 8, dt: 10 }[decl[2]] ?? 4;
      cur.decls.set(decl[1], { name: decl[1], offset, width, kind: offset >= 8 ? 'arg' : 'local' });
      continue;
    }
    if (body === '' || body.startsWith(';')) continue; // 空行 / 纯注释不结束声明区
    if (cur.declPhase && !body.startsWith('align')) {
      // 第一条真指令 ⇒ 声明区结束（IDA 把 `var_` / `arg_` 声明全放在第一条指令之前）
      if (MNEMONIC_RE.test(body)) cur.declPhase = false;
    }

    if (MEM_OPERAND_RE.test(line)) cur.hasMemoryOperand = true;
    if (line.includes('[')) {
      MEM_OPERAND_BODY_RE.lastIndex = 0;
      let mo;
      while ((mo = MEM_OPERAND_BODY_RE.exec(line))) {
        const inner = mo[1];
        if (STACK_BASE_RE.test(inner)) continue;
        DISP_H_RE.lastIndex = 0;
        let d;
        while ((d = DISP_H_RE.exec(inner))) {
          const v = Number.parseInt(d[1], 16);
          if (v > cur.maxNonStackDisp) cur.maxNonStackDisp = v;
        }
      }
    }

    // 调用 / 跳转（顺序有意义：先认 `sub_XXXXXX` 的直呼，再认具名符号，最后才是"解不出目标"）
    const c = CALL_RE.exec(line);
    if (c) {
      cur.callSites += 1;
      const map = c[1] === 'jmp' ? cur.tailHits : cur.callHits;
      map.set(c[2], (map.get(c[2]) ?? 0) + 1);
      continue;
    }
    const e = CALL_SYM_RE.exec(line);
    if (e && !REG_RE.test(e[2])) {
      cur.callSites += 1;
      cur.externalHits.set(e[2], (cur.externalHits.get(e[2]) ?? 0) + 1);
      continue;
    }
    if (INDIRECT_CALL_RE.test(line)) {
      cur.callSites += 1;
      cur.indirectCallSites += 1;
      indirectCallSites += 1;
    }

    // 全局变量的读 / 写（按操作数位置判定）
    if (body.indexOf('_') >= 0) {
      const mn = MNEMONIC_RE.exec(body);
      if (mn) {
        const operands = splitOperands(mn[2]);
        AUTO_NAME_RE.lastIndex = 0;
        for (const hit of body.matchAll(AUTO_NAME_RE)) {
          const kind = hit[1];
          if (!DATA_KINDS.has(kind)) continue; // sub_/loc_/def_ 这些是代码，不是全局
          const name = hit[0];
          const at = operands.findIndex((o) => o.includes(name));
          let rw = 'r';
          if (at === 0) rw = RMW_MNEMONICS.has(mn[1]) ? 'rw' : READ_ONLY_ONE_OP.has(mn[1]) ? 'r' : 'w';
          else if (at > 0) rw = operands[0].includes(name) ? 'rw' : 'r';
          const g = cur.globals.get(name) ?? { name, ea: eaOfAutoName(name), kind, rw: 'r', hits: 0 };
          g.hits += 1;
          if (rw !== 'r') {
            if (g.rw === 'r') g.rw = rw;
            else if (g.rw !== rw) g.rw = 'rw';
          }
          cur.globals.set(name, g);
        }
      }
    }

    // 栈槽引用（声明表反查；声明里没有的也记下来 —— "被引用但没见声明"是事实，不许静默丢）
    if (body.includes('[e') && body.includes('+')) {
      STACK_REF_RE.lastIndex = 0;
      let sr;
      while ((sr = STACK_REF_RE.exec(body))) {
        const nm = sr[2];
        cur.stackRefs.set(nm, (cur.stackRefs.get(nm) ?? 0) + 1);
      }
    }
  }
  if (cur) cur.toLine = cur.fromLine; // 未闭合（异常导出）⇒ 只说"只有头一行"

  for (const f of functions) {
    const sortPairs = (m) => new Map([...m].sort((a, b) => (a[0] < b[0] ? -1 : 1)));
    f.callHits = sortPairs(f.callHits);
    f.tailHits = sortPairs(f.tailHits);
    f.externalHits = sortPairs(f.externalHits);
    // 对外：**符号数组**（下游要的是"集合"）+ **命中次数 Map**（"110 个调用方"与"205 处调用"要分得开）
    f.callees = [...f.callHits.keys()];
    f.tailCalls = [...f.tailHits.keys()];
    f.externalTargets = [...f.externalHits.keys()];
    f.calleeHits = sortPairs(new Map([...f.callHits, ...f.tailHits]));
    f.calleeSyms = [...new Set([...f.callees, ...f.tailCalls])].sort();
    f.globals = [...f.globals.values()].sort((a, b) => (a.name < b.name ? -1 : 1));
    f.decls = [...f.decls.values()].sort((a, b) => a.offset - b.offset);
    f.args = f.decls.filter((d) => d.kind === 'arg');
    f.locals = f.decls.filter((d) => d.kind === 'local');
    // ★ **被引用但没见声明**的栈槽也是事实（IDA 确实会漏声明：实测 `sub_401100` 引用了 `arg_8` 却没声明它）。
    //   对 `arg_XX` / `var_XX` 这种名字，**偏移可以从名字直接算出来**（`arg_N` ⇒ `[ebp+8+N]`，`var_N` ⇒ `[ebp-N]`）
    //   ⇒ 补进 args/locals，并标 `declared:false`（是谁、从哪来的要看得见）。
    const declaredNames = new Set(f.decls.map((d) => d.name));
    f.refOnly = [...f.stackRefs.keys()]
      .filter((n) => !declaredNames.has(n))
      .map((name) => {
        const m = /^(arg|var)_([0-9A-F]+)$/.exec(name);
        const offset = m ? (m[1] === 'arg' ? 8 + Number.parseInt(m[2], 16) : -Number.parseInt(m[2], 16)) : null;
        return { name, offset, width: 4, kind: m ? (m[1] === 'arg' ? 'arg' : 'local') : 'unknown', hits: f.stackRefs.get(name), declared: false };
      })
      .sort((a, b) => (a.offset ?? 0) - (b.offset ?? 0));
  }
  return { functions, data, tableEdges, dataEas, indirectCallSites };
}

/** 数据 item 里的函数指针：`dd offset sub_XXXXXX` / `dd offset ___onexitinit` */
function collectTableTargets(item, ea, operands, out) {
  if (!operands || !operands.includes('offset')) return;
  for (const m of operands.matchAll(/\boffset\s+([A-Za-z_$?@][\w.$?@]*)/g)) {
    const target = m[1];
    item.tableTargets.push(target);
    out.push({ atEa: ea, dataName: item.name, dataEa: item.ea, target, isSub: /^sub_[0-9A-F]{6}$/.test(target) });
  }
}

// ─────────────────────────────────────────────────────────── `.c` 扫描（一遍）

/** 调用约定关键字（`.c` 每行签名上都印着；★ 顶部的 `#define __thiscall __cdecl` 是**编译垫片**，不是真源） */
const CONV_RE = /__(thiscall|cdecl|stdcall|fastcall|usercall|userpurge)\b/;
/** 参数上的寄存器标注：`_BYTE *a1@<edx>`（`__usercall` / `__userpurge` 用它把寄存器写在**参数**上） */
const PARAM_REG_RE = /\s*@<([a-z0-9]+)>\s*$/;
/** 参数里这些词是**类型**、不是参数名 */
const TYPE_WORDS = new Set([
  'int', 'char', 'short', 'long', 'unsigned', 'signed', 'void', 'float', 'double', 'bool', 'wchar_t',
  'const', 'struct', 'union', 'enum', 'int64', 'DWORD', 'WORD', 'BYTE', 'BOOL', 'LPCSTR', 'LPSTR',
  'HANDLE', 'HWND', 'UINT', 'SIZE_T', 'LPVOID', 'HGLOBAL', 'HMODULE', 'HRESULT', 'va_list', 'FILE',
]);

/**
 * **一遍**扫完 `.c`：函数定义 + 数据定义 + 体内对 `sub_XXXXXX` 的调用/取址。
 *
 * ★ **定义 vs 原型**的判据（**这里就是旧实现出错的地方**，所以写成一条规则、只此一处）：
 *   定义行 = 列 0 起、以 `)` 结尾、**且下一个非空行恰为 `{`**。
 *   为什么不能用旧的 `/;\s*$/`：带尾注释的原型（`int __stdcall sub_40B6C0(void *Src, …); // idb`）
 *   **不以 `;` 结尾**（结尾是注释）⇒ 被当成定义 ⇒ 该符号的行区间落在**文件顶部的原型区**
 *   （实测 40 个符号；`sub_40B6C0` 的真定义在 `.c:15186`，旧实现给的是 `.c:275` 的**单行原型**，
 *    而调用方会把那一行当成"C 体"打出来，**不报错**）。
 *   反过来说：原型行**不以 `)` 结尾**，所以这条新规则对它们天然免疫（不需要额外排除）。
 *
 * @param {string[]} lines `.c` 的行数组
 */
export function scanDecompiled(lines) {
  const funcs = [];
  const dataDefs = [];
  const byName = new Map();
  const defHeadLines = new Set();
  const defBodyRanges = [];
  // ★ **原型区边界**（**独立判据**）：`.c` 顶部那片函数原型以 `);`（可跟注释）收尾，而**定义不会**。
  //   有了它，"某条定义落在原型区"才判得出来 —— 否则判据会**自证**（拿定义集合自己的最小值当边界：
  //   把一条原型误判成定义，边界跟着变小，"落在原型区"永远不触发）。
  let lastProtoLine = 0;
  for (let i = 0; i < lines.length; i += 1) {
    // 先记原型边界（原型行**不会**通过下面的 `readSignature` ⇒ 不在这里记就永远抓不到它）
    if (/^[A-Za-z_]/.test(lines[i]) && /\bsub_[0-9A-F]{6}\s*\(/.test(lines[i]) && /\);\s*(\/\/.*)?$/.test(lines[i])) lastProtoLine = i + 1;
    const sig = readSignature(lines, i);
    if (!sig) continue;
    const noComment = sig.text;
    const paren = noComment.indexOf('(');
    const head = noComment.slice(0, paren).trim();
    // ★ `__usercall` 形态把寄存器写在名字里：`int __usercall sub_404FF0@<eax>(int)` ⇒ 名字要去掉 `@<eax>`；
    //   返回指针的函数还会印成 `int **sub_453DE0(...)` ⇒ 前导 `*` 也要去掉。
    //   （不去掉的话这些定义**全部**对不上 `.lst` 符号 —— 实测 22 个，全是这两个后缀/前缀造成的。）
    const name = (head.split(/\s+/).pop() ?? '').replace(/^[*\s]+/, '').replace(/(?:@<[^>]*>)+$/, '');
    if (name === '') continue;
    let to = null;
    for (let k = sig.bodyOpen + 1; k < lines.length; k += 1) {
      if (lines[k] === '}') {
        to = k + 1;
        break;
      }
    }
    const def = {
      name,
      from: i + 1,
      to: to ?? i + 1,
      conv: CONV_RE.exec(noComment)?.[0] ?? null,
      params: parseParams(balancedRest(noComment.slice(paren))),
      cCalls: new Map(),
      cAddrTaken: new Set(),
      cExternalCalls: new Set(),
      cExternalAddrTaken: new Set(),
    };
    funcs.push(def);
    defHeadLines.add(def.from);
    for (let k = i; k <= sig.sigEnd; k += 1) defHeadLines.add(k + 1);
    defBodyRanges.push([i + 1, def.to]);
    if (!byName.has(name)) byName.set(name, def);
    // 体内扫 `sub_XXXXXX`：跟着 `(` ⇒ 调用；否则 ⇒ 取址
    for (let k = i; k <= (to ?? sig.sigEnd); k += 1) {
      const body = lines[k].replace(/\/\/.*$/, '');
      for (const m of body.matchAll(/\b(sub_[0-9A-F]{6})\s*(\()?/g)) {
        if (m[2] === '(') def.cCalls.set(m[1], (def.cCalls.get(m[1]) ?? 0) + 1);
        else def.cAddrTaken.add(m[1]);
      }
      for (const m of body.matchAll(/\b([A-Za-z_$?@][\w.$?@]*)\s*\(/g)) {
        const nm = m[1];
        if (/^sub_[0-9A-F]{6}$/.test(nm) || TYPE_WORDS.has(nm)) continue;
        if (['if', 'while', 'for', 'switch', 'return', 'sizeof', 'else', 'do'].includes(nm)) continue;
        def.cExternalCalls.add(nm);
      }
    }
    i = sig.sigEnd; // 跳过已消化的签名行（体内那些行由上面的循环处理）
  }
  // 数据定义（列 0、**没有括号**、以 `;` + 可选注释结尾、不是函数定义/签名行）
  // ★ 必须排除带括号的行：`.c` 顶部那一大片**函数原型**也以 `;` 结尾，
  //   宽松的正则会从参数表里抠出一个假名字（实测把 `unsigned int a3` 抠成"数据定义" ⇒ 数量虚高）。
  for (let i = 0; i < lines.length; i += 1) {
    const l = lines[i];
    if (!/^[A-Za-z_]/.test(l)) continue;
    if (defHeadLines.has(i + 1)) continue;
    const noComment = l.replace(/\/\/.*$/, '').trimEnd();
    if (!noComment.endsWith(';') || noComment.includes('(') || noComment.includes(')')) continue;
    const m = /^.*?[* ]([A-Za-z_$?@][\w.$?@]*)\s*(\[[^\]]*\])?\s*(=.*)?;$/.exec(noComment);
    if (!m) continue;
    const name = m[1];
    if (TYPE_WORDS.has(name)) continue;
    dataDefs.push({ name, line: i + 1, ea: eaOfAutoName(name), raw: l.trim() });
  }
  return { funcs, byName, dataDefs, lastProtoLine };
}

/**
 * 读一条**函数签名**（可能**跨多行**）：从第 `i` 行起配平括号，返回签名文本 + 结束行 + 函数体 `{` 的行号。
 *
 * ★ 为什么必须处理跨行：`int __thiscall sub_XXXXXX(int a1,` / `int a2)` / `{` 这种签名在本语料里真实存在，
 *   而"只认单行 + 行尾是 `)`"的判据会**整条漏掉**它（漏掉的表现是"这个函数没有 C" —— 与"Hex-Rays 没反编译"
 *   长得一模一样，属于静默降级）。
 * ★ 原型照样被排除：签名之后是 `;`（跨行原型同理）⇒ 返回 `null`。
 */
function readSignature(lines, i) {
  const first = lines[i].replace(/\/\/.*$/, '').trimEnd();
  if (!/^[A-Za-z_]/.test(first) || !first.includes('(')) return null;
  let text = first;
  let depth = 0;
  let k = i;
  for (;;) {
    depth = 0;
    for (const ch of text) {
      if (ch === '(') depth += 1;
      else if (ch === ')') depth -= 1;
    }
    if (depth <= 0) break;
    if (k - i >= 30) return null; // 30 行还配不平 ⇒ 不是签名
    k += 1;
    if (k >= lines.length) return null;
    text += ` ${lines[k].replace(/\/\/.*$/, '').trim()}`;
  }
  if (text.slice(text.lastIndexOf(')') + 1).trim().startsWith(';')) return null; // 原型
  let j = k + 1;
  while (j < lines.length && lines[j].trim() === '') j += 1;
  if (lines[j] !== '{') return null; // 不是"定义 + 紧跟函数体"
  return { text, sigStart: i, sigEnd: k, bodyOpen: j };
}

/** 取从 `(` 起**配平**的一段（参数表可能含嵌套括号：`void (__stdcall *a1)(int)`） */
function balancedRest(s) {
  let depth = 0;
  for (let i = 0; i < s.length; i += 1) {
    if (s[i] === '(') depth += 1;
    else if (s[i] === ')') {
      depth -= 1;
      if (depth === 0) return s.slice(1, i);
    }
  }
  return s.slice(1);
}

/** 参数表 → `[{i, name, type, reg, raw}]`（按**位置**；名字解不出就给 `null`，不许编） */
function parseParams(src) {
  const parts = splitOperands(src.replace(/\bvoid\b\s*$/, ''));
  return parts
    .filter((p) => p.trim() !== '' && p.trim() !== 'void')
    .map((raw, i) => {
      const cleaned = raw.trim().replace(/\)\s*\(.*$/, '');
      // ★ `@<reg>` 标注：`_BYTE *a1@<edx>` ⇒ 名字 `a1`、寄存器 `edx`（`__usercall`/`__userpurge` 的寄存器信息全在这里）
      const regM = PARAM_REG_RE.exec(cleaned);
      const noReg = regM ? cleaned.slice(0, regM.index).trim() : cleaned;
      const nm = /([A-Za-z_$?@][\w.$?@]*)\s*(\[[^\]]*\])?\s*$/.exec(noReg);
      let name = nm ? nm[1] : null;
      if (name && TYPE_WORDS.has(name)) name = null;
      if (name && noReg.replace(/[*\s]/g, '') === name) name = null;
      return { i, name, type: noReg, reg: regM ? regM[1] : null, raw: cleaned };
    });
}

// ─────────────────────────────────────────────────────────── 装配 / 关联

/**
 * 建**目录**：两侧各扫一遍，然后按名字关联。
 *
 * @param {object} o
 * @param {string} o.lstFile  `.lst` 路径（必填）
 * @param {string|null|'auto'} [o.cFile]  `'auto'`（缺省）= 同名 `.c` 在场就用；`null` = 明确不要 C 侧
 * @param {string|null} [o.moduleKey]
 * @param {string|null} [o.filesDir]  给模块解析用（只在 `--module` 语义下需要）
 */
export function buildCatalog({ lstFile, cFile = 'auto', moduleKey = null } = {}) {
  if (!lstFile) throw new Error('buildCatalog 需要 lstFile');
  const lstBuf = fs.readFileSync(lstFile);
  const lstLines = lstBuf.toString('utf8').split('\n');
  const cPath = cFile === 'auto' ? pickDecompiled(lstFile) : cFile;
  const cBuf = cPath ? fs.readFileSync(cPath) : null;
  const lst = scanListing(lstLines);
  const c = cBuf ? scanDecompiled(cBuf.toString('utf8').split('\n')) : null;

  // ── 关联（名字 = 唯一钥匙；`.lst` 的修饰名要先去壳） ──
  const bySym = new Map();
  for (const f of lst.functions) if (!bySym.has(f.sym)) bySym.set(f.sym, f);
  let withC = 0;
  const cMatched = new Set();
  for (const f of lst.functions) {
    const hit = c ? (c.byName.get(f.sym) ?? c.byName.get(undecorate(f.sym))) : null;
    if (hit) {
      f.c = { from: hit.from, to: hit.to, name: hit.name, conv: hit.conv, params: hit.params };
      f.conv = hit.conv;
      f.cCalls = hit.cCalls;
      f.cAddrTaken = hit.cAddrTaken;
      f.cExternalCalls = hit.cExternalCalls;
      f.cExternalAddrTaken = hit.cExternalAddrTaken;
      cMatched.add(hit.name);
      withC += 1;
    } else {
      f.c = null;
      f.conv = null;
      f.cCalls = new Map();
      f.cAddrTaken = new Set();
      f.cExternalCalls = new Set();
      f.cExternalAddrTaken = new Set();
    }
    f.hasC = f.c !== null;
    // ★ 调用约定的**诚实标注**：`__usercall` / `__userpurge` 的寄存器/栈由 Hex-Rays 自定
    //   （能拿到的是参数上的 `@<reg>` 标注；**拿不到的不猜**）。
    f.convNote =
      f.conv === '__usercall' || f.conv === '__userpurge'
        ? `${f.conv}：寄存器/栈布局由 Hex-Rays 自定 ⇒ 只有带 \`@<reg>\` 标注的参数才有确定的寄存器；其余按顺序配，未标注的偏移**不可机械判定**`
        : null;
    const paired = pairArgs(f);
    f.args = paired.args;
    f.pairNotes = paired.notes;
  }
  const cDefsUnmatched = c ? c.funcs.filter((d) => !cMatched.has(d.name)).map((d) => ({ name: d.name, from: d.from })) : [];

  // ── 调用方索引 / 取址索引 / 全局索引 ──
  const callers = new Map(); // 被调符号 → [{from, kind, hits}]
  const addrTaken = new Map(); // 被取址符号 → [{from, kind}]
  for (const f of lst.functions) {
    for (const [kind, map] of [['call', f.callHits], ['tail', f.tailHits]]) {
      for (const [to, hits] of map) {
        if (!callers.has(to)) callers.set(to, []);
        callers.get(to).push({ from: f.sym, fromEa: f.ea, kind, hits });
      }
    }
    for (const to of f.cAddrTaken) {
      if (!addrTaken.has(to)) addrTaken.set(to, []);
      addrTaken.get(to).push({ from: f.sym, kind: 'c-addr' });
    }
  }
  for (const t of lst.tableEdges) {
    if (!addrTaken.has(t.target)) addrTaken.set(t.target, []);
    addrTaken.get(t.target).push({ from: `data:${t.dataName}`, fromEa: t.atEa, kind: 'table' });
  }
  const globalIndex = new Map(); // 全局名 → [{sym, ea, rw, hits}]
  for (const f of lst.functions) {
    for (const g of f.globals) {
      if (!globalIndex.has(g.name)) globalIndex.set(g.name, []);
      globalIndex.get(g.name).push({ sym: f.sym, ea: f.ea, rw: g.rw, hits: g.hits });
    }
  }
  const dataByName = new Map();
  for (const d of lst.data) if (!dataByName.has(d.name)) dataByName.set(d.name, d);
  const dataByEa = new Map();
  for (const d of lst.data) if (!dataByEa.has(d.ea)) dataByEa.set(d.ea, d);
  // ── 定义（数据 item）也按**名字**关联：`.c` 的 `char aError[6] = "ERROR";` ↔ `.lst` 的
  //    `.data:0051D6BC aError db 'ERROR',0`（实测逐字同名）⇒ 把 C 侧的类型/初值挂到有 EA 的那一侧。
  //    ★ 对不上的 C 数据定义**列出来**（它们多半是 `char aXxx[]` 这种可读名，EA 解不出 ⇒ 明说）。
  const cDataMatched = [];
  const cDataUnmatched = [];
  if (c) {
    for (const d of c.dataDefs) {
      const hit = dataByName.get(d.name);
      if (hit) {
        hit.c = { line: d.line, raw: d.raw };
        cDataMatched.push({ name: d.name, line: d.line, dataEa: hit.ea });
      } else {
        cDataUnmatched.push({ name: d.name, line: d.line, ea: d.ea });
      }
    }
  }

  const starts = lst.functions.map((f) => f.ea);
  const calleesNotInInventory = [...new Set(lst.functions.flatMap((f) => f.calleeSyms).filter((s) => !bySym.has(s)))].sort();
  const cat = {
    module: moduleKey ?? path.basename(lstFile).replace(/\.lst$/, ''),
    lstFile,
    cFile: cPath,
    sha: {
      lst: crypto.createHash('sha256').update(lstBuf).digest('hex'),
      c: cBuf ? crypto.createHash('sha256').update(cBuf).digest('hex') : null,
    },
    functions: lst.functions,
    bySym,
    starts,
    data: lst.data,
    dataByName,
    dataByEa,
    dataEas: lst.dataEas,
    tableEdges: lst.tableEdges,
    callers,
    addrTaken,
    globalIndex,
    indirectCallSites: lst.indirectCallSites,
    c: c ? { funcs: c.funcs, byName: c.byName, dataDefs: c.dataDefs, lastProtoLine: c.lastProtoLine, unmatched: cDefsUnmatched, dataMatched: cDataMatched, dataUnmatched: cDataUnmatched } : null,
    stats: {
      functions: lst.functions.length,
      withC,
      withoutC: lst.functions.length - withC,
      dataItems: lst.data.length,
      dataEas: lst.dataEas.size,
      tableEdges: lst.tableEdges.length,
      indirectCallSites: lst.indirectCallSites,
      callSitesTotal: lst.functions.reduce((n, f) => n + f.callSites, 0),
      callEdges: [...callers.values()].reduce((n, a) => n + a.length, 0),
      addrTakenTargets: addrTaken.size,
      calleesNotInInventory,
      cDefs: c ? c.funcs.length : 0,
      cDefsUnmatched: cDefsUnmatched.length,
      cDataDefs: c ? c.dataDefs.length : 0,
      cDataMatched: cDataMatched.length,
      cDataUnmatched: cDataUnmatched.length,
    },
  };
  cat.problems = catalogProblems(cat);
  return cat;
}

/**
 * **参数配对**：`.lst` 给偏移/宽度/声明名、`.c` 给名字/类型/顺序。
 *
 * 为什么**不能只按位置**（实测反例）：`sub_401100`（`__thiscall`）的 `.lst` 声明是
 * `lpFileName= dword ptr 8` + `arg_4= dword ptr 0Ch`，而 `.c` 签名是 `(int this, LPCSTR lpFileName, const char *a3)`。
 * 位置法会把 `a3` 配到 `[ebp+10h]`（`.lst` 那里**根本没有声明**），而把 `lpFileName` 配到 `[ebp+0Ch]` —— 全错。
 * 名字法立刻对上（两边都叫 `lpFileName`）。⇒ 规则改成：
 * 1. **先按名字配**（IDA 与 Hex-Rays 共用同一份数据库 ⇒ 名字常常逐字相同）；
 * 2. 剩下的**按顺序配**（两边各按序；`Src` ↔ `a2` 这种改名就靠这条）；
 * 3. 寄存器参数（`__thiscall` 的 `this` 在 `ecx`）单独列，**没有偏移**；
 * 4. 多出来的一边**明说**（`pairNotes`）—— 那正是"Hex-Rays 原型与 IDA 声明不一致"的信号，不许静默丢。
 *
 * @returns {{args:object[], notes:string[]}}
 */
function pairArgs(f) {
  const declared = f.decls.filter((d) => d.kind === 'arg').map((d) => ({ ...d, declared: true }));
  const refOnly = (f.refOnly ?? []).filter((a) => a.kind === 'arg' && typeof a.offset === 'number');
  const out = [...declared, ...refOnly]
    .sort((a, b) => a.offset - b.offset)
    .map((a) => ({ ...a, cName: null, cType: null, cIndex: null, pairBy: a.declared ? 'lst' : 'ref' }));
  const notes = [];
  if (!f.c) return { args: out, notes };
  const regParams = [];
  const stackParams = [];
  f.c.params.forEach((p) => {
    // ★ 寄存器的**真源**是参数上的 `@<reg>` 标注（`__usercall` / `__userpurge`）；
    //   只有 `__thiscall` / `__fastcall` 才按约定**推**寄存器。推不出来的**不猜**（列成"解不出"的栈参）。
    let reg = p.reg ?? null;
    if (!reg && f.conv === '__thiscall' && p.i === 0) reg = 'ecx';
    if (!reg && f.conv === '__fastcall') {
      if (p.i === 0) reg = 'ecx';
      else if (p.i === 1) reg = 'edx';
    }
    if (reg) regParams.push({ ...p, reg });
    else stackParams.push(p);
  });
  for (const p of regParams) {
    out.push({
      name: p.reg ? `<${p.reg}>` : '<usercall>',
      offset: null,
      reg: p.reg,
      width: 4,
      kind: 'arg',
      declared: false,
      cName: p.name,
      cType: p.type,
      cIndex: p.i,
      pairBy: 'reg',
      note: p.note,
    });
  }
  // ① 按名字配
  const claimed = new Set();
  for (const p of stackParams) {
    if (!p.name) continue;
    const hit = out.find((o) => o.offset !== null && o.cIndex === null && o.pairBy !== 'reg' && o.name === p.name);
    if (hit) {
      hit.cName = p.name;
      hit.cType = p.type;
      hit.cIndex = p.i;
      hit.pairBy = 'name';
      claimed.add(p.i);
    }
  }
  // ② 剩下的按顺序配
  const rest = stackParams.filter((p) => !claimed.has(p.i));
  const free = out.filter((o) => o.offset !== null && o.cIndex === null);
  for (let k = 0; k < Math.min(rest.length, free.length); k += 1) {
    const p = rest[k];
    const o = free[k];
    o.cName = p.name;
    o.cType = p.type;
    o.cIndex = p.i;
    o.pairBy = 'order';
    claimed.add(p.i);
  }
  // ③ 多出来的一边：明说
  const cOnly = stackParams.filter((p) => !claimed.has(p.i));
  for (const p of cOnly) {
    out.push({ name: '<未在 .lst 声明>', offset: null, width: 4, kind: 'arg', declared: false, cName: p.name, cType: p.type, cIndex: p.i, pairBy: 'c-only' });
  }
  const lstOnly = out.filter((o) => o.offset !== null && o.cIndex === null);
  if (cOnly.length > 0) notes.push(`C 侧多出 ${cOnly.length} 个栈参（.lst 里没有对应声明/引用）`);
  if (lstOnly.length > 0) notes.push(`.lst 侧多出 ${lstOnly.length} 个栈槽（C 侧没有对应参数）`);
  return { args: out.sort((a, b) => (a.offset ?? -1) - (b.offset ?? -1)), notes };
}

// ─────────────────────────────────────────────────────────── 查询

/** EA → **包含它的函数**（二分；不在任何函数里 ⇒ `null`）。签名与旧 `functionOfEa(inv, ea)` 兼容。 */
export function functionOfEa(cat, ea) {
  const { starts, functions } = cat;
  if (!functions || functions.length === 0) return null;
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
  const next = functions[hit + 1];
  if (next && ea >= next.ea) return null;
  if (ea > f.lastEa) return null;
  return f.sym;
}

/** 同 `functionOfEa`，但给**整个 item**（`show` / `impact` 要它） */
export function functionItemOfEa(cat, ea) {
  const sym = functionOfEa(cat, ea);
  return sym === null ? null : cat.bySym.get(sym);
}

/**
 * 定位一个 item：给 `sym` 或 `ea`（二选一）。
 * ★ 定位不到**抛错**（`--ea` 在函数体内也照样定位到所属函数，并明说"你给的 EA 不是函数起点"）。
 */
export function findFunction(cat, { sym = null, ea = null } = {}) {
  if (ea !== null && ea !== undefined) {
    const target = typeof ea === 'number' ? ea : Number.parseInt(String(ea).replace(/^0x/i, ''), 16);
    const f = functionItemOfEa(cat, target);
    if (!f) {
      const segs = [...new Set(cat.functions.map((x) => x.seg))].join(' / ');
      throw new Error(
        `EA 0x${target.toString(16)} 不落在任何函数里（本目录覆盖的段：${segs}；` +
          `函数 EA 范围 0x${Math.min(...cat.starts).toString(16)}..0x${Math.max(...cat.starts).toString(16)}）`,
      );
    }
    return { item: f, isStart: f.ea === target, queriedEa: target };
  }
  if (sym) {
    const f = cat.bySym.get(sym) ?? cat.bySym.get(undecorate(sym));
    if (!f) {
      const near = [...cat.bySym.keys()].filter((k) => k.includes(String(sym).slice(0, 6))).slice(0, 5);
      throw new Error(
        `符号 ${JSON.stringify(sym)} 不在目录里（共 ${cat.bySym.size} 个函数）` +
          (near.length ? `；名字相近的有：${near.join(' / ')}` : '') +
          '。★ 具名/修饰名函数（`WinMain` / `_WinMain@16`）也在目录里，但 C 侧对不上的会明说。',
      );
    }
    return { item: f, isStart: true, queriedEa: f.ea };
  }
  throw new Error('findFunction 需要 sym 或 ea');
}

/** 一个函数的**调用方**（权威边：`call` / `tail`） */
export function callersOf(cat, item) {
  return [...(cat.callers.get(item.sym) ?? [])].sort((a, b) => (a.from < b.from ? -1 : 1));
}

/** 一个函数的**被调用方**（权威边：`call` / `tail`；★ 间接调用解不出目标，只计数） */
export function calleesOf(cat, item) {
  const out = [];
  for (const [to, hits] of item.callHits) out.push({ to, kind: 'call', hits, ea: cat.bySym.get(to)?.ea ?? null });
  for (const [to, hits] of item.tailHits) out.push({ to, kind: 'tail', hits, ea: cat.bySym.get(to)?.ea ?? null });
  return out.sort((a, b) => (a.to < b.to ? -1 : 1));
}

/** 「以它为目标的**间接**证据」（取址 / 函数指针表）—— 这些**不是**能机械确定的调用方 */
export function addrEvidenceOf(cat, item) {
  return cat.addrTaken.get(item.sym) ?? [];
}

/**
 * **影响面**：沿 `call` / `tail` 做传递闭包。
 * @param {'up'|'down'} direction  `up` = 谁（间接）会到它 ⇒ 改它的影响面；`down` = 它依赖谁
 * @returns {{direction, depth, nodes:object[], edges:object[], truncated:boolean, honesty:object}}
 */
export function impact(cat, item, { direction = 'up', depth = 3, maxNodes = 200 } = {}) {
  const seen = new Map([[item.sym, 0]]);
  const nodes = [];
  const edges = [];
  let truncated = false;
  let frontier = [item];
  for (let d = 1; d <= depth; d += 1) {
    const next = [];
    for (const f of frontier) {
      const list = direction === 'up'
        ? callersOf(cat, f).map((c) => ({ other: c.from, kind: c.kind, hits: c.hits }))
        : calleesOf(cat, f).map((c) => ({ other: c.to, kind: c.kind, hits: c.hits }));
      for (const l of list) {
        if (!cat.bySym.has(l.other)) continue;
        edges.push({ from: f.sym, to: l.other, kind: l.kind, depth: d });
        if (!seen.has(l.other)) {
          if (seen.size >= maxNodes) {
            truncated = true;
            continue;
          }
          seen.set(l.other, d);
          const it = cat.bySym.get(l.other);
          nodes.push({ sym: l.other, ea: it.ea, depth: d });
          next.push(it);
        }
      }
    }
    frontier = next;
    if (frontier.length === 0) break;
  }
  // ★ 诚实项：任何闭包都只是**下界**
  const reached = [item, ...nodes.map((n) => cat.bySym.get(n.sym))];
  const honesty = {
    indirectInReached: reached.reduce((n, f) => n + f.indirectCallSites, 0),
    indirectInCorpus: cat.indirectCallSites,
    addrEvidence: direction === 'up' ? addrEvidenceOf(cat, item) : [],
    unresolvedCallees: direction === 'down' ? item.indirectCallSites : 0,
    note:
      '`call eax` / `call [esi+4]` / `call ds:dword_X` 这类调用点**解不出目标** ⇒ 闭包是**下界**；' +
      '「取址证据」里的调用方是**动态分派**（函数指针 / vtable），本模块**不猜**它会不会真的调到。',
  };
  return { direction, depth, root: { sym: item.sym, ea: item.ea }, nodes, edges, truncated, honesty };
}

/** 一个函数碰过的全局变量（按 读/写 标注） */
export function globalsOf(cat, item) {
  return item.globals;
}

/**
 * EA → **包含它的数据 item**（`[ea, lastEa]`；不在任何 item 里 ⇒ `null`）。
 * ★ 与 `functionOfEa` 对称：数据 item 也是**区间**（一个标签 + 它的续行），不是单点。
 *   没有它就会出现"`off_51D574` 不在数据标签里"这种**看起来对、其实错**的答案 ——
 *   因为那个 EA 属于标签 `First`（表项只是它的续行）。
 */
export function dataItemOfEa(cat, ea) {
  const target = typeof ea === 'number' ? ea : Number.parseInt(String(ea).replace(/^0x/i, ''), 16);
  for (const d of cat.data) {
    if (target >= d.ea && target <= d.lastEa) return d;
  }
  return null;
}

/** 谁碰过某个全局（给 `name` 或 `ea`；自动名会先落到**包含它的数据 item**） */
export function whoTouchesGlobal(cat, { name = null, ea = null } = {}) {
  if (name && cat.globalIndex.has(name)) {
    return { name, item: cat.dataByName.get(name) ?? null, users: cat.globalIndex.get(name) };
  }
  let target = null;
  if (ea !== null && ea !== undefined) target = typeof ea === 'number' ? ea : Number.parseInt(String(ea).replace(/^0x/i, ''), 16);
  else if (name) {
    const v = eaOfAutoName(name);
    if (v !== null) target = v;
  }
  if (target !== null) {
    const item = dataItemOfEa(cat, target);
    if (item) return { name: item.name, item, users: cat.globalIndex.get(item.name) ?? [] };
    return { name: name ?? `0x${target.toString(16)}`, item: null, users: [] };
  }
  return { name, item: cat.dataByName.get(name) ?? null, users: [] };
}

// ─────────────────────────────────────────────────────────── 不变量自检

/**
 * **目录自洽**（守卫的判据来源）。这里只放**契约型**判据（"任何一份语料都不许违反"），
 * 不放"当前是多少个函数"（那是快照，不是不变量 —— `AGENTS.md` §10）。
 */
export function catalogProblems(cat) {
  const p = [];
  const fns = [...cat.functions];
  // ① 函数 lst 区间：有序、不重叠
  for (let i = 0; i < fns.length; i += 1) {
    const f = fns[i];
    if (!(f.fromLine >= 1 && f.toLine >= f.fromLine)) p.push(`${f.sym} 的 lst 区间非法：${f.fromLine}-${f.toLine}`);
    if (i > 0 && fns[i - 1].toLine >= f.fromLine) p.push(`${fns[i - 1].sym} 与 ${f.sym} 的 lst 区间重叠`);
  }
  // ② 名字唯一（IDA 偶有同名 ⇒ 必须报出来，不许静默取第一个）
  for (const [sym] of cat.bySym) {
    if (fns.filter((f) => f.sym === sym).length > 1) p.push(`函数符号重复：${sym}`);
  }
  // ③ C 行区间：**不许落在原型区**（判据 = 定义行必须 ≥ 第一条函数定义行）、有序不重叠
  const cDefs = fns.filter((f) => f.c).map((f) => ({ sym: f.sym, ...f.c })).sort((a, b) => a.from - b.from);
  if (cDefs.length > 0) {
    // ★ 边界用**原型区**的独立判据（最后一条 `…);` 形态的原型行），**不是**定义集合自己的最小值：
    //   后者会自证 —— 把一条原型误判成定义，边界也跟着变小，于是"落在原型区"永远判不出来。
    const protoEnd = cat.c?.lastProtoLine ?? 0;
    for (const d of cDefs) {
      if (!(d.to >= d.from)) p.push(`${d.sym} 的 C 区间非法：${d.from}-${d.to}`);
      if (d.from <= protoEnd) p.push(`${d.sym} 的 C 起点 ${d.from} 落在原型区（最后一条原型在 ${protoEnd}）`);
    }
    for (let i = 1; i < cDefs.length; i += 1) {
      if (cDefs[i - 1].to >= cDefs[i].from) p.push(`${cDefs[i - 1].sym} 与 ${cDefs[i].sym} 的 C 区间重叠`);
    }
  }
  // ④ 函数 EA：单调、lastEa 覆盖自身
  for (let i = 0; i < fns.length; i += 1) {
    if (fns[i].lastEa < fns[i].ea) p.push(`${fns[i].sym} 的 lastEa ${fns[i].lastEa.toString(16)} < 起点 ${fns[i].ea.toString(16)}`);
    if (i > 0 && fns[i].ea < fns[i - 1].ea) p.push(`函数 EA 非单调：${fns[i - 1].sym} → ${fns[i].sym}`);
  }
  // ⑤ 数据 item：行区间有序
  for (let i = 0; i < cat.data.length; i += 1) {
    const d = cat.data[i];
    if (!(d.toLine >= d.fromLine)) p.push(`数据 item ${d.name} 的区间非法：${d.fromLine}-${d.toLine}`);
    if (i > 0 && cat.data[i - 1].toLine >= d.fromLine) p.push(`数据 item ${cat.data[i - 1].name} 与 ${d.name} 区间重叠`);
  }
  // ⑥ 全局索引与 per-function 表**双向一致**（防"索引只建了一半"）
  const fromFns = fns.reduce((n, f) => n + f.globals.length, 0);
  const fromIdx = [...cat.globalIndex.values()].reduce((n, a) => n + a.length, 0);
  if (fromFns !== fromIdx) p.push(`全局索引不一致：per-function ${fromFns} 条 vs 索引 ${fromIdx} 条`);
  // ⑦ 声明表里的**参数**偏移必须与位置自洽（`arg_{4i}` ↔ `[ebp+8+4i]`，允许改名）
  //    ★ 只查"有数值偏移的参数"：寄存器参数（`<ecx>`）与 C 侧补的槽位没有偏移，不该被判非法。
  for (const f of fns) {
    for (const a of f.args) {
      if (typeof a.offset !== 'number') continue;
      if (a.offset < 8 || (a.offset - 8) % 4 !== 0) p.push(`${f.sym} 的参数 ${a.name} 偏移 ${a.offset} 不对齐（期望 8+4i）`);
    }
  }
  return p;
}

// ─────────────────────────────────────────────────────────── 快照（产物，不是查询路径）

/**
 * **派生快照**：一 item 一行、制表符分列（可 diff、可 grep、可给下游读）。
 * ★ 它**不是查询路径** —— 查询（`show` / `impact` / …）永远**现算**（实测整个目录 ~0.4 s），
 *   所以本模块**不提供** `parseCatalog`：少一套"缓存陈旧了怎么办"的机制。
 * ★ 快照带两侧语料的 **sha256**：语料换了而快照没重建，是**看得出来**的。
 */
export function serializeCatalog(cat) {
  const L = [
    '# disasm catalog v1 —— **派生快照**（可删可重建；真源是语料本身；锚点锚 EA，行号只是派生值）',
    `# module\t${cat.module}`,
    `# lst\tsha256=${cat.sha.lst}\tfile=${path.basename(cat.lstFile)}`,
    `# c\tsha256=${cat.sha.c ?? '-'}\tfile=${cat.cFile ? path.basename(cat.cFile) : '-'}`,
    `# counts\tfunctions=${cat.stats.functions}\twithC=${cat.stats.withC}\tdata=${cat.stats.dataItems}` +
      `\tcallEdges=${cat.stats.callEdges}\tindirect=${cat.stats.indirectCallSites}`,
  ];
  for (const f of cat.functions) {
    L.push(
      [
        'F',
        `ea=${f.ea.toString(16)}`,
        `sym=${f.sym}`,
        `seg=${f.seg}`,
        `lst=${f.fromLine}:${f.toLine}`,
        `c=${f.c ? `${f.c.from}:${f.c.to}` : '-'}`,
        `conv=${f.conv ?? '-'}`,
        `sites=${f.callSites}`,
        `ind=${f.indirectCallSites}`,
        `args=${f.args.map((a) => `${a.name}@${a.offset}${a.cName ? `(${a.cName})` : ''}`).join(',') || '-'}`,
        `globals=${f.globals.map((g) => `${g.name}:${g.rw}:${g.hits}`).join(',') || '-'}`,
        `callees=${f.calleeSyms.map((s) => `${s}:${f.calleeHits.get(s) ?? 0}`).join(',') || '-'}`,
        `ext=${f.externalTargets.join(',') || '-'}`,
      ].join('\t'),
    );
  }
  for (const d of cat.data) {
    L.push(
      [
        'D',
        `ea=${d.ea.toString(16)}`,
        `name=${d.name}`,
        `seg=${d.seg}`,
        `dir=${d.dir}`,
        `lines=${d.fromLine}:${d.toLine}`,
        `size=${d.sizeBytes}`,
        `table=${d.tableTargets.join(',') || '-'}`,
      ].join('\t'),
    );
  }
  return `${L.join('\n')}\n`;
}

/** 目录一览（打印用；**不塞 marks/函数全表** —— 那可能几百 KB） */
export function catalogSummary(cat) {
  return {
    module: cat.module,
    lst: path.basename(cat.lstFile),
    c: cat.cFile ? path.basename(cat.cFile) : null,
    sha: cat.sha,
    ...cat.stats,
    problems: cat.problems,
  };
}

// ─────────────────────────────────────────────────────────── 自描述

export function describe() {
  return {
    file: 'corpus/disasm/files/*.{lst,c}（**只读**语料）+ `.cache/disasm/*.catalog.txt`（**派生快照**，可删）',
    purpose:
      '把语料切成 **item**（函数 / 定义），按**符号名（= EA）**关联两侧，并在同一遍扫描里给出影响面要的机械事实：' +
      '调用边（带来源）、全局变量的读/写、参数的栈偏移 / 寄存器、函数指针表。',
    notHere:
      '① 不做**语义**（"这个函数是干什么的"仍要人读 + 过 `AGENTS.md` §6 准入门）；② 不做 **demangle**' +
      '（`??2@YAPAXI@Z` 这类对不上 `.c` 的**列出来**，不猜）；③ 不宣布"调用闭包完整"（间接调用解不出目标 ⇒ 永远是下界）；' +
      '④ **行号不是锚**：快照里的 `lst=…` / `c=…` 只是"打印哪几行"，换一次导出就全变。',
    itemFields: [
      ['ea', '函数起点 EA（身份；`.c` 里没有地址 ⇒ 名字是唯一回连的钥匙）'],
      ['sym', '`.lst` 的 `proc near` 符号（`sub_XXXXXX` / `_WinMain@16`）'],
      ['lst', '`{fromLine,toLine,lastEa}` —— **派生值**，不是锚'],
      ['c', '`{from,to,name,conv,params} | null`；`null` = Hex-Rays 没反编译它（**明说**，不是"没逻辑"）'],
      ['conv', '调用约定（来自 `.c` 签名；顶部 `#define __thiscall __cdecl` 是编译垫片，不算真源）'],
      ['args', '参数：`{name, offset, width, kind, cName, cType, reg?}`；两边**按位置**配对'],
      ['globals', '碰过的全局：`{name, ea, kind, rw, hits}`（`rw` 按操作数位置判，不是数据流）'],
      ['callees / tailCalls', '`call` / `jmp sub_XXXXXX`（权威边）'],
      ['cCalls / cAddrTaken', '`.c` 里的调用 / 取址（**补充**；Hex-Rays 会内联 ⇒ 与 lst 会不一致）'],
      ['indirectCallSites', '解不出目标的调用点（`call eax` / `call [esi+4]` / `call ds:dword_X`）'],
    ],
    edgeKinds: ['call', 'tail', 'c-call', 'c-addr', 'table:<EA>'],
    invariants: [
      { id: 1, text: '函数/数据 item 的 **lst 行区间两两不重叠**，且 `from ≤ to`', enforcedBy: 'catalogProblems ①⑤ + tools/test/disasm-fn.assets.test.mjs' },
      { id: 2, text: 'C 侧行区间**不许落在原型区**（定义行 ≥ 第一条函数定义行），且两两不重叠', enforcedBy: 'catalogProblems ③ + 同上的守卫（这条正是旧实现出错的地方）' },
      { id: 3, text: '函数 **EA 单调**、`lastEa ≥ ea`；`functionOfEa` 用 `[ea, lastEa]` 上界（不把 `.data` 归属给最后一个函数）', enforcedBy: 'catalogProblems ④ + tools/test/ledger-coverage.test.mjs' },
      { id: 4, text: '**全局索引与 per-function 表双向一致**（防"索引只建了一半"）', enforcedBy: 'catalogProblems ⑥' },
      { id: 5, text: '参数偏移必须 `8 + 4i` 对齐（`.lst` 的 `arg_{4i}` ↔ `[ebp+8+4i]`）', enforcedBy: 'catalogProblems ⑦' },
      { id: 6, text: '缺事实**报错/报出**：定位不到抛错、没有 C 明说、对不上的 C 定义列出来、间接调用只计数', enforcedBy: 'findFunction 抛错 + stats.cDefsUnmatched + impact().honesty' },
      { id: 7, text: '语料**只读**；快照落 `.cache/`，带两侧 sha256', enforcedBy: 'tools/test/disasm-fn.assets.test.mjs + corpus/assets.json 的 readOnly' },
    ],
    honesty: [
      '`indirectCallSites` —— 任何"调用闭包"都只是**下界**（含 `call ds:dword_X`，旧的 `INDIRECT_CALL_RE` 把这一整类**静默漏掉**，实测 69 处）',
      '`cDefsUnmatched` —— `.c` 里有定义、但名字对不上任何 `.lst` 符号（修饰名要 demangle；本模块不做）',
      '`calleesNotInInventory` —— `callee` 符号在本文件里找不到（跨模块 / 导入）',
      '`addrTaken` —— 取址 + 函数指针表：这些是**可能调用方**，不是能机械确定的边',
      '`c` 与 `lst` 的调用集合不一致 —— Hex-Rays 内联 / 折叠会造出这种差；**报出来，不合并**',
    ],
  };
}
