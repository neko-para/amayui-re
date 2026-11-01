/**
 * plugins/sandbox-nomic —— 用「受限令牌 + 能力 SID 的 DACL」实现 `workspace-write`，
 * **不**给工作区打 Low 完整性标签、**不**降低子进程令牌的完整性级别。
 *
 * 它是 `@deepseek-ai/dsh-sandbox` 的另一个 provider：注册成 `ctx.sandbox`，被
 * `dsh-pwsh-sandbox` 通过 `ctx.sandbox.confine()` 消费。bundle patch
 * （`cordis.patch.yml`）先 `disabled: true` 掉出厂的 `sandbox` 行（`dsh-sandbox-local`），
 * 再插入本行，因此同一个上下文里只有一个 `sandbox` 服务。
 *
 * 授权生命周期（与出厂 `dsh-sandbox-local` 对齐）：
 * - **工作区**：每个 workspace 一条 standing 授权（跨会话复用缓存，**不撤**）；
 * - **私有 temp**：每个 **live session/workspace 对**一个随机目录 + 独立 SID，provider
 *   dispose 时撤 ACE 并删目录。同一会话的连续命令复用同一个 `%TEMP%`；
 * - **无会话**（agentless）调用：不建 temp，由 runner 自己建一次性私有目录（`manageDacls`）。
 *
 * @module @amayui/dsh-plugin-sandbox-nomic
 */
import { mkdtempSync, realpathSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { SandboxProvider } from '@deepseek-ai/dsh-sandbox'
import {
  AclWriteGrant,
  assertTempRootOutsideWorkspace,
  tempWriteSid,
  workspaceWriteSid,
} from './lib/acl-sandbox.mjs'
import { buildConfinedArgv } from './lib/confine.mjs'

/** 解析成执行世界比较用的规范路径；解析失败时保留原拼写（保守：等它存在再匹配）。 */
function canonicalPath(path) {
  try {
    return realpathSync.native(path)
  } catch {
    return path
  }
}

/** `ctx.sandbox` 的服务实现：把 argv 换成「runner + argv」，并持有每会话 temp 与授权。 */
export default class SandboxNomicProvider extends SandboxProvider {
  /** 每 workspace 一条 standing 授权（ACE 是跨会话复用缓存，不撤销）。 */
  workspaceGrants = new Map()
  /** 每 live session/workspace 对一个私有 temp + 可撤销授权。 */
  tempCapabilities = new Map()

  constructor(ctx, config) {
    super(ctx)
    this.config = config ?? {}
    ctx.logger?.info?.('sandbox-nomic: workspace-write = restricted token + capability ACLs, NO mandatory-integrity label')
    ctx.effect(() => () => this.disposeGrants())
  }

  /**
   * @param {readonly string[]} argv - 执行器即将 spawn 的精确 argv。
   * @param {{ mode: 'read-only' | 'workspace-write', workspaceRoot: string, sessionId?: string }} policy - 本次调用的策略。
   * @param {AbortSignal} [signal] - policy 解析前的取消。
   * @returns {Promise<ReturnType<typeof buildConfinedArgv>>}
   */
  async confine(argv, policy, signal) {
    signal?.throwIfAborted()
    const mode = policy?.mode
    const workspaceRoot = canonicalPath(policy?.workspaceRoot ?? '')
    if (mode !== 'workspace-write' || policy.sessionId === undefined) {
      // read-only / danger-full-access（后者在这里抛错）与 agentless workspace-write：
      // 都不建 provider 持有的 temp（agentless 由 runner 自己建一次性目录）。
      return buildConfinedArgv(argv, { mode, workspaceRoot })
    }
    const temp = this.materialize(policy.sessionId, workspaceRoot)
    return buildConfinedArgv(argv, { mode, workspaceRoot }, {
      tempDir: temp.dir,
      writeSid: workspaceWriteSid(workspaceRoot),
      tempWriteSid: temp.writeSid,
    })
  }

  /**
   * 物化一次 workspace-write 调用的授权：standing 工作区授权 + 每会话私有 temp。
   * 失败时清掉本次已建的东西再抛（fail-closed）。
   * @param {string | number} sessionId - 调用方会话标识。
   * @param {string} workspaceRoot - 已规范化的工作区根。
   * @returns {{ dir: string, writeSid: string }}
   */
  materialize(sessionId, workspaceRoot) {
    assertTempRootOutsideWorkspace(workspaceRoot, tmpdir())
    if (!this.workspaceGrants.has(workspaceRoot)) {
      const grant = AclWriteGrant.create(workspaceWriteSid(workspaceRoot))
      try {
        grant.add(workspaceRoot, true)
      } catch (error) {
        try {
          grant.dispose()
        } catch {
          // 保留原始错误；清理失败不覆盖它。
        }
        throw error
      }
      this.workspaceGrants.set(workspaceRoot, grant)
    }

    const key = JSON.stringify([String(sessionId), workspaceRoot])
    const existing = this.tempCapabilities.get(key)
    if (existing !== undefined) return existing

    const dir = mkdtempSync(join(tmpdir(), 'dsh-nomiv-'))
    const writeSid = tempWriteSid(dir)
    let grant
    try {
      grant = AclWriteGrant.create(writeSid)
      grant.add(dir)
    } catch (error) {
      try {
        grant?.dispose()
      } catch {
        // 同上：不覆盖原始错误。
      }
      try {
        rmSync(dir, { recursive: true, force: true })
      } catch {
        // 同上。
      }
      throw error
    }
    const capability = { dir, writeSid, grant }
    this.tempCapabilities.set(key, capability)
    return capability
  }

  /** 撤销每会话 temp 的 ACE、删除目录、释放全部 SID；standing 工作区 ACE 留着。 */
  disposeGrants() {
    if (this.workspaceGrants.size === 0 && this.tempCapabilities.size === 0) return
    const failures = []
    for (const grant of [...this.workspaceGrants.values(), ...[...this.tempCapabilities.values()].map((capability) => capability.grant)]) {
      try {
        grant.dispose()
      } catch (error) {
        failures.push(error)
      }
    }
    for (const { dir } of this.tempCapabilities.values()) {
      try {
        rmSync(dir, { recursive: true, force: true })
      } catch (error) {
        failures.push(error)
      }
    }
    this.workspaceGrants.clear()
    this.tempCapabilities.clear()
    if (failures.length > 0) {
      this.ctx.logger?.warn?.(`sandbox-nomic: grant cleanup completed with ${failures.length} failure(s)`)
      for (const error of failures) this.ctx.logger?.warn?.(error)
    }
  }
}
