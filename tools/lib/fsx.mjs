/**
 * tools/lib/fsx.mjs — **纯文件系统/哈希工具**（不认识任何领域数据）
 *
 * 归这里的判据：函数签名里只有路径 / Buffer，**没有任何"素材 / 槽 / 语料"概念**。
 */
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';

/** 'file' | 'dir' | 'other' | 'missing'（跟随符号链接，因为外部素材本来就是链接） */
export function statKind(p) {
  try {
    const st = fs.statSync(p);
    if (st.isFile()) return 'file';
    if (st.isDirectory()) return 'dir';
    return 'other';
  } catch {
    return 'missing';
  }
}

/** 目录下的所有文件（递归；跳过 `.gitkeep` 这类占位） */
export function listFiles(dir) {
  const out = [];
  const rec = (d) => {
    for (const e of fs.readdirSync(d, { withFileTypes: true })) {
      if (e.name === '.gitkeep') continue;
      const p = path.join(d, e.name);
      if (e.isDirectory()) rec(p);
      else if (e.isFile()) out.push(p);
    }
  };
  rec(dir);
  return out;
}

export function sha256File(p) {
  return createHash('sha256').update(fs.readFileSync(p)).digest('hex');
}

export function sha256buf(buf) {
  return createHash('sha256').update(buf).digest('hex');
}

/** Windows 反斜杠 → `/`（清单里一律正斜杠） */
export const toPosix = (p) => p.split(path.sep).join('/');
