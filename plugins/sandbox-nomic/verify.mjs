#!/usr/bin/env node
/**
 * plugins/sandbox-nomic/verify.mjs —— route A 的端到端边界验收（**只在 Windows 上跑**）。
 *
 * 它做三件事，全部只动 `os.tmpdir()` 下的一棵一次性小树：
 *   1. 用 `--report-integrity` 读**实际创建的令牌**的完整性级别（应为 Medium）；
 *   2. 在 workspace-write 下跑 `test/probe-child.mjs`，逐条记录
 *      写工作区 / 写工作区外 / 写 Everyone 可写目录 / 管道 spawn；
 *   3. read-only 下重跑同一探针（工作区写必须被拒），并验退出码透传。
 *
 * 用法：`node plugins/sandbox-nomic/verify.mjs`（或 `pnpm --filter @amayui/dsh-plugin-sandbox-nomic verify`）。
 * 退出码 0 = 全部符合预期；1 = 有断言失败。
 *
 * ★ 它**不会**碰真实工作区：workspace / outside / everyone 都是 `mkdtemp` 出来的；
 *   `icacls` 只改这棵树；结束（含失败）时整棵删掉。
 */
import { execFileSync, spawnSync } from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const RUNNER = fileURLToPath(new URL('./lib/runner.mjs', import.meta.url))
const PROBE = fileURLToPath(new URL('./test/probe-child.mjs', import.meta.url))
const NODE = process.execPath
const TIMEOUT_MS = 180_000

const results = []

/** 记录一条断言并即时打印。 */
function check(name, pass, detail = '') {
  results.push({ name, pass })
  console.log(`${pass ? 'PASS' : 'FAIL'}  ${name}${detail === '' ? '' : `  — ${detail}`}`)
}

/** 跑一次 runner，返回 spawnSync 结果（stdout/stderr 已转字符串）。 */
function runRunner(args, options = {}) {
  return spawnSync(NODE, [RUNNER, ...args], {
    encoding: 'utf8',
    timeout: TIMEOUT_MS,
    windowsHide: true,
    ...options,
  })
}

/** icacls 的 argv 形式（数组、不经 shell）。 */
function icacls(args) {
  execFileSync('icacls', args, { stdio: 'ignore', windowsHide: true })
}

/** 把一次探针的 stdout 解析成对象；解析不了就返回 null。 */
function parseProbe(run) {
  try {
    return JSON.parse(run.stdout.trim().split(/\r?\n/).at(-1))
  } catch {
    return null
  }
}

if (process.platform !== 'win32') {
  console.log('SKIP  verify.mjs only runs on Windows')
  process.exit(0)
}

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'dsh-nomic-poc-'))
const ws = path.join(root, 'ws')
const outside = path.join(root, 'outside')
const everyone = path.join(root, 'everyone')

try {
  for (const dir of [ws, outside, everyone]) fs.mkdirSync(dir)

  // 受限令牌的第二次访问检查只认限制 SID 列表（logon / Everyone / 能力 SID）。
  // 让临时树对 Everyone 可读+可穿越，探针才走得到 everyone 子目录。
  icacls([root, '/grant', '*S-1-1-0:(OI)(CI)(RX)'])
  // 一个「Everyone 可写」目录 = 本变体（以及 Codex unelevated / srt）公开接受的缺口。
  icacls([everyone, '/grant', '*S-1-1-0:(OI)(CI)(F)'])

  // ① 令牌完整性：no-MIC 的全部意义就在这一条。
  const integrity = runRunner(['--workspace', ws, '--mode', 'workspace-write', '--report-integrity'])
  const level = integrity.stdout.trim()
  check(
    'token integrity is Medium (S-1-16-8192), not Low (S-1-16-4096)',
    integrity.status === 0 && level === 'S-1-16-8192',
    `status=${integrity.status} level=${JSON.stringify(level)}${integrity.stderr ? ` stderr=${JSON.stringify(integrity.stderr)}` : ''}`,
  )

  // ② workspace-write 的写边界。
  const wsFile = path.join(ws, 'workspace.txt')
  const outsideFile = path.join(outside, 'outside.txt')
  const everyoneFile = path.join(everyone, 'everyone.txt')
  const probe = runRunner(
    ['--workspace', ws, '--mode', 'workspace-write', '--', NODE, PROBE, wsFile, outsideFile, everyoneFile],
    { cwd: ws },
  )
  const facts = parseProbe(probe)
  if (facts === null) {
    check('probe produced JSON', false, `status=${probe.status} stdout=${JSON.stringify(probe.stdout)} stderr=${JSON.stringify(probe.stderr)}`)
  } else {
    check('probe exited 0', probe.status === 0, `status=${probe.status}`)
    check('write inside workspace is allowed', facts.workspace === 'ok', String(facts.workspace))
    check('write outside workspace is denied (EPERM/EACCES)', /^denied:(EPERM|EACCES)$/.test(String(facts.outside)), String(facts.outside))
    check(
      'write to an Everyone-writable dir is ALLOWED (documented residual)',
      facts.everyone === 'ok',
      String(facts.everyone),
    )
    check(
      'piped-stdio grandchild still fails (restricted token, NOT fixed by dropping MIC)',
      String(facts.pipeSpawn).startsWith('error:') || String(facts.pipeSpawn).startsWith('throw:'),
      String(facts.pipeSpawn),
    )
    check('workspace file really exists', fs.existsSync(wsFile))
    check('outside file really absent', !fs.existsSync(outsideFile))
    check('echo through cmd redirection works', (() => {
      const echoFile = path.join(ws, 'echo.txt')
      const echo = runRunner(['--workspace', ws, '--mode', 'workspace-write', '--', 'cmd', '/c', `echo hi>${echoFile}`], { cwd: ws })
      return echo.status === 0 && fs.existsSync(echoFile) && fs.readFileSync(echoFile, 'utf8').includes('hi')
    })())
  }

  // ③ read-only：同一个探针，工作区写必须被拒。
  const ro = runRunner(
    ['--workspace', ws, '--mode', 'read-only', '--', NODE, PROBE, path.join(ws, 'ro-workspace.txt'), path.join(outside, 'ro-outside.txt'), path.join(everyone, 'ro-everyone.txt')],
    { cwd: ws },
  )
  const roFacts = parseProbe(ro)
  if (roFacts === null) {
    check('read-only probe produced JSON', false, `status=${ro.status} stdout=${JSON.stringify(ro.stdout)} stderr=${JSON.stringify(ro.stderr)}`)
  } else {
    check('read-only denies a workspace write (EPERM/EACCES)', /^denied:(EPERM|EACCES)$/.test(String(roFacts.workspace)), String(roFacts.workspace))
  }

  // ④ 退出码透传（runner 必须镜像子进程的退出码，而不是自己的 127）。
  const exitTest = runRunner(['--workspace', ws, '--mode', 'workspace-write', '--', 'cmd', '/c', 'exit', '7'])
  check('exit code is mirrored', exitTest.status === 7, `status=${exitTest.status}`)

  // ⑤ 失败的 runner 必须打签名（seam 靠它区分「sandbox 坏了」与「命令被拒」）。
  const broken = runRunner(['--workspace', path.join(root, 'does-not-exist'), '--mode', 'workspace-write', '--', 'cmd', '/c', 'exit', '0'])
  check(
    'runner failure prints nomiv-run: and exits 127',
    broken.status === 127 && broken.stderr.includes('nomiv-run: '),
    `status=${broken.status} stderr=${JSON.stringify(broken.stderr)}`,
  )

  // ⑥ 目标程序起不来必须是 126，不能是 127 —— 否则执行器把「命令打错」翻成 SANDBOX_UNAVAILABLE。
  const typo = runRunner(['--workspace', ws, '--mode', 'workspace-write', '--', 'nosuch-command-xyz'])
  check(
    'a missing target program exits 126, not 127',
    typo.status === 126 && typo.stderr.includes('nomiv-run: '),
    `status=${typo.status} stderr=${JSON.stringify(typo.stderr)}`,
  )
} finally {
  try {
    fs.rmSync(root, { recursive: true, force: true, maxRetries: 3 })
  } catch (error) {
    console.log(`WARN  could not remove ${root}: ${error.message}`)
  }
}

const failed = results.filter((r) => !r.pass)
console.log(`\n${results.length - failed.length}/${results.length} checks passed`)
if (failed.length > 0) {
  console.log(`failed: ${failed.map((r) => r.name).join(' | ')}`)
  process.exitCode = 1
}
