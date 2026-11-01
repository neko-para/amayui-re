/**
 * plugins/sandbox-nomic/lib/protocol.mjs —— runner 与 provider 之间的**唯一**协议常量。
 *
 * 为什么单独一个文件：复审发现 `nomiv-run: ` 在 `runner.mjs` 与 `confine.mjs` 各写一份，
 * 而唯一断言它的单测两边取自同一常量 ⇒ 漂移时测试恒真。现在只有这一处定义；
 * `test/confine.test.mjs` 断言的是**字面量**，`verify.mjs` 也按字面量核 runner 的 stderr。
 *
 * 退出码分层：
 * - `127` = **runner 自己坏了**（参数错、建令牌/授权失败、目录不对）⇒ seam 的
 *   `runnerFailureRules` 认这个码 + 签名，执行器据此报 `SANDBOX_UNAVAILABLE`；
 * - `126` = **目标程序起不来**（命令不存在、cwd 进不去）⇒ 不算 runner 坏，按普通命令失败处理。
 *   两者都带 `nomiv-run: ` 前缀，便于人读；区分靠退出码。
 *
 * @module @amayui/dsh-plugin-sandbox-nomic/protocol
 */

/** runner 在 stderr 上的失败前缀（含尾随空格，便于直接拼 detail）。 */
export const RUNNER_SIGNATURE = 'nomiv-run: '

/** runner 自身失败：seam 的 `runnerFailureRules.allowedExitCodes` 认它。 */
export const RUNNER_FAILURE_EXIT = 127

/** 目标程序无法创建：**不**进 runnerFailureRules（否则会把"命令打错"报成沙箱不可用）。 */
export const TARGET_START_FAILURE_EXIT = 126

/**
 * 受限令牌下的拒绝方言。与出厂 windows-acl rung 同一组字符串：子进程的
 * `Access is denied` / Node 的 `EACCES` / PowerShell 的 `Access to the path` 都要命中。
 */
export const DENIAL_SIGNATURES = Object.freeze([
  'access is denied',
  'access to the path',
  'permission denied',
  'operation not permitted',
])
