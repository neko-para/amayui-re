# saves/ —— 复现用存档（**用户自维护**）

## 维护策略（2026-11-18）

- `saves/` 里的存档是**用户自维护的复现素材**：只由用户构造 / 更新 / 删除；agent ⛔ 不新建、不修改、不重命名、不删除这里的任何文件。
- 需要跑存档时，把用到的文件**复制**到本次运行私有的 `--appdata` 的 `Local/Eushully/天結いキャッスルマイスター/SAVE/`，⛔ 不就地写（见下节；复制错一层是本目录最容易踩的坑）。
- 基于存档提出的缺陷必须写清：`SAVE0N.DAT`（哪个文件）/ 哪一屏哪一槽 / 操作序列 / 读数；⛔ 不写“某个存档”。
- **新增 / 替换存档要入库**：`.gitattributes` 已把 `*.DAT` / `*.STH` 配成 LFS ⇒ `git add saves/...` 即可。
  ⛔ 只把存档留在 `.tmp/`（gitignore）等于没加 —— 下一个人只能重新造。

## 存档表

| 文件 | 内容 | 备注 |
|--|--|--|
| `SAVE00.DAT` | SN0000 第一幕 | |
| `SAVE01.DAT` | SN0000 最后一幕 | 点击推进可测试大章节转场动画 |
| `SAVE02.DAT` | SC0000 第一幕 | |
| `SAVE03.DAT` | SC0000 中间幕 | 点击推进可测试 matrix 效果和条纹翻转转场效果 |
| `SAVE04.DAT` | SC0000 第一个地图前的对话 | 点击可进入战斗地图（询问是否阅读教程） |
| `SAVE05.DAT` | SC0000 第一个地图中的一个场景 | 点击 740,355 的宝藏会有一串加载逻辑；载入后落 `FIELD.BIN` |
| `SAVE06.DAT` | SC0000 第一个地图中结束时的 ADV | 等加载稳定后点击，重复两次，会回到外部 ADV 界面 |
| `SAVE07.DAT` | SC0000 第二个地图中即将战斗 | 点击右上角的回合结束按钮，敌怪会来触发战斗动画，点击战斗可触发后续 |
| `SAVE08.DAT` | **未登记** | 用途待用户补；agent ⛔ 不猜 |
| `SAVE09.DAT` | **未登记** | 同上 |
| `SAVE10.DAT` | **FIELD**（地图 `0xb`） | 2026-11-25 读数：载入后 `FIELD.BIN` 就绪、21 个 live 槽（`--plugin-param slot=10`） |
| `SAVE11.DAT` | **未登记** | 同上 |
| `SAVE99.DAT` | **未登记** | 用途待用户补 |
| `SAVE.DAT` | 槽位索引 + **持久化页号** | 与 `SAVE<NN>.DAT` 分开；读档界面进屏时 `SAVE.BIN` 从这里把页号读回（`load-int (global-int 138e)`） |
| `RT.DAT` | 引擎运行期数据 | 语义未取证；复制时原样带上 |

- 每个 `SAVE<NN>.DAT` 都要配一份同名 `.STH`（缩略图）；**成对复制**，⛔ 只拷 `.DAT`。
- 文件大小**不是**判据（同一场景的存档大小会随内容变）；⛔ 不要按大小推“空槽 / 有内容”。

## `--appdata` 与目录结构（★ 复制错一层 = 读档界面点行没反应）

`--appdata` 指的是 **`%AppData%` 这一层**（不是存档目录本身）。VFS 把 guest 的
`CSIDL_LOCAL_APPDATA` 映射到 `<--appdata>/Local`，引擎再自己拼 `Eushully\<标题>\SAVE`
（`docs/03-emulator/save-format.md` §外部组装）：

```
<--appdata>/Local/Eushully/天結いキャッスルマイスター/SAVE/
    SAVE.DAT  RT.DAT  SAVE00.DAT  SAVE00.STH  SAVE01.DAT  SAVE01.STH  …
```

- **推荐**：用 `apps/emulator-lift/scripts/clean-appdata.sh <appdata-根>`（bash）一次建好并复制；
  它把 `saves/` 的 `SAVE*.DAT` / `SAVE*.STH` / `SAVE.DAT` / `RT.DAT` 放进上面那个 `SAVE/`，并删掉 `$$AGE.TMP` / `SAVE.BAK`。
- Windows `pwsh` 等价：

  ```powershell
  $root='E:\Projects\amayui-re'; $dst="$root\.tmp\my-run\appdata"
  $game="$dst\Local\Eushully\天結いキャッスルマイスター\SAVE"
  New-Item -ItemType Directory -Path $game -Force | Out-Null
  Copy-Item "$root\saves\SAVE*.DAT","$root\saves\SAVE*.STH","$root\saves\SAVE.DAT","$root\saves\RT.DAT" -Destination $game
  ```

- **反例（2026-11-24/25 实测，代价六次 run）**：把 `SAVE*.DAT` 直接放在 `<--appdata>/` 根目录
  ⇒ 引擎会新建**空的**嵌套 `SAVE/`、读档界面能开能翻页（读不到 `SAVE.DAT` 的持久页号 ⇒ 从默认页 0 开始），
  但**点行不弹“要读取吗？”确认框** ⇒ 插件停在 `load-save: gate timeout (load:sbunki)`（或 `load:save`）。
  ★ 这**不是**点击被吞、也**不是**页基算错；先查这一层目录。
  （同一份 `saves/` 换成上面的嵌套落点后，2026-11-25 的 `slot=10` run 直接到 `FIELD.BIN`；
  headless 与 `--present window` 的读数逐行相同 ⇒ 与 present 模式无关。）

## 槽位 ↔ 页 / 行

- 读档界面每页 **10 行**：`page = floor(slot/10)`、`row = slot % 10`。
  页号是**可读的 guest int global `0x138e`**（0 基，环形 0..89）；页 1 = `010..019`。
- 进屏页由 `SAVE.DAT` **持久化**（`SAVE.BIN` 第 11 行 `load-int (global-int 138e)` 读回）
  ⇒ 同一份 `saves/` 在不同 appdata 里可能从不同页开始；插件必须按读到的页号翻页（`loadSave` 已这么做）。
- 行中心的 y（页内）：`{0:91, 1:150, 2:209, 3:269, 4:328, 5:388, 6:447, 7:507, 8:567, 9:626}`，
  x = 300（列表行）。读数与出处：`apps/emulator-lift/scripts/plugins/lib/save.js` 头注释。

## 怎么查

- 存档二进制结构（头 / 字段 / 与引擎的交互）：`docs/03-emulator/save-format.md`。
- 载入链（点 LoadData / 翻页 / 点行 / 确认框的门与坐标）：`apps/emulator-lift/scripts/plugins/lib/save.js` 头注释。
- 一次可复跑的端到端（headless，`slot=15`，`run=done` 且点名 `FIELD`）：CTest `plugin/load-save-e2e`
  （`apps/emulator-lift/tests/plugin_e2e.cpp` ⑥）。
