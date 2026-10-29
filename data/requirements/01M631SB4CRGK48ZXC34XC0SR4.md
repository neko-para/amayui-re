# config-toggle 依赖环境玩家配置：没有玩家文件时 OFF 分支不会建键

- id: REQ-01M631SB4CRGK48ZXC34XC0SR4
- type: bug
- status: done
- parent: REQ-01M5SZ3M7GCR0M23RSYBD7GEA5
- verify: apps/emulator-cxx/CMakeLists.txt#emulator-core-config-toggle
- repro: apps/emulator-cxx/tests/config_toggle.cmake#run_toggle
- severity: S2
- tags: [emulator-cxx, gate]

## 症状

在本机重编 `emulator-cxx` 后 `ctest` 的 `emulator-core-config-toggle` 红：

```
off: 配置导出里没有 display:ScreenMode
```

同一份源码此前（旧二进制）是绿的 ⇒ 不是字面意义的"开关坏了"，而是**测试依赖环境状态**。

## 根因

`config_toggle.cmake` 的 OFF 用例依赖"初始值 = 1，所以点 OFF（目标 0）一定走更换路径写键"。
可是玩家配置根按规范解析到**本地**应用数据（`~/.local/share/Eull...`），本机没有这个文件；
缺省表里 `display:ScreenMode` 不在引擎内建默认值里 ⇒ **键根本不存在**。点 OFF 时目标 0 与"缺省"相同，
引擎不建键 ⇒ 导出里没有这个键 ⇒ 断言红。旧二进制之所以绿，是因为它从**旧路径**（漫游 `~/.config`）读到了
一份 `ScreenMode=0` 的玩家文件（那份文件不能当测试前提）。

## 修法

两个方向都让初始值与点击目标相反，用 `--config <临时种子>` 显式给玩家文件：
- ON 用例：种子 `ScreenMode=0` ⇒ 点击必须写成 1；
- OFF 用例：种子 `ScreenMode=1` ⇒ 点击必须写成 0。
回读仍是 `--dump-config` 的差分，判据（OFF = 0、ON ≠ OFF）不变。种子文件落在 CTest 的 `OUTDIR` 下，
不碰真实玩家配置。

## 验证

- `ctest -R emulator-core-config-toggle` 绿；
- 全量 `ctest` 21/21 绿；
- 反向检查：把种子初值改回"依赖本机"（或删掉 `--config`）⇒ 在本机（无玩家文件）当场红。
