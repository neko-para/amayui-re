# 错误通道 / 异常三支 / 对外接口槽 45（语料内可继续取证）

- id: REQ-01M68PPEDA9VPRXYDNCXVEETV9
- type: req
- status: dropped
- parent: REQ-01M674XHSX2HSFWSKDWZV2C63D
- dropped_reason: 残差清单式节点：audit.json 的残差已带 what/next，需求树不重复登记；无 audit 归宿的条目已并入 REQ-01M694VKD97ERC5ZBHDA4AHQV2。
- tags: [spec residual]

## 目标

收敛「错误通道 / 异常」这一族在批 2（`REQ-01M674XHSX2HSFWSKDWZV2C63D`）收敛时留下的**语料内可继续取证**的缺口。

## 具体项（入口 EA 已给）

1. **通道过滤器**（通道 `+0x410` / `+0x420`）的安装者与输出句柄来源：入口 `0x497620` / `0x4034C0`（~45 行）。对应残差 `RES-OPS-ERRCH-FILTER`。
2. **显示消息型异常三支**（`sub_415640` 的 catch 段，>200 行，R3 预算外）：异常被接住后按什么分三支、各自改什么。
3. **`0x13f` 越界文本对齐**（`0x42FBAC`，~20 行）：查位也有越界文本，本轮只做了置位 / 清位两条。
4. **对外接口表第 45 槽的外部调用方**：入口 `0x4765C0`（无参查询、按槽号判非 0）；语料内直接调用者 0 ⇒ 需要外部模块反汇编或运行时 trace。对应残差 `RES-OPS-142-CELL-NAME`。

## 收口判据

- 1 / 3 能静态判 ⇒ 写进 `60-ops-numeric` 的错误通道节并删对应残差；
- 2 判不动就按 R3 继续拆（把入口 EA 写进子单）；
- 4 若外部模块不可得 ⇒ 保持 `unknown` 并把"采什么"写死。
