# 配置子系统：spec（默认表 / 玩家文件 / 保存 / 脚本访问）+ emulator-new 落地

- id: REQ-01M5CE7G98YHMK1JXFC4JWM573
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-config.test.mjs#★ 默认表：整数/字符串/列表/SAVEVERSION 拆分/未知键响亮失败
- tags: [emulator, engine, spec]

## 判据

- spec：`55-config.md` 五条规则（库 / 默认表 / 玩家文件 / 保存 / 脚本访问）落盘，`spec validate` 绿。
- 实现：`apps/emulator-new` 从真实索引取 51 条发行默认，叠加可选 `SYS4REG.INI`，`--init-config` 可写出默认文件；四条配置 opcode 有守卫。
- 守卫：pure 单测覆盖默认表归一化 / INI 覆盖 / 保存格式 / 四条 opcode；assets 覆盖真实索引与启动阶段；变异条目证明会红。

## 范围

- 只做配置子系统；`0x2f6` 之后的启动前段与显示设备消费不在本单。
- `CORE-FS` 仍显式延后；玩家文件路径的宿主根由 `--config` 显式给，默认读安装根（不写）。

## 不做什么

- 不把 `SYS4REG.INI` 当成游戏数据入库；没有样本就不造样本。
- 不实现引用族操作数（类型 6/8/12/14）的键取下标 —— 先响亮失败，等 CORE-POOL 引用族。
