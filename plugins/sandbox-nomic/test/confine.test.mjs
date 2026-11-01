/** @env pure @kind contract @why the no-MIC provider must wrap the exact argv, keep one failure-signature literal, report partial enforcement, and refuse unconfined modes */
import test from 'node:test'
import assert from 'node:assert/strict'
import { existsSync } from 'node:fs'
import { dirname } from 'node:path'
import { fileURLToPath } from 'node:url'
import { buildConfinedArgv, DEFAULT_RUNNER_PATH, DENIAL_SIGNATURES, RUNNER_FAILURE_SIGNATURE } from '../lib/confine.mjs'

const POLICY = { mode: 'workspace-write', workspaceRoot: 'E:\\work' }
const OPTS = { execPath: 'node.exe', runnerPath: 'runner.mjs' }
const HERE = dirname(fileURLToPath(new URL('../lib/confine.mjs', import.meta.url)))

test('wraps the exact argv behind the runner', () => {
  const argv = ['pwsh', '-NoProfile', '-Command', 'Get-Location']
  const out = buildConfinedArgv(argv, POLICY, OPTS)
  assert.deepEqual(out.argv, ['node.exe', 'runner.mjs', '--workspace', 'E:\\work', '--mode', 'workspace-write', '--', ...argv])
  assert.equal(out.enforcement, 'partial', 'no MIC label => partial, never full')
  assert.ok(out.denialSignatures.includes('access is denied'))
})

test('the runner-failure signature is the literal the runner prints (not a shared alias)', () => {
  // ★ 复审发现：若断言取自同一个常量，改掉常量两边一起变 ⇒ 恒真。
  //   这里钉死字面量，runner 或 confine 任一方漂移都会红。
  const out = buildConfinedArgv(['cmd'], POLICY, OPTS)
  assert.deepEqual(out.runnerFailureRules, [{ allowedExitCodes: [127], fatalSignatures: ['nomiv-run: '] }])
  assert.equal(RUNNER_FAILURE_SIGNATURE, 'nomiv-run: ')
  assert.ok(DENIAL_SIGNATURES.includes('access to the path'))
})

test('read-only is confined through the same runner', () => {
  const out = buildConfinedArgv(['cmd', '/c', 'dir'], { ...POLICY, mode: 'read-only' }, OPTS)
  assert.deepEqual(out.argv.slice(0, 7), ['node.exe', 'runner.mjs', '--workspace', 'E:\\work', '--mode', 'read-only', '--'])
})

test('a provider-owned session temp adds --temp and the two capability SIDs', () => {
  const out = buildConfinedArgv(['cmd'], POLICY, {
    ...OPTS,
    tempDir: 'C:\\tmp\\dsh-nomiv-x',
    writeSid: 'S-1-4-1-1',
    tempWriteSid: 'S-1-4-1-1-1',
  })
  assert.deepEqual(out.argv, [
    'node.exe', 'runner.mjs',
    '--workspace', 'E:\\work',
    '--mode', 'workspace-write',
    '--temp', 'C:\\tmp\\dsh-nomiv-x',
    '--write-sid', 'S-1-4-1-1', '--temp-write-sid', 'S-1-4-1-1-1',
    '--', 'cmd',
  ])
})

test('SID half-pairs and read-only temp are rejected before spawning', () => {
  assert.throws(() => buildConfinedArgv(['cmd'], POLICY, { ...OPTS, writeSid: 'S-1-4-1-1' }), /together/)
  assert.throws(() => buildConfinedArgv(['cmd'], POLICY, { ...OPTS, writeSid: 'S-1-4-1-1', tempWriteSid: 'S-1-4-1-1-1' }), /tempDir/)
  assert.throws(
    () => buildConfinedArgv(['cmd'], { ...POLICY, mode: 'read-only' }, { ...OPTS, tempDir: 'C:\\tmp' }),
    /read-only accepts neither/,
  )
})

test('danger-full-access is refused — the executor must bypass the provider instead', () => {
  assert.throws(() => buildConfinedArgv(['cmd'], { ...POLICY, mode: 'danger-full-access' }, OPTS), /unsupported sandbox mode/)
})

test('rejects empty argv and a missing workspace root', () => {
  assert.throws(() => buildConfinedArgv([], POLICY, OPTS), /non-empty array/)
  assert.throws(() => buildConfinedArgv(['cmd'], { mode: 'workspace-write' }, OPTS), /workspaceRoot/)
})

test('the default runner is a real file next to confine.mjs', () => {
  assert.equal(dirname(DEFAULT_RUNNER_PATH), HERE)
  assert.ok(existsSync(DEFAULT_RUNNER_PATH), `missing ${DEFAULT_RUNNER_PATH}`)
})
