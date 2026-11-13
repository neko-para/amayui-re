# 🐞S3 mutate-timeout 的「wrapper 崩了 ≠ 超时」用墙钟判据 ⇒ 机器被编译压住时假红（重跑即绿）

- id: REQ-01M78A0Z5G7P7CGYRXPFEBJKCA
- type: bug
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- repro: tools/test/mutate-timeout.test.mjs#wrapper 崩了 ≠ 超时
- tags: [toolchain test flaky]

## 现象

`tools/test/mutate-timeout.test.mjs:333` 的用例「wrapper 崩了 ≠ 超时」在机器被**编译压住**时**假红**：

```
elapsedMs 2413ms > 1s ⇒ 判成 timeout
```

重跑即绿（2026-11-13，`plugins/wait-job` 门禁期间实测两次；同轮的另一个代理正在 `cmake --build`）。

## 诊断

该用例用**墙钟绝对值**区分「wrapper 崩了」与「真超时」。这个判据在空闲机器上成立，
但在负载下两种情形**不可区分**（正常路径也会超过阈值）。

⇒ 这是"守卫在负载下自己红"的一类：比没有守卫更坏，因为
① 假红会被当成"别人的改动弄坏了门禁"；② 长期靠"重跑即绿"抹掉，等于训练所有人忽略门禁。

## 要做

把判据从**墙钟**换成**结构性**（任一即可，按最小改动选）：
1. 让被测 wrapper 输出**明确的退出码或标记**（崩 = 特定退出码 / stderr 前缀），据此判，不看耗时；
2. 或改用**同轮基线比例**（同一次运行里先测一个已知快速子命令，用比值而不是绝对值）；
3. 或断言 wrapper 的 stderr 文本标记（"crash" vs "timeout"）。

⛔ 不许只是把 `1s` 调大（那是把同一类假红推迟到更重的负载下才发作）。

## 判据

- 在 **4 路并行 `cmake --build`** 压满机器时连跑 10 次，**0 次红**；
- 同时仍**能红**：真施加"wrapper 崩"与"真超时"两种变异，各自**必须红**（两条都要，否则改成了恒绿）；
- 若选方案 1/3，把"退出码 / 标记"的语义写进 `tools/mutate-check.mjs` 的自描述，不要只活在测试里。

## 反证条件

- 若在 4 路并行下 10 次仍红，说明墙钟不是唯一变量（还有别的共享状态）⇒ 先查临时目录 / 端口 / 全局变量的串用。
