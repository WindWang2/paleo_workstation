# 2026-10-24 导入容错与文件知识图谱

## 工程创建与资料隔离

`ProjectSetupPreview.errors` 只表达阻断整个工程的配置错误（例如无效工程 CRS、取消扫描）。
`sourceProblems` 以原始来源路径记录文件、目录和图件套件的问题；确认页逐项说明，创建与资料设置跳过这些项，继续导入其他资料。
来源问题保存在工程 `sourceStats.sourceProblems`，完成通知报告部分成功。修正后可在“工程资料设置”重新识别与导入，沿用现有去重与逐行回滚。
目录、清单、工程坐标配置失败仍走原来的工程级失败与回滚；用户取消仍丢弃未提交的整批暂存。

- 井点和分层表没有有效记录时不进入待导入列表；有效表中的逐行问题继续通过解析报告展示。
- 井点源 CRS 独立校验并单独回填向导；不使用层位/边界的坐标声明补足未知的井点源坐标。
- 自动识别到的非米制坐标无法直接作为工程基准时，推迟对应空间资料并说明如何指定基准，其余资料继续。
- 失效来源目录、不可读子目录、无法哈希的文件、缺配套的古地理图按来源报告。
- 图件层位尚未确定时跳过该套图，不阻止创建工程或导入其他图件。
- 多选文件批次保留不存在的显式文件路径，执行时落失败行；不再因同批存在有效文件而静默丢掉该路径。

## 文件图谱实现

复用 `DataGraphWorkflow` / `datagraph` 查询与真实 catalog 关系，继续使用 Qt Graphics View，不引入第二套关系数据库或新的 Web 运行时。
原生 `QSplitter` 提供“节点详情 / 图谱 / 关联关系与操作”三栏；两侧可调宽、收起，中央保留漫游和缩放。
知识图谱使用紧凑椭圆集合与矩形文件节点；力导向布局后按真实连通分量分岛排列，避免无关集合的边交叉穿插。
选中节点突出其直接邻居，淡化其余节点与连线。密集关系边标签在选中后显示，完整关系始终可在右侧和工具提示中查询。
保留搜索、类型/阶段筛选、双击展开、预览、追溯、导出和原生主题令牌。

实现依据：[Qt Graphics View](https://doc.qt.io/qt-6/graphicsview.html) 支持场景选择、图元事件与视图变换；
[QSplitter](https://doc.qt.io/qt-6/qsplitter.html) 提供原生可调分栏与逐栏折叠。
邻域淡化和连通分量排列是本项目的呈现策略，不产生新数据关系。关系图仍只反映已登记归属和血缘，不声称从文档内容抽取了语义知识。

## 验证

独立 worktree 从 `master` / `origin/master` 同一提交 `7d956fd4` 开始，Windows MSVC + 已有 vendored/localdeps QGIS 前缀构建，编译并行上限 8。`paleo` 及六个目标均构建成功。

| 定向回归 | Qt 用例通过 | 跳过 | 结果 |
|---|---:|---:|---|
| tst_import | 67 | 4 | 通过 |
| tst_folderimport | 27 | 0 | 通过 |
| tst_projectsetup | 23 | 4 | 通过 |
| tst_ingestplan | 10 | 0 | 通过 |
| tst_datagraph | 30 | 0 | 通过 |
| tst_datagraphpage | 11 | 0 | 通过 |

共 168 个 Qt 用例通过、8 个条件用例跳过。跳过项为两个缺少 vendored LibreOffice 的 legacy 转换测试、一个未启用的规模探针、一个真实目录导入冒烟和四个未配置真实工区的工程设置测试。
图谱页面初次回归发现截图隐藏/重新显示后的查询等待，以及原生选择先于页面延迟派发的测试时序问题；补足等待后该套完整复跑通过。其他五套已通过，后续仅修改图谱呈现与其测试。

源码闸门通过：layering strict / symbol audit / transitive（指定 QGIS_PREFIX）/ qgs audit strict / qgis spread strict / UI invariants strict / UI size strict / file budget strict / UI tokens strict / i18n / docs drift。
Qgs 直连与散布基线均未增长；图谱构造函数冻结值 351→225，其他规模基线不变。

真实 Qt 页面截图经浅/深主题目视核验：`build/visual-import-graph/data-knowledge-graph-{light,dark}.png`、`data-knowledge-neighborhood-{light,dark}.png`。
覆盖三栏、集合计数、关系表列宽、邻域淡化、主题同步；页面级测试还验证空白处恢复、滚轮缩放及既有返回/预览入口。

重跑入口（依赖环境见 BUILDING.md）：

```powershell
ctest --test-dir build -R '^tst_(projectsetup|datagraph|datagraphpage|folderimport|import|ingestplan)$' --output-on-failure -j1
```

本次没有使用用户真实工区数据，因此不把这些夹具回归等同于所有现场数据格式均已验证。
