# 🐞S3 mutate-check 的 CTest 守卫没有超时：一条会挂死的变异把整轮自检卡住（实测 20+ 分钟）

- id: REQ-01M71B4JXCGWR3MV1HNB6T8D05
- type: bug
- status: done
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- verify: tools/test/mutate-timeout.test.mjs#b) 假守卫超时 ⇒ 记「不可判定（超时）」而不是「守卫红了」；继续跑下一条；还原 + 全文比对仍执行
- repro: tools/mutate-check.mjs#runCtestGuard
- severity: S3
- done_reason: CTest 守卫有了单测超时：超时 ⇒ 收掉 ctest 与被守可执行的整棵进程树（Win taskkill /T /F、POSIX 进程组）⇒ 记「不可判定（超时）」⇒ 仍还原 + 全文比对 ⇒ 继续下一条；超时不再被记成「守卫红了」。四态（红/绿/超时/其它不可判定）分开，超时数 >0 ⇒ exit 5（不被 exit 0 掩盖）。超时分层：普通 120 s、守卫名含 assets/external 段 600 s、--ctest-timeout 可覆盖、条目可写 ctestTimeoutSec。新用例 4 条（含假挂死命令 + 孙进程收尾 + 两条假条目继续跑），去掉超时 ⇒ 2 红 exit 1。
- tags: [tooling, mutation, harness]

## 判据（存在性 + 观察）

- **现象**：`node tools/mutate-check.mjs` 每施加一条变异就跑一次 CTest；`runCtestGuard` **不给子进程设超时**（`tools/mutate-check.mjs` 里跑
  `ctest --test-dir <buildDir> -R <名字>`）。所以只要某条变异让被守的可执行**挂死**（而不是失败退出），**整轮变异自检就卡在那一条上**。
- **实测**：原 `guest-token-stress` 那条"`acquire` 空实现"的变异把两线程的无锁并发路径打开 ⇒ 两个线程无锁并发改 `std::map` ⇒ **链表成环、烧满 20+ 分钟 CPU 不退出**
  （实测被它卡住整轮）。这条变异在 HEAD 上本来就因 `from` 过期而**从没真跑过**，`from` 修好后才暴露。
- 判据：① 给 CTest 守卫加**单测超时**（超时即判"该条不可判定"并**继续下一条**，⛔ 不许把整轮 harness 卡住）；
  ② 超时不许被记成"✅ 守卫红了"（那会把"红得有意义"变成假绿）；③ 保持"还原 + 全文比对"在所有分支上都执行。

## 为什么现在做（而不是"以后再说"）

- 变异自检是本仓"守卫真的会红"的唯一机械证据（`AGENTS.md` §5 `pnpm test:mutation`）。它自己被一条挂死变异卡住 ⇒ 那一步**静默失效**。
- 本轮已经踩到一次（不是我引入的，是 `from` 修好后暴露的既有条目）⇒ 不是假想风险。

## 修法边界

- 超时值要**按守卫类型分**（CTest 单条通常 <30 s；`assets` 档可能几分钟），⛔ 不要一个常数拍死全部。
- 超时后的输出要能区分三态：**红（预期）** / **绿（守卫没抓住）** / **不可判定（超时 / 编译失败）**；⛔ 不许把后两者混进前两者。
- ⛔ 不许靠"删掉那条会挂死的变异"解决（那是把问题藏起来）；本轮已经把那条改成同族但不碰锁的确定性形态，**那是测量侧的事**，与超时无关。
- 守卫：`tools/test/` 下加一条 —— 造一个**故意 sleep 超过超时**的假守卫命令，断言工具把它记成"不可判定"且**继续跑下一条**（不 hang 整轮）。

## 与别的单的关系

- 发现于 `REQ-01M70S2WR4S8MN2JDWJRJD6WFD`（派发层拆分，测量时被卡）。
- 相邻：`REQ-01M70KGDXSKDVK7MCJYFABXERH`（变异清单 `from` 失配 —— 已收口，加了 `--list` 静态检查）；本单补的是"跑起来之后不许卡死"。
