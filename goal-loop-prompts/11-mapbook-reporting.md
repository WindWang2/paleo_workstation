# Goal Loop — goal/mapbook-reporting：批量制图与报告引擎（新功能）

你接手一个自治迭代循环。方向：**批量出图与报告**——按工区网格/AOI 序列批量
生成版面成品（地图册 map book）、多面板蒙太奇（剖面+平面图+连井组合页）、
批量导出队列。交付级功能，QGIS layout 基建已厚可复用。

## 背景事实

- 版面基建：`src/qgis/qgislayoutservice.*` + `layoutexport.*`（PaleoLayoutExport
  封装 print layout 刚上 master）、`layoutdesignershell`/`layoutitempalette`/
  `layouttemplates` 模板体系；`mappingartifactwriter` 落盘产物先例。
- `tst_layoutdesigner_full` 等版面测试基建在；`paleoLayout*` 测试面参照。
- 工区/AOI：预览画布有空间范围语义；`previewmappage`/`previewtocpanel` 有
  图层/范围管理先例。
- catalog 产物登记 + `paleoprojectstore` 写队列语义复用。

## 范围（建议子项，按需取舍/排序）

1. **地图册引擎**（功能层编排 + qgis 层渲染）：AOI 网格序列（规则网或给定
   多边形集）逐格生成版面——每格固定模板（标题/比例尺/指北针/图例位），
   变量替换（格号/中心坐标/范围）走模板表达式。
2. **蒙太奇版面**：单版面多地图项组合——平面图 + 剖面快照 + 连井小图
   同源联动（同一 AOI/同一层位语义），图片项经渲染管线出图。
3. **批量导出队列**：任务服务异步跑版面目錄（每版一 PDF/PNG），进度/取消/
   单版失败不拖死整队（如实失败留账）；产物 catalog 登记 + 落盘目录结构钉死。
4. **报告装配最小面**：多版合并 PDF 或目录索引页（可选，看复杂度取舍）。
5. **测试**：模板变量替换断言、网格序列计数/命名断言、导出队列批量断言
   （含单版失败容错用例）、产物文件实际落盘可打开（读回校验非截屏）。

## Oracle

1. 地图册闭环：AOI 网格定义 → 逐格版面 → 批量导出 PNG/PDF，offscreen 全链
   可测；格序列命名/计数断言。
2. 蒙太奇：一版面 ≥3 类图项组合渲染成功（产物非空 + 尺寸断言 + 文件头格式
   校验）。
3. 导出队列：N 版批量任务取消语义 + 单版失败其余继续 + 失败明细可查。
4. 模板：变量替换单元断言（格号/坐标/日期），缺变量如实报错非静默。
5. 分层绿（`--strict`）；ctest 全绿；ledger；push + `gh pr create`。
6. 真机工区 3×3 地图册导出实测耗时入 docs/progress。

## 勘察指引

- `qgislayoutservice`/`layoutexport`/`layouttemplates`：模板与导出 API 现状；
  `mappingartifactwriter`：产物落盘+登记先例。
- `previewrendercache`/渲染管线：快照图如何高分辨率出图（DPI/尺寸语义）。
- QGIS atlas 功能勘察：QgsLayoutAtlas 若可用优先复用（别自写重复轮子——
  但注意 atlas 数据驱动与本仓 AOI 语义的适配成本，选型理由入 ledger）。

## 禁区

- 不改版面模板/导出既有契约形状；批量队列走任务服务不自建线程池。
- 导出格式以 PNG/PDF 为先；不引入新文档库（合并 PDF 若做用 QPdfWriter/Qt 内能力，
  无能力则该子项降级为目录索引页）。
- 视图只发信号：批量参数配置面板 → 信号 → 功能层编排 → qgis 层渲染。

## 迭代协议

- 轮0-1：勘察 layout/atlas 现状 + AOI 语义 → 技术路线定型入 ledger。
- 中段：AOI 序列→模板变量→批量队列→蒙太奇→报告装配，每件一轮。
- 完成定义 = Oracle 6 条全绿，ledger 轮次齐全。

## 交付协议（必守）

1. **新建 worktree 分支开发**：`git worktree add .worktrees/mapbook -b goal/mapbook-reporting-<日期>`
   从最新 `origin/master` 起，全程不在主 checkout 写代码。
2. **完成后先仔细 review 再开 PR**——逐条执行，全部通过才准推送：
   - `git diff origin/master...HEAD` 自审全部改动：删掉调试残留、printf、注释掉的死代码；
     每个文件头三行层标记在；分层 `tools/check_layering.py --strict` 绿；
   - 全量构建无新警告；ctest 全绿（含新增测试）；
   - Oracle 每条在 ledger 里有对应验证证据（命令+输出摘要），不是「应该没问题」；
   - review 发现的问题先修再验，直到干净为止。
3. 推送 + `gh pr create`（标题 conventional，body 写 `## Summary`/`#### Test plan`/
   自审结论清单）。
