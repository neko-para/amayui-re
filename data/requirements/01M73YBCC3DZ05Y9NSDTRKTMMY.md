# emulator-lift · --dump-frame 目录不存在时进程空转（546 s）而不报错

- id: REQ-01M73YBCC3DZ05Y9NSDTRKTMMY
- type: bug
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/cli_dump_frame.cpp#cli-dump-frame
- repro: apps/emulator-lift/src/main.cpp#frame_dump
- severity: S3
- tags: [emulator-lift cli usability]

## 一句话
`--dump-frame <dir>` 的**目录不存在**时，进程既不报错也不退出，而是空转很久（A10 实测 **546 s**，补建目录后同一命令 7 s 完成）。
A7、A10 两次取证都被它拖过，容易被误判成"模拟器卡死"。

## 观测
- 现象：不给 `--dump-frame` 的目录预建 ⇒ 该次 run 长期不结束；`New-Item -ItemType Directory -Force` 之后同一命令数秒完成。
- 位置：`--dump-frame` 的写入点（按**符号**找 `frame_dump` 的消费者），失败时没有向上报告。

## 修法
- 解析参数后 `std::filesystem::create_directories(dir)`；**失败**（权限/非法路径）⇒ 明确 `stderr` + `exit 2`（⛔ 不静默）。
- 每帧写帧文件失败 ⇒ 打一次默认级别 warn 并**计数**（`dumpFailures=` 进 run summary），⛔ 不每帧刷屏。

## 守卫
- CTest（pure）：`--dump-frame <不存在的目录> --max-frames 1` ⇒ 要么建目录并成功、要么**在合理时间内**明确失败；
  断言"进程在 30 s 内结束"且 stderr 含原因。修前会一直空转 ⇒ 红。
- 变异：把 `create_directories` 去掉 ⇒ 该用例红。

## 未做
- 不改 `--dump-frame` 的文件命名模板语义（`{frame}` 模板不在本单）。
