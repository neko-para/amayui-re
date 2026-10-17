/**
 * tools/lib/shape-propagate.mjs —— **类型传播（全图 BFS）**：批次顺序与"类判定"的唯一机械依据。
 *
 * ★ 为什么不能按"偏移命中率"猜类：低命中**既可能**是另一个类、**也可能**只是 T0 的成员表还没覆盖到。
 *   实测：1697 个未建模函数里 744 个"T0 命中 ≤10%"，但其中大多数只是"T0 的偏移还没声明"。
 * ⇒ 唯一站得住的判据是**调用链**：调用方在调用点上把什么值传给被调方的根寄存器。
 *
 * 传播规则（只用已证明的）：
 *  · `this` / `interior(this+k)` ⇒ 与被调方同类（内指向量不改变类）；
 *  · `member(path)` ⇒ 沿 `types` 路径解析出目标类型（可能是**另一个类**）；
 *  · `val` / `unknown` ⇒ **不传播**（⛔ 不猜）；
 *  · 只对"有函数体的直接调用"传播（间接调用目标未知 ⇒ 不传播，由闭合度点名）。
 */
import fs from 'node:fs';

const num = (v) => Number.parseInt(String(v), 16);

/** 沿路径解析成员类型（`types[cur][hop]` 必须是 `kind:"ptr"` + `type`） */
export function resolveMemberPath(types, selfType, path) {
  let cur = selfType;
  for (const hop of path ?? []) {
    const t = types[cur];
    if (!t) return null;
    const m = (t.members ?? []).find((x) => num(x.off) === hop);
    if (!m || m.kind !== 'ptr' || !m.type) return null;
    cur = m.type;
  }
  return cur;
}

/**
 * 从已知载体出发做 BFS。
 * `analyzeFn(sym)` 由调用方提供（返回 `{links}` / `null`），这样本模块**不绑**语料读取方式（便于合成用例）。
 *
 * ★ `globals`：**全局载体**（`kind:"global"`，`sym` → 类型 id）。为什么必须有这一类 ——
 *   根对象（Engine / `Command`）**不是参数**：它在 `_WinMain@16` 里 `operator new(0xAABC8)`、
 *   `call sub_415640`（构造器）之后才 `mov ds:dword_55E1BC, eax` 存进全局（`0x4baa11/0x4baa2e/0x4baa3a`）。
 *   ⇒ 没有它，"从根正向爬"一步都走不了，只能继续从 op handler 倒着爬。
 *   ★ 规则与 `member` 一样是**机械的**：调用点上 `ecx` 的来源是"某个全局槽里装着的东西"（`k:'global'`），
 *     而这个全局有声明 ⇒ 被调方的根类型 = 该声明。⛔ 没有声明的全局一律不传播（不猜）。
 */
export function propagate({ carriers, types, analyzeFn, maxHops = 6, globals = null, extraCallers = null, acceptTarget = null }) {
  const known = new Map();     // `${fn}|${reg}` → 类型 id
  const inferred = new Map();  // fn → { type, hops, via }
  const GLOB = globals ?? new Map();   // 全局符号 → 类型 id
  const queue = [];
  for (const c of carriers ?? []) {
    if (!c.type || !c.reg) continue;
    const t = c.type.replace(/\*$/, '');
    const key = `${c.fn}|${c.reg}`;
    known.set(key, t);
    // ★ **只有 arg 载体**能决定"函数的根类型"；local 载体只说明那个寄存器，
    //   与 this 无关（踩过：local 被当根类型种子 ⇒ 把 T_5c73c_0 污染到 T0 方法上，报 5 处假冲突）
    if (c.kind === 'arg' && c.fn && c.reg === 'ecx') {
      if (!inferred.has(c.fn)) inferred.set(c.fn, { type: t, hops: 0, via: '(arg 载体)' });
      queue.push({ fn: c.fn, type: t, hops: 0 });
    }
  }
  let linksSeen = 0;
  const conflicts = [];
  // ★ **"声明 > 推断"**：被调方若已有显式载体（`kind:"arg"`），调用点上推出的另一类**不是冲突** ——
  //   它是"这个被调方不是调用方那一类的实例方法"，最典型的就是 **`this->成员` 的转发器**：
  //   实测 `sub_4034C0`/`sub_4034D0`/`sub_4034E0` 都是 `mov ecx,[ecx+4]; …`（其中 `sub_4034E0` 还是
  //   `mov eax,[ecx]; mov eax,[eax+8]; jmp eax` = **虚调用**），三条分别转发到 `this->sink` 的两个具体方法与一个 vtable 槽。
  //   ⇒ 记成"冲突"会让人以为 4 处声明错了；记成 note 才诚实：**是调用方类在该偏移上相容**（同一偏移、同一目标类型），
  //   被调方的真实类型（适配器 / 基类）由**声明**给出，推断无权推翻它。
  const declared = new Set((carriers ?? []).filter((c) => c.kind === 'arg' && c.fn).map((c) => c.fn));
  const declaredMismatch = [];
  // ★ **全局根锚定扫描**：根对象的那些调用方自己**没有**对象根约定（`_WinMain@16` 是 `__stdcall`，
  //   WndProc 也是），它们永远进不了队列 ⇒ 只靠"有类型的调用方"出发，全局这一整类调用点**扫不到**。
  //   这些种子带 `type: null`：`this`/`interior`/`member` 一律解不出（⛔ 不猜），只有 `global` 那一支能定类。
  for (const fn of extraCallers ?? []) {
    if (known.has(`${fn}|ecx`)) continue;
    queue.push({ fn, type: null, hops: 0 });
  }
  while (queue.length) {
    const cur = queue.shift();
    if (cur.hops >= maxHops) continue;
    const r = analyzeFn(cur.fn);
    if (!r) continue;
    for (const l of r.links ?? []) {
      if (l.kind !== 'call') continue;
      linksSeen += 1;
      const arg = l.args?.ecx;
      if (!arg) continue;
      let t = null;
      if (arg.k === 'this') t = cur.type;
      else if (arg.k === 'interior') {
        // k===0 ⇒ 对象本身；否则**只有**当 k 是已声明的**内嵌子对象**时才换类，否则不传播
        const off = arg.off ?? 0;
        if (!cur.type) t = null;                 // 全局扫描种子：自己的类都不知道 ⇒ 子对象更不知道
        else if (off === 0) t = cur.type;
        else {
          const self = types[cur.type];
          const m = (self?.members ?? []).find((x) => num(x.off) === off);
          t = (m && m.type && m.kind !== 'ptr' && m.count === 1) ? m.type : null;
        }
      } else if (arg.k === 'member') t = cur.type ? resolveMemberPath(types, cur.type, arg.path) : null;
      else if (arg.k === 'global') t = GLOB.get(arg.sym) ?? null;   // ★ 根对象的来源（见函数头注释）
      if (!t) continue;
      // ★ **被调方必须真的收 `this`**（它的根寄存器就是 ecx）。理由：`ecx` 在 `__cdecl` 函数里
      //   不是 this（是残留值）；把调用点上"ecx 装的是谁"记到它头上是**无意义的断言**，
      //   而副作用是**假冲突**（实测：不加这一条，`sub_40A160`(cdecl) 同时被推出 T_77960 与 T0）。
      if (acceptTarget && !acceptTarget(l.target)) continue;
      const key = `${l.target}|ecx`;
      const had = known.get(key);
      if (had && had !== t) {
        // 声明过的被调方：推断无权推翻声明 ⇒ note（不算冲突）；只在**都是推断**时才是冲突
        if (declared.has(l.target)) declaredMismatch.push({ target: l.target, from: cur.fn, ea: l.ea, declared: had, implied: t });
        else conflicts.push({ target: l.target, from: cur.fn, ea: l.ea, had, now: t });
        continue;
      }
      if (had) continue;
      known.set(key, t);
      if (!inferred.has(l.target)) {
        inferred.set(l.target, { type: t, hops: cur.hops + 1, via: `${cur.fn}@0x${l.ea.toString(16)} (${arg.k})` });
        queue.push({ fn: l.target, type: t, hops: cur.hops + 1 });
      }
    }
  }
  return { inferred, known, linksSeen, conflicts, declaredMismatch };
}

/** 批次候选：已被传播确定类型、尚无载体的函数（按体量/站点数由调用方再排） */
export function nextCandidates({ inferred, carriers }) {
  // ★ 只把 `arg` 载体算作"已声明"：`global` 载体没有 `fn`（它声明的是全局槽，不是某个函数的 this）
  const declared = new Set((carriers ?? []).filter((c) => c.kind === 'arg').map((c) => c.fn));
  return [...inferred].filter(([fn]) => !declared.has(fn)).map(([fn, v]) => ({ fn, ...v }));
}
