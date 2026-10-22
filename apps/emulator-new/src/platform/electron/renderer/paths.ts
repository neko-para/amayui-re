/**
 * renderer 侧的**最小路径实现**（`Host.join` / `Host.dirname` / `Host.pathFromEngine` 用）。
 *
 * ★ core 的 `Host` 把 `join` / `dirname` / `pathFromEngine` 定为同步接口，而 renderer 与 main
 *   之间的 IPC 是异步的 ⇒ 不能让每次路径拼接都跨进程。这里按 main 报告的 `sep` 做同一套字符串
 *   运算；真正碰文件系统的 `read*` / `isFile` / `write*` 仍然走 `bridge.invoke`。
 * ★ 只实现 core 实际用到的 Node path 子集（`join` / `dirname`）；⛔ 不假装是完整 path 模块。
 * ★ `pathFromEngine` 的输入是 CP932 字节串：先解码，再把 `\` 换成宿主分隔符
 *   （POSIX 的 `join` 不认 `\`，而 Shift-JIS 的 trail byte 可能是 0x5C ⇒ 必须先解码再替换）。
 */
import { decodeCp932 } from '../../../core/bytes.ts';

function isWin(sep: string): boolean {
  return sep === '\\';
}

function splitDrive(file: string, win: boolean): { drive: string; rest: string } {
  if (win && /^[A-Za-z]:/.test(file)) return { drive: file.slice(0, 2), rest: file.slice(2) };
  return { drive: '', rest: file };
}

/** Node `path.join` 的常用子集：逐段拼接后消掉 `.` / `..`；保留绝对路径前缀 */
export function joinPath(sep: string, ...parts: string[]): string {
  const win = isWin(sep);
  const all = parts.filter((part) => part.length > 0).join(sep);
  if (all.length === 0) return '.';
  const { drive, rest } = splitDrive(all, win);
  const absolute = rest.startsWith(sep) || (win && rest.startsWith('/'));
  const segments = rest.split(win ? /[\\/]+/ : /\/+/);
  const out: string[] = [];
  for (const segment of segments) {
    if (segment === '' || segment === '.') continue;
    if (segment === '..') {
      if (out.length > 0 && out[out.length - 1] !== '..') out.pop();
      else if (!absolute) out.push('..');
      continue;
    }
    out.push(segment);
  }
  let result = out.join(sep);
  if (drive !== '') result = `${drive}${absolute ? sep : ''}${result}`;
  else if (absolute) result = `${sep}${result}`;
  return result.length === 0 ? (drive !== '' ? `${drive}${sep}` : '.') : result;
}

/** Node `path.dirname` 的常用子集 */
export function dirnamePath(sep: string, file: string): string {
  const win = isWin(sep);
  const { drive, rest } = splitDrive(file, win);
  const normalized = rest.replace(win ? /\\/g : /\//g, sep);
  const at = normalized.lastIndexOf(sep);
  if (at < 0) return drive === '' ? '.' : `${drive}${sep}`;
  if (at === 0) return `${drive}${sep}`;
  const head = normalized.slice(0, at);
  return `${drive}${head}`;
}

export function pathFromEngine(sep: string, text: string): string {
  return decodeCp932(text).replace(/\\/g, sep);
}
