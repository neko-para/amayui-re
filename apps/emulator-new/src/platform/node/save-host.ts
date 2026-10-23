/**
 * 平台层（Node）：`SaveHost` 的 `node:fs` 实现。
 *
 * ★ 槽号 → 文件名规则是**文件协议**（`SAVE<槽号>.DAT` / `.STH`，至少两位、左补 0）；
 *   目录前缀由调用方给（通常是存档目录）。
 * ★ 不读 `data/spec` / 不碰 spec；只是把 `core/save.ts` 的接口接到真实文件系统。
 */
import fs from 'node:fs/promises';
import path from 'node:path';

import type { SaveHost } from '../../core/save.ts';

export class NodeSaveHost implements SaveHost {
  readonly dir: string;

  constructor(dir: string) {
    this.dir = dir;
  }

  fileForSlot(slot: number, kind: 'dat' | 'sth'): string {
    const s = slot < 0 ? `-${String(-slot).padStart(2, '0')}` : String(slot).padStart(2, '0');
    return path.join(this.dir, `SAVE${s}.${kind.toUpperCase()}`);
  }

  async read(file: string): Promise<Uint8Array | null> {
    try {
      return await fs.readFile(file);
    } catch {
      return null;
    }
  }

  async write(file: string, bytes: Uint8Array): Promise<boolean> {
    try {
      await fs.mkdir(path.dirname(file), { recursive: true });
      await fs.writeFile(file, bytes);
      return true;
    } catch {
      return false;
    }
  }

  async remove(file: string): Promise<boolean> {
    try {
      await fs.rm(file, { force: false });
      return true;
    } catch {
      return false;
    }
  }

  async copy(from: string, to: string): Promise<boolean> {
    try {
      await fs.mkdir(path.dirname(to), { recursive: true });
      await fs.copyFile(from, to);
      return true;
    } catch {
      return false;
    }
  }

  async rename(from: string, to: string): Promise<boolean> {
    try {
      await fs.rename(from, to);
      return true;
    } catch {
      return false;
    }
  }
}
