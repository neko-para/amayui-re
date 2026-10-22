/**
 * 平台层（Node）：`Host` 的实现 —— 文件系统 / 路径 / 环境变量只出现在这里。
 *
 * ★ 这里是**唯一**知道 `node:fs` / `node:os` / `node:path` / `process.env` 的地方
 *   （核心的 `tsconfig.core.json` 里有 `types: []`：核心写一行 `node:fs` 就编译不过）。
 * ★ 返回的字节一律是**纯 `Uint8Array`**（`Buffer` 是它的子类；这里显式取视图，
 *   免得核心在 Node 里靠 `Buffer` 的方法侥幸跑通、到浏览器里才炸）。
 * ★ `readBytesMaybe` 只把 **ENOENT** 当成"没有"；其它 IO 错误照抛（⛔ 不静默吞）。
 */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import type { Host } from '../../core/install.ts';

/** `Buffer`（Node 的 `Uint8Array` 子类）→ 纯 `Uint8Array` 视图（零拷贝） */
function asBytes(view: Uint8Array): Uint8Array {
  return new Uint8Array(view.buffer, view.byteOffset, view.byteLength);
}

function codeOf(err: unknown): string | undefined {
  return (err as NodeJS.ErrnoException | null)?.code;
}

/** 只读路径的建目录：沙箱 / 只读盘 / 权限不足都不算错误（读不到 ⇒ 用发行默认） */
const READ_DIR_ERRNOS = new Set(['EPERM', 'EACCES', 'EROFS', 'ENOENT']);

export const nodeHost: Host = {
  join(...parts: string[]): string {
    return path.join(...parts);
  },

  dirname(file: string): string {
    return path.dirname(file);
  },

  async readBytes(file: string): Promise<Uint8Array> {
    return asBytes(await fs.promises.readFile(file));
  },

  async readBytesMaybe(file: string): Promise<Uint8Array | null> {
    try {
      return asBytes(await fs.promises.readFile(file));
    } catch (err) {
      if (codeOf(err) === 'ENOENT') return null;
      throw err;
    }
  },

  async readSlice(file: string, offset: number, length: number): Promise<Uint8Array> {
    const handle = await fs.promises.open(file, 'r');
    try {
      const out = new Uint8Array(length);
      let done = 0;
      while (done < length) {
        const { bytesRead } = await handle.read(out, done, length - done, offset + done);
        if (bytesRead <= 0) throw new Error(`read-slice-short:${file} need=${length} got=${done}`);
        done += bytesRead;
      }
      return out;
    } finally {
      await handle.close();
    }
  },

  async readText(file: string): Promise<string> {
    return fs.promises.readFile(file, 'utf8');
  },

  async isFile(file: string): Promise<boolean> {
    try {
      return (await fs.promises.stat(file)).isFile();
    } catch (err) {
      const code = codeOf(err);
      if (code === 'ENOENT' || code === 'ENOTDIR') return false;
      throw err;
    }
  },

  async ensureReadDir(dir: string): Promise<void> {
    try {
      await fs.promises.mkdir(dir, { recursive: true });
    } catch (err) {
      if (READ_DIR_ERRNOS.has(codeOf(err) ?? '')) return;
      throw err;
    }
  },

  async writeBytes(file: string, data: Uint8Array): Promise<void> {
    // 写路径**不**容忍：建不出目录 / 写不进去都必须响亮失败
    await fs.promises.mkdir(path.dirname(file), { recursive: true });
    await fs.promises.writeFile(file, data);
  },

  env(name: string): string | null {
    return process.env[name] ?? null;
  },

  homeDir(): string {
    return os.homedir();
  },
};
