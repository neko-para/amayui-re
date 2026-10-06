# ⬜ 指令区之后的三张表（label / message / call）结构未建模

- id: REQ-01M48FRMKME8VERSH42M7MQMYP
- type: req
- status: open
- parent: REQ-01M48DJ3PK48CXQXAV1M3EEQNR
- order: 14
- tags: [engine, emulator]

# ⬜ 指令区之后的三张表（`table_1/2/3` = label / message / call）结构未建模

- id: REQ-01M48G0000000000000000001
- type: req
- status: open
- parent: REQ-01M48DJ3PK48CXQXAV1M3EEQNR
- order: 14
- tags: [engine, emulator]

## 已知（机械可复核）
* 三张表紧跟在指令区之后，各自的 (长度, 偏移) 在头部第 +36/+40、+44/+48、+52/+56 字节处
  （v4 头部长 60；字段序见 `packages/age-format/src/asm/types.mts` 的 `FIELD_NAMES`）。
* ★ **指令区的终点由"最早出现的数据块"决定**，不只是三张表：type-2（字符串）与 `0x64` 的数组块
  也会把终点前压（实测：不这么做会撞上一堆 `opcode=0`）⇒ 三张表与指令区**不是简单相邻**。
* 旧仓口径（待核，**不许当事实**）：三张全局 ip 表 = 0x71 消息表 / 0x3 call-script 表 / 0x8F call 表；
  帧内 `+0x54..+0x68` 是三组 (len, ptr) 指向这三张表；`+0x6C state_6C` 是当前 ip 在表 1/2 的下标。

## 待做
1. 三张表各自的**条目结构**（元素宽度、首项是什么、下标怎么算）—— 从访问点机械推。
2. `帧+0x54..+0x68` 三组 (len, ptr) 与三张表的**逐一对应**（别按顺序猜）。
3. label 表引用的 **label 编号口径**（反汇编器里 label 是**绝对文件偏移**：`headerLen + rawData*4`）。

## 判据
结论按准入门登记进 `data/ledger/`（锚到语料 EA / 守卫），并把本单 `verify` 指到对应守卫。
