# AOT CLI 缺口（pure 可断）：--dump-frame 开跑前建目录 · --time-limit 解析纯函数化

- id: REQ-01M7JN4ZPCFN5Y161JK8YTNNAP
- type: req
- status: open
- parent: REQ-01M7J6D62QFRZC2454GV7CKJDJ
- tags: [emulator-lift, aot, cli, test]

## 为什么单独一张（父单 `REQ-01M7J6D62QFRZC2454GV7CKJDJ` 的三分之一）

父单把"解释器退役后 AOT 上没有端到端断言的面"混成一张清单，里面是三种**失效方式不同**的东西。
本单只收第一格：**pure 可断** —— 红了 = 一条 pure CTest 红；不需要 guest 代码、不需要安装树、不需要人读代码。

## 要做（两条，都可机械判）

1. **`--dump-frame` 的"开跑前把目录建好"闸门**（父单第 4 条，最像该补的一条）：
   旧缺陷形态 = **目录不存在 ⇒ 跑到写盘失败才报**（空转 546 s）。AOT 侧今天就缺这个闸门。
2. **`--time-limit` 的解析**（父单第 1 条）：把解析抽成纯函数（`strtod + isfinite + <0 ⇒ 2`），
   断言"`0` 与省略等价"、负数 `exit 2`、`abc` / `1e400` 各自的行为。

## 判据（能红）

- 4 ⇒ 新增 pure 档 CTest：拿一个不存在的目录当 `--dump-frame`，AOT 必须**在开跑前**建目录（exit 0 + 目录存在）；
  目录建不出来 ⇒ exit 2 且 stderr 点名 `--dump-frame`。⚠ 这条**不需要** guest 代码（启动期判定）。
- 1 ⇒ pure 单测断言 0 / -1 / abc / 1e400 四档。
- 按 `AGENTS.md` §12.4：想在 reviewed 级用，就把 `add_test` 注册在 `repo:apps/emulator-lift/CMakeLists.txt` 最外层；
  新增守卫要顺手往 `repo:tools/mutate-check.mjs` 加一条（§5.3）。

## 边界

⛔ 不碰"要 guest 代码 / 要安装树才成立"的那些（见兄弟单：只能代码审阅 · 需真安装 assets）。
