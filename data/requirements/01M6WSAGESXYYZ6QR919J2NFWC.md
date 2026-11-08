# tools/mutate-check.mjs 缺 input-injection 条目：加 ctest:input-injection（buildDir=apps/emulator-lift/out）+ GetAsyncKeyState 恒 0 的变异

- id: REQ-01M6WSAGESXYYZ6QR919J2NFWC
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift mutation guard]

## 缺口

`input-injection` 是顶层 CTest（pure），它守的是“注入是**唯一输入状态源**”：把 `GetAsyncKeyState` 改回读 OS、或让注入改走 SDL 事件泵，它应当当场红。但 `repo:tools/mutate-check.mjs` 的清单里**没有**这一条 ⇒ `pnpm test:mutation` 跑不到它，“它会红”目前只是**声称**。清单条数以 `node tools/mutate-check.mjs --list` 为准（⛔ 不在此处写数字）。

## 要加的那一条（实现者实测：施加三次都红）

- `file`: `repo:apps/emulator-lift/src/shims/user32_window.cpp`
- 变异点：`shimGetAsyncKeyState` 里的 `call.result = cpu.windows->keyDown(vk) ? 0x8000u : 0u;` ⇒ 改成 `call.result = 0u;`（恒报“没按下”；`from` / `to` 的缩进以文件为准）
- `guard`: `ctest:input-injection`，并写 `ctest: 'input-injection'`（与顶层 `add_test` 的名字逐字一致，见 `repo:apps/emulator-lift/CMakeLists.txt`）
- `buildDir`: `apps/emulator-lift/out`（cxx 与 lift 是两个独立工程；harness 已支持按条目覆盖，见 `repo:tools/mutate-check.mjs` 的 `runCtestGuard`）
- `what`：写清“采样恒为 0 ⇒ 注入通道的 `GetAsyncKeyState` 断言当场红”，并点名它证伪的是哪条判据。

## 为什么现在才算“真的守着”

`pnpm test:mutation` 的承诺是“每条关键常量都有一处已知破坏能让对应守卫红”。没有条目 ⇒ 这条 CTest 与其它 pure 守卫的地位不同（别人红过、它没有）。这也是 `AGENTS.md` §5“加新守卫时顺手往清单里加一条”欠的那一笔。

## 父级裁决（2026-11-08）：条目本身**稍后统一做**，本轮只登记

不在这轮改 `repo:tools/mutate-check.mjs`（它动的是工作树文件，且与正在跑的构建 / 其它条目互相影响）。

## 判据（做的时候）

- `node tools/mutate-check.mjs --list` 里出现这一条；`pnpm test:mutation` 跑到它时红、还原后绿。
- 还原必须逐字回写（harness 已有全文比对，不一致就中止）。
- `buildDir` 写 `apps/emulator-lift/out`，⛔ 不要复用 cxx 的构建目录。
- 若此后 `input-injection` 改名 / 换注册位置（不再顶层），这条要跟着改 —— 名字漂了就等于没守。
