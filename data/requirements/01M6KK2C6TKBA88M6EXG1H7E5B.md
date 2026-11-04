# PE 载入剩余两项：填充区间的严格校验 / 导入表两张原始 RVA 的诊断

- id: REQ-01M6KK2C6TKBA88M6EXG1H7E5B
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift, m2, loader]

## 一句话
PE 载入路径剩下的两个边角：① `SizeOfRawData` 声明到文件外的**填充字节**仍被接受（有意防误杀，但没进 `SCOPE.md`，也没第二道口）；
② `PeImportInfo` 不保留 `OriginalFirstThunk` / `FirstThunk` 的原始 RVA ⇒ A5 的缺口诊断指不出"是哪张表"。

## 背景（2026-11-04 由 `REQ-01M6JFZFCQ956CBFMKKQ37PCYK` 的收口代理点名）
那次收口把「半载入」改成了「带原因拒绝」，并顺手修掉了两处：
- `loadPeFile` 用 `std::ftell`（Windows 的 `long` 是 32 位）⇒ 改成 `_ftelli64` / `ftello` 的 64 位取长，并对 >4 GiB 的文件明确拒绝；
- `rvaToOffset` 的「头部区域直接按 RVA 当偏移」分支现在要求 `rva < 文件长度`（否则只调 `parsePeImage`、
  不走 `mapPeImage` 的消费方 —— 例如 `version!GetFileVersionInfo*` 读版本资源 —— 会"截断头也解析成功"）。

剩下的两条**没做**，因为都不是"改一行就安全"：

## ① 填充区间的严格校验
- 现在的判据是「**要拷的字节**（`min(SizeOfRawData, VirtualSize)`）不许越界」。`SizeOfRawData` 声称到文件外、
  但超出 `VirtualSize` 的那部分（纯填充）**被接受**。
- 之所以这样：真实镜像里 `SizeOfRawData > VirtualSize` 且超出文件尾的形态存在（对齐/工具产物），
  一刀切会把正常 DLL 判成坏件 —— 而 M2 要对 `--game-dir` 里的**任意** DLL 调版本读取。
- 要做的：先**取证**（对安装树里所有 PE 统计"`PointerToRawData + SizeOfRawData > 文件长度`"的镜像有几份、
  差多少字节），再决定是"拒绝"还是"接受 + 归入 `SCOPE.md` 的已登记偏差"。⛔ 不许凭感觉加严。

## ② 导入表诊断的身份
- `PeImportInfo` 目前只留模块名与函数名 ⇒ A5 的"哪条导入没实现"能说清，但"坏在哪张表"说不清。
- 要做的：保留 `OriginalFirstThunk` / `FirstThunk` 的**原始 RVA**（不是换算后的偏移），
  让 `loader` / `base-coverage` 的诊断能打印 `descriptor #i (OFT=0x… FT=0x… module=…)`。

## 判据
- ①：一条 assets 档用例（对安装树里的 PE 做统计并断言"被接受的那些确实都在白名单形态里"），或一条 `SCOPE.md` 偏差条目 + 一条 pure 夹具钉住"我们接受哪种形态"；
- ②：`loader` 或 `base-coverage` 的失败信息里出现两张表的原始 RVA，且有用例断言这两个字段非 0（真镜像上）。

## 边界
- 本单**不改**已经收口的那些拒绝逻辑（坏节 / 坏 thunk / 坏名字 / IAT 越界 / 零字节半载入）。
- 严格 raw 区间若最终决定"加严"，必须同时给一条「合法虚拟尾部 + 填充」的**反向用例**（那个已有：`pe_loader.cpp#virtual-tail-ok`）。
