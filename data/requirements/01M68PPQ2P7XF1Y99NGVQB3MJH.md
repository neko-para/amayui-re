# show-logo 复位路径的运行时 trace（S2）

- id: REQ-01M68PPQ2P7XF1Y99NGVQB3MJH
- type: req
- status: dropped
- parent: REQ-01M674XHSX2HSFWSKDWZV2C63D
- dropped_reason: 残差清单式节点：audit.json 的残差已带 what/next，需求树不重复登记；无 audit 归宿的条目已并入 REQ-01M694VKD97ERC5ZBHDA4AHQV2。
- tags: [spec residual]

## 目标

批 2 只剩这一条**必须运行时 trace** 的 S2（其余都能静态判）。

## 具体项

- **show-logo 复位路径的真实输入**：复位例程（`sub_428A60`，就是 `0x9` 的处理体）在语料内的非脚本入口只有两个（窗口过程 case 16/WM_CLOSE `0x4B95FF`→`sub_4057B0`→`0x405A7D`、对外接口 `sub_476590`），**没有**"回标题 / 打开菜单"的内部路径直接调它。
- 要采：真实输入走"回标题 / 打开菜单"⇒ 复位例程是否被调、之后 `0x130` 的读值。
- 采到后改哪：`CORE-OPS-SHOW-LOGO-130` 的"复位由谁触发"那一句 + 残差 `RES-OPS-SHOW-LOGO-130-RESET-REACH`。
