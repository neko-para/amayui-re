// tests/plugin_fixtures/recipe-driver.js —— C3 parity 的**插件入口**（`recipe-driver-parity` 夹具加载它）
//
// 合同：`docs/03-emulator/plugin-runtime.md` §5 C3（`:1162-1206`）；driver 本体 =
//       `scripts/plugins/lib/recipe.js`（`runRecipe`），本文件只做入口与数据接线。
//
// ★ 为什么有两个 import（而且都必须是**相对**路径）：宿主 QJS 的 import 根 = **入口文件所在目录**
//   （`src/ui/script_quickjs.cpp` 的 `moduleNormalize`，`:1223-1256`；绝对路径 / bare / `data:` 一律拒），
//   所以 `tests/plugin_fixtures/*` **不能**直接 import `scripts/plugins/lib/*`。parity 夹具
//   （`tests/recipe_driver_parity.cpp`）因此准备一个 **run 私有临时目录**：
//     · `recipe.js`        = `scripts/plugins/lib/recipe.js` 的**逐字节副本**（⛔ 不手抄第二份实现）
//     · `recipe-steps.js`  = `src/runtime/recipe_json.cpp` 的 `stepsJson()` 输出（`export default …`）
//   并把本文件当**入口源码**装载、`origin` 指到那个临时目录 ⇒ 下面两条相对 import 落在它里面。
//
// ★ `export default` 必须**是** `{ start, onTrigger }`（宿主只读 default；`start` 与 `onTrigger`
//   必须成对，否则装载失败并点名 —— `plugin/module-*` 用例的同一份契约）。
// ★ ⛔ 不在本文件里加任何判定：门 / 超时 / 完成度全在 `runRecipe` 与宿主的 `TriggerRunner` 里。
import { runRecipe } from "./recipe.js";
import plan from "./recipe-steps.js";

export default runRecipe(plan);
