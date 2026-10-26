# emulator-cxx --skip-logo（ShowLogo=0，仍走 boot 到 TITLE）

- id: REQ-01M5VV4ZSGQ8PGNYDBAA47VBTP
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- done_reason: --skip-logo 置 ShowLogo=0；footprint 证明 script-chain 不再含 21090、0x20F 不出现，仍到 21092；--skip-logo --play-frames 1 正常跑完并退出；可选 CTest emulator-core-boot-play-skip-logo。
- tags: [boot, cli]

## 交付物

`apps/emulator-cxx` 新增 `--skip-logo`：在跑根脚本前把引擎的 **ShowLogo 格**置 0；根脚本的 `0x130 load-show-logo` 因而走“跳过 LOGO”分支，不执行 `call-script 5262`，但其余 boot 流程照旧，仍到 TITLE。

## 判据 / 复现

- `--footprint --skip-logo --max-steps 600000`：`script-chain` 不含 21090，且 `0x20F play-movie` 不出现；最后仍到 script 21092（TITLE）。
- 不带 `--skip-logo` 的同一命令：出现 21090 与 `0x20F`。
- `--skip-logo --play-frames 1` 正常退出（窗口路径跑到 TITLE 并呈现 1 帧）。
- 可选 CTest `emulator-core-boot-play-skip-logo`（仅在设置 `EMULATOR_CXX_INSTALL_DIR` 时注册）。

## 非目标

⛔ 不是“直接跳到 TITLE”的宿主捷径：它只写引擎自己的 ShowLogo 格，根脚本仍按 `0x130` 的分支执行。
