/** @env external @kind contract @suite toolchain @why the provider must own one private temp per live session/workspace pair, reuse it across commands, isolate sessions, and remove it on dispose */
import test from 'node:test'
import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { setTimeout as sleep } from 'node:timers/promises'

const WINDOWS = process.platform === 'win32'

/** 从 confined argv 里取一个 flag 的值。 */
function flagValue(argv, flag) {
  const index = argv.indexOf(flag)
  return index === -1 ? undefined : argv[index + 1]
}

test('per-session temp: reused within a session, distinct across sessions, removed on dispose', async (t) => {
  if (!WINDOWS) return t.skip('win32 only')

  // 依赖可能没装（peer/fresh checkout）：不能因为环境缺失把 external 档跑红。
  let Context
  let Plugin
  try {
    ;({ Context } = await import('@deepseek-ai/cordis'))
    ;({ default: Plugin } = await import('../index.mjs'))
  } catch (error) {
    return t.skip(`plugin dependencies not resolvable: ${error.message}`)
  }

  const ctx = new Context()
  const ws = fs.mkdtempSync(path.join(os.tmpdir(), 'dsh-nomiv-ws-'))
  const temps = []
  let failure
  try {
    await ctx.plugin(Plugin)
    await sleep(50)
    assert.equal(typeof ctx.sandbox?.confine, 'function', 'provider must register ctx.sandbox')

    const confine = async (sessionId, mode = 'workspace-write') =>
      ctx.sandbox.confine(['cmd', '/c', 'exit', '0'], { mode, workspaceRoot: ws, sessionId })

    const first = await confine('s1')
    const second = await confine('s1')
    const other = await confine('s2')
    const readOnly = await confine('s1', 'read-only')

    const tempOf = (wrapped) => flagValue(wrapped.argv, '--temp')
    temps.push(tempOf(first), tempOf(second), tempOf(other))
    assert.equal(tempOf(first), tempOf(second), 'the same session must reuse its private temp')
    assert.notEqual(tempOf(first), tempOf(other), 'different sessions must not share a temp')
    assert.ok(fs.existsSync(tempOf(first)), 'the private temp must exist')
    assert.equal(flagValue(first.argv, '--temp-write-sid')?.startsWith('S-1-4-'), true)
    assert.equal(flagValue(first.argv, '--write-sid')?.startsWith('S-1-4-'), true)
    assert.equal(tempOf(readOnly), undefined, 'read-only must not carry a temp')

    // seam 传下去的 SID 必须被 runner 接受：真跑一次（`cmd /c exit 0`）。
    const run = spawnSync(first.argv[0], first.argv.slice(1), { encoding: 'utf8', timeout: 120_000 })
    assert.equal(run.status, 0, `confined argv must run: status=${run.status} stderr=${run.stderr}`)
  } catch (error) {
    failure = error
  } finally {
    await ctx.fiber.dispose()
    await sleep(50)
  }

  // dispose 的断言放在 fiber 关掉之后（失败时也要先清理再报）。
  try {
    for (const dir of temps) assert.equal(fs.existsSync(dir), false, `temp not removed on dispose: ${dir}`)
  } finally {
    fs.rmSync(ws, { recursive: true, force: true })
  }
  if (failure) throw failure
})
