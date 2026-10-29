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

阶段一验证结果（子模块 pin `4992620`）：

- 主仓四个目标编译成功，`tst_catalogstore`、`tst_datacatalog_sqlite`、
  `tst_dataservice` 3/3 通过；独立执行 `tst_dataservice_hd` 1/1 通过。
- 子模块完整独立构建成功，18/18 CTest 套件通过。
- SDK 安装成功，包含新增高清地震案例在内的十个 getting-started 示例全部编译。
- 原始日志位于本 worktree 的 gitignored `build/`：
  `integration-upstream-tests.log`、`dataservice-hd-tests.log`、
  `data-service-upstream-tests.log`、`data-service-sdk-build.log`、
  `source-gates.log`、`master-transitive.log`、`missing-submodule-check.log`；
  各对应 CTest 运行同时保留 JUnit。日志、数据库、二进制均未入库。

## 阶段二：新建工程、类型化导入与主要井数据读取

子模块已跟进上游 `acd3472`（批量时间切片、有界缓存），适配提交固定为
`c41a6e30c5d37e4452b5a204752b27e57317c420`，配套上游 PR 仍为
[Data_service #1](https://github.com/changyanyanchang/Data_service/pull/1)。
主项目在 [#817](https://github.com/WindWang2/paleo_workstation/pull/817) 继续更新。

实际落地：

- 新建 QGIS 工程前初始化共享目录 SQLite，清单记录数据后端；目录初始化失败
  不关闭当前工程。真实验证新建→导入→保存→关闭重开→读取曲线及稳定项目 ID。
- 导入 worker 将已验证的测井、井位、分层、时深、测斜模型保存为各版本独立的
  Data_service 数据项目；关闭写连接并计算摘要后，与源文件一起通过既有恢复协议
  发布。外链源保留在原处，模型受管；受管来源使用相对引用，整个目录可移动。
- 井表标准化及深度补算的派生版本建立独立模型，不能继承另一组字节的绑定。
  KB、井底坐标、分层 XY/时间等扩展列通过公开 SourceTable 保留；已确认的坐标
  CRS 对应处理后的坐标，未确认字段保持空。NaN 空值不再次套默认负数哨兵。
- WellLogRead、LasCache、主要井表/时深/轨迹门面、框架分层、岩石物理时深和
  分层编辑输入读取已绑定的模型。损坏明确失败，缓存感知模型变化，存储清理保护
  模型；读取绑定跟随服务生命周期，在切换/关闭工程时释放。
- 通用回滚和路径重定位清除旧 source 路径绑定；旧项目、未确认身份以及尚未迁移
  的交互编辑输出保留兼容读口。没有声称地震、岩性/样品和所有编辑已全部替换。

验收与范围：

- 宿主 10 个目标编译并通过：目录两套回归、Data_service 接口、高清地震、批量
  时间切片、原有导入 UX、项目坐标、清单安全、QGIS 工程服务和原生导入链路。
- 子模块完整独立构建、20/20 CTest 通过；安装 SDK 后 11 个示例全部编译。
- 本地源码 CTest 闸门 30/30 通过。显式给 QGIS 头目录的 transitive 全闸仍是
  `ui_qtwidgets_notes 6 > 5`；独立 ts_stale 检查仍是 `169 > 31`。master 与
  本 worktree 两项值相同，新增中文消息已入 TS，没有提高任何基线。
- 从全新 bare 仓库经 `.gitmodules` 的上游 URL 成功获取固定提交，避免只能从
  fork 获取、而普通递归克隆失效。没有修改上游 URL 来绕过此验证。
- Windows/MSVC/Qt 6 验证；未在 Linux 本机编译，也未跑全量工作站 GUI 套件。
  构建不超过 8 workers，Windows 测试串行。本机 MSVC 增量链接缓存曾损坏，
  清理后在本地构建缓存设 `/INCREMENTAL:NO` 完成重新链接，不修改项目策略。

阶段二日志（均在 gitignored `build/`）：`native-complete-tests.log`、
`native-verified-tests.log`、`native-sdk-tests.log`、`native-sdk-examples.log`、
`native-final-source-gates.log`、`native-full-transitive.log`；对应运行保存 JUnit。
合成输入、SQLite、日志、对象文件和 SDK 安装产物均未入库。
