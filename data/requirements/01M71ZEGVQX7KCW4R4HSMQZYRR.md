# 🐞S2 corpus 写入口把「本机私有覆盖」写进入库清单：macOS 上跑一次 set --write 就把 Windows 的 roots 冲掉

- id: REQ-01M71ZEGVQX7KCW4R4HSMQZYRR
- type: bug
- status: done
- parent: REQ-01M4FEFA9RRP5TKVWH351NV1RD
- verify: tools/test/corpus-manifest.test.mjs#★ 本机私有覆盖（assets.local.json）不许被写进入库清单
- repro: tools/test/corpus-manifest.test.mjs#★ 本机私有覆盖（assets.local.json）不许被写进入库清单
- severity: S2
- tags: [corpus, manifest, cross-platform]

## 现象（2026-11-10，macOS arm64）

本机用 `corpus/assets.local.json` 把 `oldRepo` / `gameInstall` 指到 macOS 路径（设计如此：入库清单留 Windows 路径，
本机靠私有覆盖）。在这种机器上跑一次**写入口**（`pnpm tools corpus set binary/age-sectfix '…' --write`）之后：

```
- "oldRepo": "E:\\Games\\Eushully\\天結"
+ "oldRepo": "/Users/nekosu/Documents/Projects/amayui-cn"
- "gameInstall": "E:\\Games\\Eushully\\天結いキャッスルマイスター"
+ "gameInstall": "/Users/nekosu/Documents/Projects/amayui-cn/raw"
```

⇒ 一旦提交，**Windows 侧（主平台）全部路径失效**。本机上是被用户当场发现并制止的。

## 根因（已核）

`loadManifest()` 把本机私有覆盖**合并进** `manifest.roots`（解析需要，这一半是对的）；
`saveManifest()` 直接把这个合并后的对象 `canonicalStringify` 落盘 ⇒ **本机覆盖被写进入库文件**。
`assets.local.json` 的设计前提（"只覆盖 roots、条目是仓库事实、本机不许改写"）在**写路径**上没有任何守卫。

## 处置（2026-11-10）

- `saveManifest` 落盘前：被本机覆盖的键**还原成盘上的值**、只在本机覆盖里新增的键**删掉**；
  只有 `opts.rootsPatch` **显式点名**的键才写（`--set-root <名> <路径>` 非 `--local` 那条路传它）。
  校验仍按**本机有效视图**（合并后）跑，落盘写**入库视图**。
- 守卫：`tools/test/corpus-manifest.test.mjs#★ 本机私有覆盖（assets.local.json）不许被写进入库清单`
  —— 入库清单里的 root 必须等于盘上的值 / 只在本机覆盖里的键不许落盘 / 条目改动照常写 / `rootsPatch` 点名的键要写。
- 文档：`repo:corpus/assets.md` 的「本机私有覆盖」段补"覆盖永远不会被写进入库清单"。

## 复现（修前）

1. 在 `corpus/assets.local.json` 放一条与入库清单不同的 `roots.oldRepo`；
2. 跑任意写入口（`corpus set <id> '{…}' --write`）；
3. `git diff corpus/assets.json` ⇒ `roots.oldRepo` 已被改成本机路径。
