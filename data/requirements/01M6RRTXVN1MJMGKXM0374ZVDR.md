# emulator-lift · --skip-logo：T_Command 的 show-logo 置 0，仍走 boot 到 TITLE

- id: REQ-01M6RRTXVN1MJMGKXM0374ZVDR
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/skip_logo.cpp#skip-logo
- tags: [emulator-lift boot cli]

## 原理（来自 emulator-cxx）

`emulator-cxx` 在跑根脚本前 `set_show_logo(false)`；根脚本的 `0x130 load-show-logo` 读到 0，
因而不执行 `call-script 5262`（LOGO），但其余 boot 照旧到 TITLE。真机上的格是
`T_Command + 0x5EB5C`；`[0x55E1BC]` 是 `T_Command*`。

## 落点（精确 hook）

- `0x004BAA37`：`sub_415640`（T_Command 构造）返回之后、`mov ds:dword_55E1BC,eax` 之前；
  此时 `EAX` = 新对象，构造器已在 `0x00415AE0` 把 show-logo 写成 1。
- 写 `[EAX + 0x5EB5C] = 0`；随后引擎自己发布该对象。⛔ 不跳过 boot、不改 root script、不按帧号猜 LOGO。
- 可选 `--skip-logo` 只在钩子命中时写一次；对象指针为 0 时不写、不崩、不谎报。

## 判据

- `--skip-logo --present window --raster gpu`：首帧 draw 签名 = TITLE
  （`sep_alpha=1 tex=1`）；不带 `--skip-logo` 的同一命令首帧 = LOGO（`sep_alpha=0 tex=0`）。
- `LIFT_SLOW_DRAW=1 LIFT_SLOW_DRAW_MS=0`：带 skip 时第一次 `sep_alpha=1` 出现在第 4 行；
  不带时第 582 行。
- `ctest -R skip-logo`：字段写 0、邻居字节不变、关闭时不写、空对象不崩。

## 守卫边界

CTest `skip-logo` 只守**字段写 / 邻居字节 / 关闭时不写 / 空对象不崩**；它不跑 boot、不看 draw 签名。
上面的首帧 TITLE/LOGO 对比是**真实窗口观测**（未自动化），⛔ 不要把它当成 CTest 已覆盖。

## 非目标

⛔ 不是“直接跳到 TITLE”的宿主捷径；它只写引擎自己的 ShowLogo 格。
