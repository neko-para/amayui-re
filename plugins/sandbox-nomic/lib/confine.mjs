/**
 * plugins/sandbox-nomic/lib/confine.mjs —— `ctx.sandbox.confine()` 的纯函数部分。
 *
 * 契约与 `@deepseek-ai/dsh-sandbox` 的 `SandboxProvider.confine` 一致：
 * 给一段**原样 argv** 与一次调用的 policy，返回
 * `{ argv, enforcement, denialSignatures, runnerFailureRules }`。
 *
 * 与出厂 `dsh-sandbox-local` 的差别：
 *   ① runner 是我们自己的（`./runner.mjs`），不是 windows-acl 的 runner；
 *   ② `enforcement` 诚实报 `partial`：出厂 windows-acl 也是 `partial`（硬链接 / 读 /
 *      AppContainer 边界），但**缺口集合不同**——本变体没有 MIC，因此 Everyone 可写路径、
 *      NULL DACL、FAT 也落在边界外。
 *
 * 本文件只 import `node:url` + `./protocol.mjs`，不碰 fs / 原生模块 ⇒ 可以被
 * `@env pure` 的测试直接引用。
 */
import { fileURLToPath } from 'node:url'
import { DENIAL_SIGNATURES, RUNNER_FAILURE_EXIT, RUNNER_SIGNATURE } from './protocol.mjs'

/** 本包 runner 的绝对路径（`node <runner> ...` 的第二个 argv）。 */
export const DEFAULT_RUNNER_PATH = fileURLToPath(new URL('./runner.mjs', import.meta.url))

/** 兼容旧名字：失败签名现在只在 `protocol.mjs` 定义。 */
export const RUNNER_FAILURE_SIGNATURE = RUNNER_SIGNATURE

export { DENIAL_SIGNATURES }

/** 只接受执行器真正会传的两种受限模式；`danger-full-access` 根本不走 confine。 */
const CONFINED_MODES = new Set(['read-only', 'workspace-write'])

/**
 * 把一段 argv 包成「no-MIC runner + 原 argv」。
 *
 * @param {readonly string[]} argv - 执行器即将 spawn 的精确 argv。
 * @param {{ mode: string, workspaceRoot: string }} policy - 本次调用的策略。
 * @param {{
 *   execPath?: string,
 *   runnerPath?: string,
 *   tempDir?: string,
 *   writeSid?: string,
 *   tempWriteSid?: string,
 * }} [options] - 测试注入点 / provider 持有的会话私有 temp 与能力 SID。
 * @returns {{ argv: string[], enforcement: 'partial', denialSignatures: string[], runnerFailureRules: Array<{ allowedExitCodes: number[], fatalSignatures: string[] }> }}
 */
export function buildConfinedArgv(argv, policy, options = {}) {
  const mode = policy?.mode
  if (!CONFINED_MODES.has(mode)) {
    throw new Error(`sandbox-nomic: unsupported sandbox mode "${String(mode)}" (only read-only and workspace-write are confined)`)
  }
  if (!Array.isArray(argv) || argv.length === 0) {
    throw new Error('sandbox-nomic: argv must be a non-empty array')
  }
  const workspaceRoot = policy?.workspaceRoot
  if (typeof workspaceRoot !== 'string' || workspaceRoot.length === 0) {
    throw new Error('sandbox-nomic: policy.workspaceRoot must be a non-empty absolute path')
  }

  const { tempDir, writeSid, tempWriteSid } = options
  const anySid = writeSid !== undefined || tempWriteSid !== undefined
  if (anySid && (writeSid === undefined || tempWriteSid === undefined)) {
    throw new Error('sandbox-nomic: writeSid and tempWriteSid must be supplied together')
  }
  if (anySid && (typeof tempDir !== 'string' || tempDir.length === 0)) {
    throw new Error('sandbox-nomic: writeSid/tempWriteSid require a provider-owned tempDir')
  }
  if (mode === 'read-only' && (anySid || tempDir !== undefined)) {
    throw new Error('sandbox-nomic: read-only accepts neither tempDir nor write SIDs')
  }

  const execPath = options.execPath ?? process.execPath
  const runnerPath = options.runnerPath ?? DEFAULT_RUNNER_PATH
  const args = [execPath, runnerPath, '--workspace', workspaceRoot, '--mode', mode]
  if (tempDir !== undefined) args.push('--temp', tempDir)
  if (writeSid !== undefined) args.push('--write-sid', writeSid, '--temp-write-sid', tempWriteSid)
  args.push('--', ...argv)

  return {
    argv: args,
    enforcement: 'partial',
    denialSignatures: [...DENIAL_SIGNATURES],
    runnerFailureRules: [{ allowedExitCodes: [RUNNER_FAILURE_EXIT], fatalSignatures: [RUNNER_SIGNATURE] }],
  }
}
