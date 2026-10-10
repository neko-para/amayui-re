# int 池容量：3,335,068 的来路与 7,375,580→7,375,836 的 +256 增长项未查明

- id: REQ-01M4GSWZXXFV2Q2Q88QQC68TKB
- type: req
- status: done
- parent: REQ-01M48DJ3PK48CXQXAV1M3EEQNR
- order: 14
- verify: packages/age-format/test/alf.test.mjs#★★ 解压 TOC 偏移 `0x10` = `3,335,068`（8 条归档记录各一处、落在归档记录的文件名字段内）；**盘上压缩件**的 `0x10` 不是它
- tags: [engine ledger]

## 两问都已收口（2026-10-10）

结论落在台账，各一条（`accepted`，`self` 锚 = 本仓新增守卫）：

* 问题① ⇒ **`KN-01M4H0PBBM7Z4S1B3D1X022H2E`**（subject `SYS4INI.BIN+0x10/decompressed-toc-archive-record-tail-3335068`）
* 问题② ⇒ **`KN-01M4H0P4BW1V2H0Y362V603D5W`**（subject `Engine+0x5D7E8/alt-capacity-7375836-plus-256`）

### ① `3,335,068` 的来路 = **`SYS4INI.BIN` 的 LZSS 解压 TOC 偏移 `0x10`**
旧仓 `analysis/engine-capabilities.json` 的 gaps 条目 + `tickets/T-0107/changes-c107.md` §4.3 **逐字**就是
「容量读出来是 3,335,068 dword（13.3 MB，**取自解压 TOC 偏移 0x10**）」⇒ 来路**钉住了**（它自己写明了读法）。

* 本机逐字复核：解压 TOC 长 `1,692,004`（= 件头声明的 `orig`；盘上件 `0x12C` 起才是 S4SECTHDR + LZSS），
  `toc+0x10` = **`3,335,068`**（字节 `9C E3 32 00`），且以步长 `0x100` 出现 **8 次**（`0x10/0x110/…/0x710`，
  = 归档数 8，每条归档记录一处）。
* 它是**归档记录里文件名字段内部的尾巴**（归档记录自 `0x04` 起、宽 `arcEntry = 256`；记录 #0 名字 = `DATA1.ALF`），
  **不是池容量**。
* ★★ **上一轮那条否定被推翻**：它拿「**盘上压缩件**全件 446,555 个 4 字节窗口命中 0 次」去证伪 —— 但旧仓读的是
  **解压后**的 TOC，**两者不是同一段字节** ⇒ 那次否定**核错了件**（盘上件 `0x10` = `0x62838383`）。

### ② `7,375,836` 与 `7,375,580` 的 `+256` = **两个不同来源的量被并置**（既非低字节口径，也非"另一格"）
* 格子本身 = 安装件偏移 `0x114` = `dc 8a 70 00` = **7,375,580**（`0x708ADC`，全件只此一处；
  旧仓 `tickets/T-0102/white-report.md` 直接量同一字节亦为此值）。
* `7,375,836` 的十六进制**恰好是 `0x708BDC` = 格子 `+0x100`**，且该值在安装件里**命中 0 次**。
* 旧仓 `tickets/T-0187/recheck.md` §5.10 把**格子的十六进制**与**另一个数的十进制**并排写成「同值」，
  并注明后者出处 = CLI 的 `MaxGlobalSlots`（= 该内存区段大小 ÷4）⇒ 两个不同的量。
* 为什么**不可能**恒等（逐字 EA）：int 池的分配式 = `VirtualAlloc(..., 4*(Engine[0x5D7E8] + rand()%10000h) + 4, …)`：
  `0x4151B4 call _rand`（`and edi,8000FFFFh`）→ `0x4151DF mov eax,[ebx+5D7E8h]` → `0x4151E7 add eax,edi`
  → `0x4151EE lea ecx,ds:4[eax*4]` → `0x4151FD call VirtualAlloc` ⇒ 区段被 `rand()` 随机撑大 `0..0xFFFF` 个 dword
  ⇒「区段大小 ÷4」每次运行都可能不同。

## 判据 / 收口凭据
`verify` = 本单新增的守卫用例（见文件头 `verify`；同一文件里另有第二条钉 `0x114` 与 `+0x100` 的关系）。
两条都**当场从安装件字节/解压 TOC 重算**，不采信任何二手数字。
