# 🐞S3 mutate-check 的 CTest 超时在 macOS 上仍空等 5 s：zombie 未收尸 ⇒ isProcessAlive 一直真 ⇒ pnpm verify 红

- id: REQ-01M71YHT35RMW1XA6CCSNT4RNM
- type: bug
- status: open
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- repro: tools/test/mutate-timeout.test.mjs#a) 超时真的触发：假守卫睡 5 s、超时 2 s ⇒ 立刻返回 timedOut，且整棵进程树（含孙进程）被收掉
- severity: S3
- tags: [emulator-lift, toolchain]

## 现象（2026-11-10，macOS arm64 / Node v24.16）

`pnpm verify`（工具链档）里的 `tools/test/mutate-timeout.test.mjs` 用例 **a)** 红：

```
✖ a) 超时真的触发：假守卫睡 5 s、超时 2 s ⇒ 立刻返回 timedOut，且整棵进程树（含孙进程）被收掉 (7026ms)
  AssertionError: 超时必须**不等它自己结束**（等了 7024 ms ≥ sleep 5000 ms）
```

**不是本轮改动引起的**：把 `tools/mutate-check.mjs` 还原到 HEAD 后复跑同一探针，仍是 `elapsed 7048`。

## 读数（一条命令）

```
node -e "import('./tools/mutate-check.mjs').then(async m=>{const fs=await import('node:fs');
  const d=fs.mkdtempSync('.tmp/probe-');const f=d+'/f.mjs';fs.writeFileSync(f,'setTimeout(()=>{},5000);');
  const t=Date.now();console.log(Date.now()-t, m.runTimedCommand(process.execPath,[f],{root:d,cwd:d,timeoutSec:2}));})"
```

⇒ `elapsed 7022` / `{"timedOut":true,"timeoutSec":2,"treeKilled":true,"elapsedMs":7018}`。

⇒ 超时**判定**是对的（2 s），但函数在 2 s 之后又空转了**整整 5 s**（= `runTimedCommand` 里"等根进程真的消失"的
`deadline = Date.now() + 5000`）。

## 机制（假设；未取证到 `waitpid` 层）

等待循环用 `sleepSync`（`Atomics.wait`）**阻塞事件循环**。被 SIGKILL 的子进程此时只到 **zombie**，
Node 来不及 `waitpid` 收尸；而 `isProcessAlive()` 用 `kill(pid, 0)` —— 对 zombie **仍然返回成功**
⇒ 5 s deadline 一直以为"还活着"。Windows 上 `taskkill /T /F` 后进程直接从系统消失 ⇒ 这条在 Windows 上不出现。

## 影响

- `pnpm verify` 在 macOS 上**红**。真正的语义没坏（`timedOut` / `treeKilled` 都对），坏的是**墙钟**：
  真遇到"一条会挂死的变异"时，每条超时白等 5 s。
- 与本轮 room-exit / BUG-16 无关（实测见上）。

## 处置（待做；本轮边界外，只开单）

- 判据：同一条用例 a) 在 macOS 上 `elapsed` 落回 ≈2 s 量级，且 `timedOut` / `treeKilled` 仍为真。
- 形状候选（**未裁决**）：① 等待循环改成异步（`setTimeout` 轮询，让 libuv 收尸）；② 判据换成
  `child.exitCode !== null || child.signalCode !== null`（同样要事件循环）；③ 对 zombie 查 `ps -o stat=`（本机沙箱里不可用）。
  ⇒ 动的是 `mutate-check` 的超时机制，**要连带改 guard 与变异条**，单独一轮做。

## 未做

- 没有在 Windows / Linux 上复测（口径是 win32 优先；这条是 macOS 侧兼容性）。
- 没有改 `runTimedCommand` 的任何代码（本轮只做 room-exit / BUG-16）。
