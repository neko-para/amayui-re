/**
 * plugins/deploy/lib/probe.mjs —— **特权探针**：三步之内证明"这个进程真的不受 ACL 沙箱约束"
 *
 * 为什么需要探针：本插件存在的理由是"宿主进程不受沙箱约束"，而**这一点必须能被证伪**。
 * 三个 op 各证一角，且都不留痕迹（探针件在 `finally` 里删掉）：
 *
 * | op | 证什么 | 沙箱里会怎样 |
 * |---|---|---|
 * | `status` | 宿主起的**子进程**是什么完整性；宿主能不能开管道 | 受限子进程是 Low；受限沙箱里 `stdio:'pipe'` ⇒ EPERM |
 * | `probe-link` | 能不能对**游戏安装目录**建硬链接（会改源文件的链接数 ⇒ 不是纯读操作） | EPERM（实测过） |
 * | `probe-write` | 能不能在**工作区外**写一个临时件 | 受限沙箱只允许写工作区与私有临时区 ⇒ 被拒 |
 *
 * ★ 这里**故意**用 `stdio: 'pipe'` 抓 `whoami` 的输出：受限沙箱下这条路是走不通的
 *   （`AGENTS.md` §5 第①条），所以"管道通"本身就是证据之一。
 */
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

/** 读仓库的**唯一**一处根路径真源：`corpus/assets.json` 的 `roots.gameInstall` */
export function gameInstallRoot(repoRoot) {
  const file = path.join(repoRoot, 'corpus', 'assets.json');
  if (!fs.existsSync(file)) throw new Error(`找不到 ${file}（游戏安装根的唯一真源）`);
  const roots = JSON.parse(fs.readFileSync(file, 'utf8')).roots ?? {};
  const dir = roots.gameInstall;
  if (typeof dir !== 'string' || dir === '') throw new Error('corpus/assets.json 的 roots.gameInstall 没配');
  if (!fs.existsSync(dir)) throw new Error(`游戏安装根不在：${dir}`);
  return dir;
}

/** 宿主起的子进程的完整性级别（`whoami /groups` 的那一行；管道读得到 ⇒ 已经说明"不在受限沙箱里"） */
function childIntegrity() {
  try {
    const out = execFileSync('whoami', ['/groups'], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'], windowsHide: true });
    // ★ `whoami` 按 OEM 码页输出，中文那截读成 utf8 会是乱码 ⇒ 只留能确定含义的 ASCII 部分
    const raw = (out.match(/Mandatory Label\\[^\r\n]*/) ?? [''])[0];
    const line = raw.replace(/[^\x20-\x7e]+/g, ' ').replace(/\s+/g, ' ').trim();
    return { ok: true, line: line || '(没找到 Mandatory Label 行)', raw: out.length };
  } catch (err) {
    return { ok: false, line: '', error: `${err.code ?? ''} ${err.message}`.trim() };
  }
}

/**
 * 本插件的 DSH 依赖解析到哪一份（应是**普通 dependencies** 装出来的那一份；
 * 与正在运行的 DSH 同版本是关键 —— 见 `../README.md` §5）
 */
function depsSource() {
  try {
    return import.meta.resolve('@deepseek-ai/dsh-tools');
  } catch (err) {
    return `(解析不了：${err.code ?? ''} ${err.message})`.trim();
  }
}

/** `status`：只读，报告事实 */
export function probeStatus({ repoRoot, pluginDir }) {
  const integ = childIntegrity();
  let game = null;
  try {
    game = gameInstallRoot(repoRoot);
  } catch (err) {
    game = `(读不到：${err.message})`;
  }
  const facts = {
    node: process.version,
    execPath: process.execPath,
    platform: process.platform,
    cwd: process.cwd(),
    pluginDir,
    dshTools: depsSource(),
    repoRoot,
    gameInstall: game,
    childIntegrity: integ.ok ? integ.line : `whoami 跑不起来：${integ.error}`,
  };
  const sandboxed = /Low Mandatory Level/i.test(facts.childIntegrity);
  return {
    ok: integ.ok,
    summary:
      `宿主起的子进程完整性 = ${facts.childIntegrity}\n` +
      `　⇒ ${sandboxed ? '**Low**（这条路仍然受限，说明本插件没起作用）' : '**不是 Low** ⇒ 宿主侧代码不受 ACL 沙箱约束（插件路线的立足点成立）'}\n` +
      `管道读取：${integ.ok ? '可用（受限沙箱下会 EPERM）' : '不可用'}\n` +
      `node ${facts.node} · @deepseek-ai/dsh-tools 解析到 ${facts.dshTools}\n` +
      `插件 ${pluginDir}\n游戏安装根 ${facts.gameInstall}`,
    details: facts,
  };
}

/** `probe-link`：对游戏安装目录建硬链接、核对 inode、**立刻删掉** */
export function probeLink({ repoRoot, dir }) {
  const game = gameInstallRoot(repoRoot);
  const srcRoot = path.parse(game).root;
  const dstRoot = path.parse(dir).root;
  if (srcRoot.toLowerCase() !== dstRoot.toLowerCase()) {
    throw new Error(`硬链接要求同卷：游戏安装根在 ${srcRoot}、落点在 ${dstRoot} ⇒ 用 dir 指到与游戏同一个盘上`);
  }
  const alfs = fs.readdirSync(game).filter((n) => /\.ALF$/i.test(n)).sort();
  if (!alfs.length) throw new Error(`游戏安装根里没有 *.ALF：${game}`);
  const src = path.join(game, alfs[0]);
  const dst = path.join(dir, `${alfs[0]}.deploy-probe`);
  const t0 = Date.now();
  try {
    fs.rmSync(dst, { force: true });
    fs.linkSync(src, dst);
    const a = fs.statSync(src);
    const b = fs.statSync(dst);
    const sameInode = Boolean(a.ino) && a.ino === b.ino && a.dev === b.dev;
    return {
      ok: sameInode,
      summary:
        `硬链接建成：${alfs[0]}（${(a.size / 1048576).toFixed(0)} MB 的逻辑大小，硬链接只花元数据）\n` +
        `　src ${src}\n　dst ${dst}\n　ino ${a.ino} vs ${b.ino} ⇒ ${sameInode ? '一致' : '**不一致（有东西不对）**'} · nlink=${b.nlink} · ${Date.now() - t0} ms\n` +
        '　⇒ 这一步在受限沙箱里是 EPERM（建硬链接会改源文件的链接数）',
      details: { src, dst, size: a.size, ino: [a.ino, b.ino], nlink: b.nlink, ms: Date.now() - t0 },
    };
  } finally {
    fs.rmSync(dst, { force: true }); // 探针不留痕
  }
}

/** `probe-write`：在工作区外写一个临时件、回读、**立刻删掉** */
export function probeWrite({ dir }) {
  const file = path.join(dir, `.dsh-deploy-probe-${process.pid}.tmp`);
  const payload = `probe ${new Date().toISOString()} pid=${process.pid}\n`;
  const t0 = Date.now();
  try {
    fs.writeFileSync(file, payload, 'utf8');
    const back = fs.readFileSync(file, 'utf8');
    return {
      ok: back === payload,
      summary:
        `写入成功：${file}（${payload.length} B，回读${back === payload ? '一致' : '**不一致**'}）· ${Date.now() - t0} ms\n` +
        `　⇒ 受限沙箱里往工作区外写会被拒（ACL 只授工作区与私有临时区）`,
      details: { file, bytes: payload.length, ms: Date.now() - t0 },
    };
  } finally {
    fs.rmSync(file, { force: true });
  }
}
