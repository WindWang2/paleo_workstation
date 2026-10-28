# Data_service 接入：模型分析与目录后端迁移

## 范围与版本

- 主仓基于本地 master `0b9c0a7a0094c3f56c31910e5e375ffbfe35e90f`，
  沿用已创建的 `codex/add-data-service-submodule` 独立 worktree。
- 子模块初始 pin 为 `d9e123f8`；实施期间上游新增高清地震读取，已同步到
  `b64e1cf609f487a41628554a18719da17eaa0ed9` 后重做验证。
- 接入 pin：`4992620440f9fb1fa30fe520b49b98a74c9efc80`。
- 子模块更新 PR：[changyanyanchang/Data_service#1](https://github.com/changyanyanchang/Data_service/pull/1)，
  从 WindWang2 的 fork 提交，未合并。`.gitmodules` 保留原上游 URL；已在
  全新空仓验证可从原上游 URL 按此 SHA fetch，避免 gitlink 只能在本机解析。

## 分析结论

子模块的 WellDataset/LogSet/IntervalTrack/TimeDepthTable/SeismicVolume 等
模型适合作为类型化资料入口。主项目还拥有显式资产关联、不可变版本、来源
血缘、目录 staging journal、备份恢复、地质编辑和 QGIS 工程契约。

因此先搬共享持久化实现，保留这些契约；不能将子模块的原位曲线编辑直接
绑定到 RAW，也不能在两个库之间未经协调地双写。详细映射与后续迁移顺序见
[DATA_SERVICE_INTEGRATION.md](../DATA_SERVICE_INTEGRATION.md)。

**本批不是全项目读写的完成声明。** 曲线、分层、测斜、时深等生产消费者
尚未全部切换到类型化 DataService。QGIS 工程／空间文件继续由 QGIS/GDAL
执行；没有删除未被子模块覆盖的 DLIS/LIS 和专业格式读口。

## 已实施

1. 主项目通过 `extra-data-service.cmake` 嵌入库目标，关闭子模块 demo、tools、
   examples、tests、install，明确复用已有 SBM target 及匹配头目录。
2. 子模块 ZIP namespace 隔离，避免两份同名符号混链。宿主构建图确认没有
   `pds_sbm` 或 `paleo_data_demo` 目标，有 `paleo_data_service`。
3. 子模块新增标准 C++ 目录记录和 CatalogStorage，拥有四表／meta 的兼容
   schema、SQL 装载和增量写实现。主仓 CatalogStore 只转换值并协调事务、
   schema 门、WAL/FULL、恢复与备份；没有生成第二份权威目录。
4. 保持 rowid 顺序、坏路径过滤、坏 ord 防护、序号吸收、KB/TD 缺失标记、
   嵌套 extra、来源与父版本。只读连接不被子模块偷偷重开为可写。
5. DataService 新增原生只读会话，不做增量 schema 升级；导入／重导入在
   复制和 worker 准备前拒绝，修改由只读 SQLite 连接拒绝。
6. CI checkout 递归初始化子模块；缺失子模块的 configure 验证明确失败并
   给出初始化命令。更新 BUILDING、架构计划、文档索引与 TODOS。
7. file-budget 已有可收紧项 `WellSectionTracksDialog` 冻结值由 257 下调到
   实测 256；未改该 UI 实现，未上调任何基线、阈值或跳过规则。

## 验证

Windows x64、MSVC、Qt 6.11.2，一致的 localdeps/QGIS 环境；构建最多 8 个
worker，测试串行。合成夹具均在临时目录生成，未使用私人工区。

按 BUILDING.md 准备环境后，可复现的目标命令：

```powershell
cmake --build build --target tst_catalogstore tst_datacatalog_sqlite tst_dataservice tst_dataservice_hd --parallel 8
ctest --test-dir build -R '^(tst_catalogstore|tst_datacatalog_sqlite|tst_dataservice|tst_dataservice_hd)$' -j1 --output-on-failure

cmake -S vendor/Data_service -B build/data-service-standalone -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/data-service-standalone --parallel 8
ctest --test-dir build/data-service-standalone -j1 --output-on-failure

cmake --install build/data-service-standalone --prefix <SDK目录>
cmake -S vendor/Data_service/examples/getting_started -B build/data-service-sdk-examples -G Ninja -DCMAKE_BUILD_TYPE=Release '-DCMAKE_PREFIX_PATH=<SDK目录>;<Qt前缀>'
cmake --build build/data-service-sdk-examples --parallel 8
```

主仓测试覆盖旧目录迁移、持久化、恢复与新增的跨模型读取、原始／修改值、
quality、重开、只读、ZIP 共存。上游高清地震合成契约额外在宿主复用引擎的
链接环境运行，区别于子模块自己的 standalone 引擎验证。

源码闸门 29 项中 28 项通过。唯一失败为带 QGIS 头目录的
`layering_transitive`：`ui_qtwidgets_notes: 6 > baseline 5`。从 master 通过
git archive 导出原始 src/tools，以同一 QGIS 头目录复跑，仍为 6/5，重头
指标同为 51 条边／49 TU，本批没有增加。保留原基线，不以豁免或调高数值放行。

独立的 CI 脚本引用、docs_drift、git diff --check 亦已执行。
没有跑全量工作站 GUI 套件，也没有在 Linux 实机编译，不宣称这两项通过。

最终验证结果（子模块 pin `4992620`）：

- 主仓四个目标编译成功，`tst_catalogstore`、`tst_datacatalog_sqlite`、
  `tst_dataservice` 3/3 通过；独立执行 `tst_dataservice_hd` 1/1 通过。
- 子模块完整独立构建成功，18/18 CTest 套件通过。
- SDK 安装成功，包含新增高清地震案例在内的十个 getting-started 示例全部编译。
- 原始日志位于本 worktree 的 gitignored `build/`：
  `integration-upstream-tests.log`、`dataservice-hd-tests.log`、
  `data-service-upstream-tests.log`、`data-service-sdk-build.log`、
  `source-gates.log`、`master-transitive.log`、`missing-submodule-check.log`；
  各对应 CTest 运行同时保留 JUnit。日志、数据库、二进制均未入库。
