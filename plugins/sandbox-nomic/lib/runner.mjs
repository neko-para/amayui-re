/**
 * plugins/sandbox-nomic/lib/runner.mjs —— no-MIC 的 Windows 执行器前缀。
 *
 * 形态与出厂的 `@deepseek-ai/dsh-sandbox-windows-acl/runner` 相同（argv 前缀包装器），
 * 差别在于**不降低令牌完整性、不写强制标签**：
 *
 *   [node, runner.mjs, '--workspace', <dir>, '--mode', <read-only|workspace-write>,
 *    ['--temp', <dir>], ['--write-sid', <sid>, '--temp-write-sid', <sid>],
 *    ['--report-integrity'], '--', <argv...>]
 *
 * 两种授权所有权：
 * - **seam 托管**（给了 `--write-sid` + `--temp-write-sid`，由 provider 持有每会话 temp）：
 *   runner 只校验两个 SID 与各自路径匹配，`manageDacls: false`，不授权也不撤权；
 * - **独立运行**（不给 SID）：`--temp` 当**根**用，runner 自己建随机私有子目录、派生 SID、
 *   授权并在退出时删掉（与上游 runner 的 agentless 形态一致）。
 *
 * 令牌始终是调用者令牌的 `WRITE_RESTRICTED` 复制，限制 SID = `[logon, Everyone, workspace, temp]`，
 * **完整性级别保持 Medium**（不调用 `SetTokenInformation(TokenIntegrityLevel)`），
 * 授权只写 DACL（Allow ACE + `FILE_DELETE_CHILD` Deny），不碰 SACL。
 *
 * 退出码（见 `./protocol.mjs`）：
 * - `127` = runner 自身失败（参数 / 令牌 / 授权 / 目录），stderr 打 `nomiv-run: <detail>`，
 *   seam 据此报 `SANDBOX_UNAVAILABLE`；
 * - `126` = 目标程序起不来（命令不存在、cwd 进不去），不算 runner 坏，按普通命令失败处理。
 */
import { existsSync, mkdtempSync, rmSync, statSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import {
  AclSandbox,
  assertTempRootOutsideWorkspace,
  tempWriteSid,
  tokenIntegrityLevel,
  workspaceWriteSid,
} from './acl-sandbox.mjs'
import { RUNNER_FAILURE_EXIT, RUNNER_SIGNATURE, TARGET_START_FAILURE_EXIT } from './protocol.mjs'

class RunnerFailure extends Error {}

/** 打印失败签名并抛出（顶层只对非 RunnerFailure 再打印一次）。 */
function fail(detail) {
  process.stderr.write(`${RUNNER_SIGNATURE}${detail}\n`)
  throw new RunnerFailure(detail)
}

function requireDirectory(label, path) {
  if (!existsSync(path) || !statSync(path).isDirectory()) fail(`${label} is not an existing directory: ${path}`)
}

/** 严格解析参数；`--` 之后的第一个 token 是命令。 */
function parseArgs(raw) {
  const parsed = {
    workspace: undefined,
    temp: undefined,
    mode: undefined,
    writeSid: undefined,
    tempWriteSid: undefined,
    reportIntegrity: false,
    command: undefined,
    args: [],
  }
  let index = 0
  for (; index < raw.length; index++) {
    const token = raw[index]
    if (token === '--') {
      index++
      break
    }
    if (token === '--report-integrity') {
      parsed.reportIntegrity = true
      continue
    }
    const value = raw[++index]
    if (value === undefined) fail(`missing value after ${token}`)
    switch (token) {
      case '--workspace':
        parsed.workspace = value
        break
      case '--temp':
        parsed.temp = value
        break
      case '--mode':
        parsed.mode = value
        break
      case '--write-sid':
        parsed.writeSid = value
        break
      case '--temp-write-sid':
        parsed.tempWriteSid = value
        break
      default:
        fail(`unknown argument: ${token}`)
    }
  }
  if (parsed.workspace === undefined) fail('missing --workspace')
  if (parsed.mode !== 'read-only' && parsed.mode !== 'workspace-write') fail(`unknown mode: ${String(parsed.mode)}`)
  const seamManaged = parsed.writeSid !== undefined || parsed.tempWriteSid !== undefined
  if (seamManaged && (parsed.writeSid === undefined || parsed.tempWriteSid === undefined)) {
    fail('--write-sid and --temp-write-sid must be given together')
  }
  if (seamManaged && parsed.mode === 'read-only') fail('read-only does not accept --write-sid or --temp-write-sid')
  if (seamManaged && parsed.temp === undefined) fail('--write-sid requires --temp (the provider-owned private temp directory)')
  if (!parsed.reportIntegrity) {
    const command = raw[index]
    if (command === undefined) fail('missing command after --')
    parsed.command = command
    parsed.args = raw.slice(index + 1)
  }
  return { ...parsed, seamManaged }
}

async function main() {
  const parsed = parseArgs(process.argv.slice(2))
  requireDirectory('--workspace', parsed.workspace)
  // ★ 与上游一致：SID 从**字面**路径派生（provider 已把 workspace 规范化），不在这里 realpath，
  //   否则 seam 传下来的 `--temp-write-sid` 会与 runner 重算的值对不上。
  const workspace = parsed.workspace

  let ownedTemp
  let sandbox
  let initialized = false
  try {
    let tempDir = null
    let writeSid
    let tempSid
    if (parsed.mode === 'workspace-write') {
      if (parsed.seamManaged) {
        requireDirectory('--temp', parsed.temp)
        tempDir = parsed.temp
        writeSid = workspaceWriteSid(workspace)
        tempSid = tempWriteSid(tempDir)
        if (parsed.writeSid !== writeSid) fail('--write-sid does not match --workspace')
        if (parsed.tempWriteSid !== tempSid) fail('--temp-write-sid does not match --temp')
      } else {
        const root = parsed.temp ?? tmpdir()
        assertTempRootOutsideWorkspace(workspace, root)
        ownedTemp = mkdtempSync(join(root, 'dsh-nomiv-'))
        tempDir = ownedTemp
        writeSid = workspaceWriteSid(workspace)
        tempSid = tempWriteSid(tempDir)
      }
    }

    sandbox = new AclSandbox({
      writableDirs: parsed.mode === 'workspace-write' ? [workspace] : [],
      ...(tempDir === null ? {} : { tempDir }),
      ...(writeSid === undefined ? {} : { writeSid, tempWriteSid: tempSid }),
      mode: parsed.mode,
      manageDacls: !parsed.seamManaged,
    })
    await sandbox.init()
    initialized = true

    if (parsed.reportIntegrity) {
      process.stdout.write(`${tokenIntegrityLevel(sandbox.api, sandbox.token)}\n`)
      return 0
    }

    if (tempDir !== null) {
      // 子进程以 lpEnvironment = NULL 创建 ⇒ 继承 runner 当前的环境块；
      // 这两个赋值走 SetEnvironmentVariableW，因此子进程看到的是私有 temp。
      process.env.TMP = tempDir
      process.env.TEMP = tempDir
    }

    let child
    try {
      child = sandbox.spawn({ command: parsed.command, args: parsed.args, stdio: 'inherit' })
    } catch (error) {
      // 目标程序起不来（命令不存在 / cwd 进不去）：这是命令失败，不是 runner 坏。
      // 用 126 让 seam 的 runnerFailureRules（只认 127）不去报 SANDBOX_UNAVAILABLE。
      process.stderr.write(`${RUNNER_SIGNATURE}spawn failed: ${error instanceof Error ? error.message : String(error)}\n`)
      return TARGET_START_FAILURE_EXIT
    }
    const { exitCode } = await child.wait()
    return exitCode
  } finally {
    if (initialized) {
      try {
        sandbox.dispose()
      } catch (error) {
        process.stderr.write(`${RUNNER_SIGNATURE}cleanup: ${error instanceof Error ? error.message : String(error)}\n`)
      }
    }
    if (ownedTemp !== undefined) {
      try {
        rmSync(ownedTemp, { recursive: true, force: true })
      } catch (error) {
        process.stderr.write(`${RUNNER_SIGNATURE}cleanup: ${error instanceof Error ? error.message : String(error)}\n`)
      }
    }
  }
}

main().then(
  (exitCode) => {
    process.exitCode = exitCode
  },
  (error) => {
    if (!(error instanceof RunnerFailure)) {
      process.stderr.write(`${RUNNER_SIGNATURE}${error instanceof Error ? error.message : String(error)}\n`)
    }
    process.exitCode = RUNNER_FAILURE_EXIT
  },
)
