/**
 * plugins/pnpm-priv/lib/probe.mjs —— **宿主侧探针**：验证特权与链接形态
 *
 * 两件事，都是"只读或自清"的：
 * 1. `canSymlink()`：**本插件进程能否建真目录符号链接** —— 在系统临时目录里建一个、验完**立刻删**。
 *    ★ 为什么必须在**临时目录**里试：`node_modules` 里试会污染工作树；而 Windows 的
 *    `SeCreateSymbolicLinkPrivilege` 是**进程级**的 ⇒ 临时目录的结果对 `node_modules` 同样成立。
 * 2. `inspectWorkspaceLinks()`：看每个 workspace 消费者的 `node_modules/@amayui/*` 是
 *    **symbolic link** 还是 **junction**（后者 ⇒ Node 不能对 `.mts` 剥类型）。
 *
 * ★ 这里用 `lstatSync` 而不是 `statSync`：junction 在 `statSync` 下看着像目录，
 *   只有 `lstatSync().isSymbolicLink()` 能区分（本仓踩过：PowerShell 的 `Get-ChildItem -Directory`
 *   既不跟随也不显示 junction，用它数会得出全 0 的假象）。
 */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

/** 这个路径是"真符号链接"吗（junction ⇒ false） */
export function linkKind(p) {
  let st;
  try {
    st = fs.lstatSync(p);
  } catch (err) {
    return { exists: false, kind: 'missing', why: err.code ?? err.message };
  }
  let real = null;
  try {
    real = fs.realpathSync(p);
  } catch (err) {
    real = `（realpath 失败：${err.code ?? err.message}）`;
  }
  if (st.isSymbolicLink()) return { exists: true, kind: 'symlink', real, stripped: !/[\\/]node_modules[\\/]/.test(real) };
  if (st.isDirectory()) return { exists: true, kind: 'junction-or-dir', real, stripped: false };
  return { exists: true, kind: 'other', real, stripped: false };
}

/**
 * 本进程能否建**真**目录符号链接（探针自清）。
 * @returns {{ok:boolean, why:string, code?:string}}
 */
export function canSymlink() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'pnpm-priv-probe-'));
  const target = path.join(dir, 'target');
  const link = path.join(dir, 'link');
  try {
    fs.mkdirSync(target);
    fs.symlinkSync(target, link, 'dir');
    const st = fs.lstatSync(link);
    if (!st.isSymbolicLink()) {
      return { ok: false, why: '`symlinkSync(..., "dir")` 没报错，但建出来的是 **junction 而不是符号链接** ⇒ 特权不成立（降级了）', code: 'DOWNGRADED' };
    }
    return { ok: true, why: '能建真目录符号链接 ⇒ 宿主特权成立，`pnpm install` 会建真 symlink' };
  } catch (err) {
    return { ok: false, why: `建符号链接失败：${err.code ?? ''} ${err.message}`, code: err.code ?? null };
  } finally {
    // ★ 自清：无论成败都删掉探针目录
    try { fs.rmSync(dir, { recursive: true, force: true }); } catch { /* 自清失败不影响判据 */ }
  }
}

/** 递归找出所有 `node_modules/@amayui` 目录（从 repoRoot 下两层起） */
export function findAmayuiLinks(repoRoot, extraDirs = []) {
  const found = [];
  const roots = [repoRoot, ...extraDirs.map((d) => path.resolve(repoRoot, d))];
  const scan = (base, depth) => {
    if (depth > 4) return;
    let entries;
    try { entries = fs.readdirSync(base, { withFileTypes: true }); } catch { return; }
    for (const e of entries) {
      if (!e.isDirectory() && !e.isSymbolicLink()) continue;
      const p = path.join(base, e.name);
      if (e.name === 'node_modules') {
        const am = path.join(p, '@amayui');
        if (fs.existsSync(am)) {
          for (const n of fs.readdirSync(am)) found.push({ consumer: path.relative(repoRoot, p) || '.', name: n, path: path.join(am, n), ...linkKind(path.join(am, n)) });
        }
        continue;
      }
      if (e.name === '.git' || e.name === '.pnpm-store' || e.name === '.tmp') continue;
      scan(p, depth + 1);
    }
  };
  for (const r of roots) scan(r, 0);
  return found;
}

/** `pnpm store path` 落在哪（用 `node` 直接跑 pnpm 的 CLI 有版本差异，这里用 `pnpm` 可执行） */
export function storePathHint(repoRoot) {
  const candidates = [path.join(repoRoot, '.pnpm-store'), path.join(repoRoot, 'node_modules', '.pnpm')];
  return candidates.filter((p) => fs.existsSync(p));
}
