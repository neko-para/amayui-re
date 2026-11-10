# 🐞S3 mutate-check 的 CTest 超时在 macOS 上仍空等 5 s：zombie 未收尸 ⇒ isProcessAlive 一直真 ⇒ pnpm verify 红

- id: REQ-01M71YHT35RMW1XA6CCSNT4RNM
- type: bug
- status: done
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- verify: tools/test/mutate-timeout.test.mjs#a) 超时真的触发：假守卫睡 5 s、超时 2 s ⇒ 立刻返回 timedOut，且整棵进程树（含孙进程）被收掉
- repro: tools/test/mutate-timeout.test.mjs#a) 超时真的触发：假守卫睡 5 s、超时 2 s ⇒ 立刻返回 timedOut，且整棵进程树（含孙进程）被收掉
- severity: S3
- tags: [emulator-lift, toolchain]

## 现象（2026-11-10，macOS arm64 / Node v24.16）

`pnpm verify`（工具链档）里的 `tools/test/mutate-timeout.test.mjs` 用例 **a)** 红：

```
✖ a) 超时真的触发：… (7026ms)
  AssertionError: 超时必须**不等它自己结束**（等了 7024 ms ≥ sleep 5000 ms）
```

同一条在 HEAD 上也红（把 `tools/mutate-check.mjs` 还原到 HEAD 复跑：`elapsed 7048`）⇒ 不是某次改动引入的。

## 根因（已核，探针在 `.tmp/reveq/mutate-timeout/`）

`runTimedCommand` 的等待循环用 `sleepSync`（`Atomics.wait`）**阻塞事件循环**，而 libuv 的 `uv__chld → waitpid`
是事件循环里的 watcher ⇒ 被 SIGKILL 的孩子停在 **zombie**（`exitCode`/`signalCode` 恒 null、`'exit'` 在解除阻塞那刻
才触发），而 `isProcessAlive()` 用 `kill(pid,0)` —— 对 zombie **仍然成功** ⇒ 5 s deadline 空转。

同根的两个缺陷（同一轮实测）：
- ① **正常结束也白等 2 s**（trivial 命令 2094 ms；81 条 CTest × 3 次调用 ≈ 486 s 推算）。
- ② **wrapper 在写退出码前死 ⇒ 被误报成 `timedOut`**（`timeoutSec:1` 实测 6036 ms）。

## 处置（2026-11-10）

- `runTimedCommand` 的等待循环改 **async**（`await sleep(25)`）；`runCtestOnce` / `runMutationEntry` / `main`
  连带 async，直跑行改 `.then/.catch`（`.catch` 落 2 = "还原失败 / 内部错误"，⛔ 不落 0）。
- **配套 (1)**：`gone`（wrapper 没了没写退出码）分支补 `killProcessTree` —— 否则 A 之后 ~50 ms 就返回，
  会把 ctest + 被守 exe 留成孤儿（旧实现靠"最终走 timedOut ⇒ kill"兜住）。
  ★ **Windows 上补这一刀收不到**：`taskkill /T` 靠父进程枚举子进程，wrapper 已死就枚举不到被收养的 `ctest`
  ⇒ `TIMED_WRAPPER` 由 `spawnSync` 改 `spawn`（退出码映射不变：number → 原值 / 信号 → 125 / spawn 失败 → 126），
  spawn 后把**真命令的 pid** 写进 `AMAYUI_MUTATE_CHILD_FILE`；`gone` 分支在 **win32** 上再用它补一刀
  （POSIX 不走这条：组长被收尸后 `kill(-pgid)` 仍有效，猜 pid 反而有误杀风险）。
- **配套 (2)**：`restoreOnSignal` 在 `process.exit` 前杀掉在飞的 wrapper（模块级 `activeTimedPid`）——
  这之前它是**死代码**（`Atomics.wait` 与 `spawnSync` 期间 SIGINT 根本不派发）。
- 守卫（`tools/test/mutate-timeout.test.mjs`）：a) 墙钟预算 `< TIMEOUT_SEC*1000+1000` · a2) 新增
  "普通结束不许白等" `< TIMEOUT_SEC*1000` · b) `< TIMEOUT_SEC*1000+3000` · `waitDead` 缺省 1500 ·
  **新增 d)** "wrapper 崩了 ≠ 超时" 且树（含孙进程）必须被收。
- 变异：`MUTATIONS` 加第 97 条（唯一指向 `tools/mutate-check.mjs` 自己的一条）—— 把 `await sleep(25)`
  回退成 `sleepSync(25)` ⇒ a) 红。★ `from` 在文件里出现两次（真代码在前、条目字面量在后），`before.replace`
  取第一处；若代码挪到清单之后会变成"守卫没红"并 exit 1（响亮）。

## 读数（可重跑；探针现在是 async）

```
node -e "import('./tools/mutate-check.mjs').then(async m=>{const fs=await import('node:fs');
  const d=fs.mkdtempSync('.tmp/probe-');const f=d+'/f.mjs';fs.writeFileSync(f,'setTimeout(()=>{},5000);');
  const t=Date.now();const r=await m.runTimedCommand(process.execPath,[f],{root:d,cwd:d,timeoutSec:2});
  console.log(Date.now()-t, JSON.stringify(r));})"
```

- 修前：`7022 {"timedOut":true,"timeoutSec":2,"treeKilled":true,"elapsedMs":7018}`。
- 修后（**父级复核**）：单测 **5/5**，a 2046 / a2 195 / b 2322 / c 73 / d 109 ms（总 4.8 s，修前 24.8 s）；
  `pnpm verify`（工具链档）**459 pass / 0 fail**（修前 457 / 1），整档耗时 13 s（修前 ~25 s，2 s 税消失）。

## 未做 / 边界

- **SIGINT 路径没有机械守卫**：`restoreOnSignal` 杀 `activeTimedPid` 只有代码审读，没有真 Ctrl-C 冒烟
  （要真 CTest 构建目录 + 交互）；将来若要守，形状 = 起真 CTest 后发 SIGINT，断言进程树消失。
- 只在 macOS 实测；Windows 侧按 libuv 源码（`uv_kill` sig-0 走 `GetExitCodeProcess`）不该出现，**未实测**。
