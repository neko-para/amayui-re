/**
 * tools/lib/bin-source.mjs — **BIN 来源解析层**（领域模型：认"游戏安装根"这件事，但不解析 argv、不打印）
 *
 * 一个"游戏根目录"里，同一个名字的 BIN 可能出现在两处，**优先级是引擎自己的**：
 *
 * ```
 * ① 散装 BIN（根目录下直接躺着的 *.BIN）        ← 优先
 * ② 否则进 ALF 归档（索引 → *.ALF 数据体）      ← 其次
 * ```
 *
 * ★ 为什么散装优先：散装件本来就是**开发商给本体打的 patch**（覆盖归档里的旧版本）。
 *   实测同一个文件名在两处的字节**可以不同**（`SC0000.BIN` / `SCINIT.BIN` 都是），
 *   所以"先散装后归档"不是性能优化，而是**语义**。
 *
 * ★ 为什么不硬编码 `SYS4INI.BIN` / `APPEND0N.AAI`：按**签名**发现索引（`LAYOUTS` 的 sigs）——
 *   数据片的数量与名字是外部事实，写死一份清单迟早漂。认不出的当普通 BIN（或直接忽略）。
 *
 * ★ 为什么不引用旧仓 `raw-parts/`：它只是 ALF 的**解包派生**（实测它的字节既不等于散装基线、
 *   也不等于产物），把它当依赖等于给同一份信息再加一处副本。ALF 读取用 `packages/age-format`（M2 已交付）。
 *
 * ★ 取条目按**实际读到的字节数**：索引声明的 `length` 可能比实际长
 *   （实测 `PLINIT` 声明 52496、实际 52492）—— 按声明长度 `readSync` 会把短读当成满读。
 */
import fs from 'node:fs';
import path from 'node:path';

import { LAYOUTS, indexEntries, readAlf } from '@amayui/age-format/src/alf.mts';
import { readHeader } from '@amayui/age-format/src/asm/index.mts';

/** 可能承载 ALF 索引的扩展名（大写比较） */
const INDEX_EXT = new Set(['.AAI', '.BIN']);
/** 可能承载散装 BIN 的扩展名 */
const BIN_EXT = new Set(['.BIN']);

/** 读文件的一段（不整读大文件）；返回实际读到的字节 */
function readSlice(abs, offset, length) {
  const fd = fs.openSync(abs, 'r');
  try {
    const buf = Buffer.alloc(length);
    let read = 0;
    while (read < length) {
      const n = fs.readSync(fd, buf, read, length - read, offset + read);
      if (n <= 0) break;
      read += n;
    }
    return buf.subarray(0, read);
  } finally {
    fs.closeSync(fd);
  }
}

/** 这个文件是不是 ALF 索引？（只看头部签名，不整读） */
function indexSignatureOf(abs) {
  const head = readSlice(abs, 0, 16);
  for (const [name, l] of Object.entries(LAYOUTS)) {
    for (const sig of l.sigs) {
      const want = l.unicode ? Buffer.from(sig, 'utf16le') : Buffer.from(sig, 'latin1');
      if (head.length >= want.length && head.subarray(0, want.length).equals(want)) {
        return { layout: name, signature: sig };
      }
    }
  }
  return null;
}

/**
 * 打开一个游戏根目录。
 * @param {string} dir 绝对或相对路径
 * @returns {{dir:string, loose:Map<string,string>, indices:Array<object>,
 *            resolve:(name:string)=>({buf:Buffer, from:string}|null), names:()=>Set<string>}}
 */
export function openRoot(dir) {
  if (!fs.existsSync(dir)) throw new Error(`游戏根目录不存在：${dir}`);
  const absent = [];
  const loose = new Map();
  const indices = [];

  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    if (!e.isFile()) continue;
    const ext = path.extname(e.name).toUpperCase();
    if (!INDEX_EXT.has(ext)) continue;
    const abs = path.join(dir, e.name);
    if (indexSignatureOf(abs)) {
      try {
        const alf = readAlf(abs);
        const { map, duplicates } = indexEntries(alf);
        indices.push({ name: e.name, abs, alf, map, duplicates });
        continue; // 索引文件不是脚本
      } catch (err) {
        absent.push({ name: e.name, why: `签名像索引但读不出来：${err.message}` });
      }
    }
    if (BIN_EXT.has(ext)) loose.set(e.name.toUpperCase(), abs);
  }

  /** @returns {{buf:Buffer, from:string}|null} */
  function resolve(name) {
    const key = String(name).toUpperCase();
    if (loose.has(key)) return { buf: fs.readFileSync(loose.get(key)), from: `loose:${name}` };
    for (const ix of indices) {
      const entry = ix.map.get(name);
      if (!entry) continue;
      const arc = ix.alf.archives[entry.archiveIndex];
      if (!arc) continue;
      const arcAbs = path.join(dir, arc.filename);
      const buf = readSlice(arcAbs, entry.offset, entry.length);
      return {
        buf,
        from: `alf:${ix.name}→${arc.filename}@${entry.offset}+${buf.length}`,
        declaredLength: entry.length,
      };
    }
    return null;
  }

  function names() {
    const out = new Set(loose.keys());
    for (const ix of indices) for (const n of ix.map.keys()) out.add(n.toUpperCase());
    return out;
  }

  return { dir, loose, indices, resolve, names, unreadable: absent };
}

/** 名字以 `.BIN` 结尾（大小写不敏感） */
export const isBinName = (name) => /\.bin$/i.test(name);

/**
 * 这份字节是不是 AGE 脚本？（`readHeader` 认不出签名就抛 ⇒ 这里转成布尔）
 *
 * ★ 用格式层的 `readHeader` 而不是在这里抄 `SYS4` / `SYS5` 两个魔数：
 *   那是**格式事实**，抄一份就等于给同一件事造了第二个真源。
 * ★ 实测基线根 942 个 `.BIN` 里只有 `AGE.EXE__USERDATA.BIN` 落选。
 */
export function isAgeScript(buf) {
  try {
    readHeader(buf);
    return true;
  } catch {
    return false;
  }
}
