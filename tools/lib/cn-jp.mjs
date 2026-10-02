/**
 * tools/lib/cn-jp.mjs — **中文 ↔ BIN 文本**的机械映射（领域模型：认"文档里的字"这件事，不解析 argv、不打印）
 *
 * ## 它解决什么
 * 游戏走 Shift-JIS（cp932），**简体字多半编不进去**。旧仓脚本的真实规则（`scripts/lib/sjis-encode.js`）是：
 *
 * ```
 * canEncodeCp932(ch) ? ch : (dict[ch] ?? '　')      ← 顺序不能反
 * ```
 *
 * 即：**能编就原样**；编不进去就查"简 → 日文写法"占位字典（`data/translations/subs-cn-jp.json`），
 * 字典也缺就记一个问题并用**全角空格**兜底。
 *
 * ## 为什么 patch 里必须存中文（本仓的关键口径）
 * 同一个码位**既可能**来自"本来就编得进去的汉字"、**也可能**来自字典占位 ⇒ **两者在 BIN 里不可区分**。
 * ⇒ **BIN 无法反推回中文** ⇒ 中文才是真源，BIN 里的写法只是它在**构建时**的派生。
 * ⇒ 提取时只能靠字典**反查**（有损），所以本文件对每一步都做**往返自证**：
 *    `toBinForm(toCnForm(s)) === s` 不成立就**放弃那次反查**、保留原形式（并让调用方计数报出）。
 *
 * ## 边界
 * 只管"字符串常量"这一层；**不碰**指令名 / 操作数形态（格式层的事在 `packages/age-format`）。
 * 也不解释字典的语义（那是编码方案的一半，见 `data/translations/subs-cn-jp.md`）。
 */
import fs from 'node:fs';
import path from 'node:path';
import { createHash } from 'node:crypto';

import { canEncodeCp932 } from '../../packages/age-format/src/asm/codec.mjs';
import { REPO_ROOT } from './paths.mjs';

export const DEFAULT_SUBS = path.join(REPO_ROOT, 'data', 'translations', 'subs-cn-jp.json');

/** 字典也查不到时的兜底字符（**全角空格**，与旧仓一致） */
export const FALLBACK = '　';

/** 行里的字符串常量：与汇编器的 `RE_PARSE_ARGS` 同形（非贪婪、支持 `\x` 转义） */
const QUOTED = /"((?:[^"\\]|\\.)*)"/g;

export function loadDict(p = DEFAULT_SUBS) {
  return JSON.parse(fs.readFileSync(p, 'utf8'));
}

export function dictSha(p = DEFAULT_SUBS) {
  return createHash('sha256').update(fs.readFileSync(p)).digest('hex');
}

/**
 * 造一对映射器。
 * @param {Record<string,string>} dict 简 → 日文写法
 */
export function makeMapper(dict) {
  /** 日文写法 → 简（同一写法被多个简共用时**保留第一个** ⇒ 反查不是单射，故下面要自证） */
  const inverse = new Map();
  for (const [cn, jp] of Object.entries(dict)) if (!inverse.has(jp)) inverse.set(jp, cn);

  const stats = { mapped: 0, fallback: 0, unmappable: new Map(), revertedInverse: 0 };

  /** 中文（或任何文本）→ BIN 里能存的写法 */
  function toBin(str) {
    let out = '';
    for (const ch of str) {
      if (canEncodeCp932(ch)) { out += ch; continue; }
      const jp = dict[ch];
      if (jp === undefined) {
        stats.fallback += 1;
        stats.unmappable.set(ch, (stats.unmappable.get(ch) ?? 0) + 1);
        out += FALLBACK;
        continue;
      }
      stats.mapped += 1;
      out += jp;
    }
    return out;
  }

  /**
   * BIN 里的写法 → 中文（**只用于提取**；有损，靠往返自证兜底）。
   * 只在"反查回来的是编不进 cp932 的简"时才替换 —— 否则那个字符本来就该原样留在 BIN 里。
   */
  function toCn(str) {
    let out = '';
    for (const ch of str) {
      const cn = inverse.get(ch);
      out += cn !== undefined && !canEncodeCp932(cn) ? cn : ch;
    }
    if (out !== str && toBin(out) !== str) {
      stats.revertedInverse += 1;
      return str; // 反查不回来 ⇒ 不改（宁可少认一个中文，也不制造"看起来对但重建会变"的假象）
    }
    return out;
  }

  /** 只改一行里的**字符串常量**，其余字节原样（指令名 / 操作数形态不归这里管） */
  const inStrings = (fn) => (line) => line.replace(QUOTED, (_m, body) => `"${fn(body)}"`);

  return {
    toBin,
    toCn,
    lineToBin: inStrings(toBin),
    lineToCn: inStrings(toCn),
    stats,
    /** 一次构建下来还剩哪些编不出去的字（`--status` / 报告用） */
    unmappable: () => [...stats.unmappable.entries()].sort((a, b) => b[1] - a[1]),
  };
}
