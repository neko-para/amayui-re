# 🐞S3 dumpBmp 不检查 fwrite/fclose ⇒ 短写/磁盘满仍报成功（screenshot 会假成功）

- id: REQ-01M7HZ7A7E2AJFZE9XJC2JA4DX
- type: req
- status: open
- parent: REQ-01M7HC1WJ9X0QCQ51Q9F01988W
- tags: [emulator-lift, control-plane]

## 现象

`SurfaceArena::dumpBmp`（`src/runtime/surface.cpp` 的写盘段）**不检查 `fwrite` / `fclose` 的返回值**，
函数在"文件打开成功"之后就 `return true` ⇒ **短写 / 磁盘满 / 配额打满时仍然报成功**。

之前的消费者只有 `--dump-frame`（写失败也只是少一张图）。控制层的 `screenshot` 把它变成**对外契约**：
agent 会拿"截图成功"当作"我看到了这一帧"的证据 ⇒ 假成功会让后面的判断全错。

## 根因

写路径只判了 `fopen`：
- 每行 `std::fwrite(row.data(), 1, row.size(), f);` 的返回值被丢弃；
- `std::fclose(f)` 的返回值被丢弃（缓冲区里的尾巴是在 `fclose` 才落盘的 ⇒ **只有它能抓到短写**）。

## 判据（能红）

1. 正常路径：`screenshot <path>` 写出的 BMP 字节数 == 期望（`shasum` 与同一暂停点第二次截图相同）。**这条今天就绿**（对照用）。
2. 失败路径：把目标指到一个**写不满**的位置（例：`/dev/full`；或先 `ulimit -f` 把文件大小压到 1 个块；Windows 用只读介质 / 配额）
   ⇒ `screenshot` 必须回 `ok:false` + `error.code:"io"`，⛔ 不许回 `ok:true`。
3. `--dump-frame` 同样：写失败 ⇒ 日志里有一行失败（⛔ 静默）。

## 修法（最小）

`dumpBmp` 里逐行判 `fwrite` 返回值 == `row.size()`，末尾判 `fclose` == 0；任一失败 ⇒ 填 err 并 `return false`。
调用点（`dumpLastFrame`）已经会把它转成 false + err ⇒ 不需要改签名。

## 边界

不是"BMP 格式写错"（那由 `hash_still_matches` 与像素对比管）；本单只收**写盘失败被吞掉**这一类。

## 来源

`REQ-01M7HX0CJ202MJ4PZXW80VFA0J`（M0 控制层）的 S2 实现过程中发现；按任务书当轮不修，只登记。
