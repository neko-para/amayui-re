# 系统容器样本入库 + FMT-SAVE-SYSTEM 逐段更正（32 字节缺段 / 字数语义 / 键尾 3 字节）

- id: REQ-01M5Z9RC53NA3GNB9KS1EAGVX7
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/fixtures-system-container.assets.test.mjs#系统容器 SAVE.DAT：分段恰好消费到 EOF

## 做完了什么

两份**真实引擎**写出的系统容器进了 `corpus/fixtures/`（`SAVE.DAT` 脚本键表、`RT.DAT` 文本库），
`fixtures` 模型新增 `containers[]`（不属任何槽的文件：身份 = 文件名 + `what`），配 `--add-container` / `--remove-container`
与"一个文件只能有一个身份"的写入口守卫。

## 为什么值得

`FMT-SAVE-SYSTEM` 之前只有**静态**证据（写侧 + 读侧两处汇编互证），从没被字节检验过。第一次逐段走查就抓到：

- **规范缺一段**：段 5「末尾一个 0 字」之后还有 **32 字节**（8 个 0 字），两份独立样本一致、且落在内层 CRC 覆盖的主体里；
- 段 3 的 `字数` = `ceil(两条串字节数/4) + 1`（多 1 字，不是"补齐到字边界"那么随意）；
- 段 2 的 12 字节键里有 **3 字节常量尾**（`00 51 00`），段 3 的键没有；
- 头 `+240` 是**解压后**长度（原文写"解压后（未变换）"把两个量混了）；魔数后 2 字符是**容器种类**（`SD` / `RT`），
  而同一目录里的 `RT.DAT` 族标识是 `S3` ⇒ 族标识是**逐容器**的。
- 解密结果比 `12 + 压缩字节数` 长（零填充，余量 37~38 字节）—— 规范原来没写。

## 判据

`tools/lib/save-container.mjs`（按规范文字写的解码器）+ `tools/test/fixtures-system-container.assets.test.mjs`
（六条：信封每一步、分段恰好消费到 EOF、键形态、命名键**不在**系统容器里、**槽容器套同一套分段必须垮**、`RT.DAT` 是另一种容器）。

★ 它是 `@env assets`（样本走 LFS）⇒ **撑不起 `reviewed`**：这是口径（二进制样本的字节级判据进不了默认门禁），不是缺陷。
