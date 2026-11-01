/**
 * 在**受限子进程内**运行的探针：把每条写尝试的结果汇总成一行 JSON 打到 stdout。
 *
 * argv: <workspaceFile> <outsideFile> <everyoneFile>
 *
 * 它只知道「成功/被拒」，不知道原因；原因由 `verify.mjs` 结合 runner 的令牌完整性
 * 与目录 ACL 解释。`pipeSpawn` 一条是**受限令牌本身**的性质（与 MIC 无关），
 * 用来把「route A 修好了什么、没修好什么」钉在同一份输出里。
 */
import { spawnSync } from 'node:child_process'
import fs from 'node:fs'

const [workspaceFile, outsideFile, everyoneFile] = process.argv.slice(2)
const result = {}

function attempt(name, action) {
  try {
    action()
    result[name] = 'ok'
  } catch (error) {
    result[name] = `denied:${error.code ?? error.message}`
  }
}

attempt('workspace', () => fs.writeFileSync(workspaceFile, 'probe\n'))
attempt('outside', () => fs.writeFileSync(outsideFile, 'probe\n'))
attempt('everyone', () => fs.writeFileSync(everyoneFile, 'probe\n'))

try {
  const piped = spawnSync('cmd', ['/c', 'echo', 'hi'], { encoding: 'utf8', timeout: 10_000, windowsHide: true })
  result.pipeSpawn = piped.error ? `error:${piped.error.code ?? piped.error.message}` : `ok:${(piped.stdout ?? '').trim()}`
} catch (error) {
  result.pipeSpawn = `throw:${error.code ?? error.message}`
}

process.stdout.write(`${JSON.stringify(result)}\n`)
