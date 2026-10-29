# Data_service 接入与全项目数据迁移

## 工区模型作为预览数据源（本次修正）

井道图通过子模块 `listSourceWells` / `readWellPlotData` 按稳定井 ID 汇集已入库的
曲线、层段、标准层及照片引用。当前 RAW 测井预览按 `areaModelSourceId` 直接请求
模型，原件离线仍可显示已入库的数值；模型失败如实报错。井坐标系／单位与工程
目标坐标系／单位分别从模型读取并展示，不推测或隐式重投影。
详细边界和验证见 [井道图接入记录](progress/welltracks-data-model-20261029.md)。

新建空工程先通过 DataService.createProject 建立
`artifacts/metadata/area-model/data.sqlite`；选择资料总目录后，在后台直接调用
DataService.importDirectory 构建跨文件的井、曲线、地层、样品及工区资料模型。
新建工程默认使用 Data_service 专用原生 QWizard，收集工程与资料目录、EPSG、
坐标单位、并行预算及可选井位列映射，不再先跑旧分类扫描；旧向导仅用于补充资料。
任务页显示 importDirectory 的真实进度，结束后显示 ImportReport 数量。模型来源表及
对象关系直接批量投影到独立目录索引，不再调用 importFiles 二次读取、哈希或解析原文件。
资产目录继续承担文件版本、导入台账和 QGIS 图层登记，不能代替这个业务模型。
工程 CRS 与单位通过 setProjectCrs 写入 ds_meta，并作为解析坐标约定；未声明时仍保留未知。
原始单元格、层位点、层段和测量值按 256 行一批执行 execBatch，兼顾写入耗时与中途取消。

目录创建强制引用来源，避免继承上一次确认表的复制选项。引用模式的测井校验
直接读取原件／工区模型，以读取前后 SHA-256 检查替代临时文件复制。已属于工区
模型的 RAW 外链只登记资产和关联，不再为每个版本另建 native-model 数据库；
未建立工区模型的单文件与 DERIVED 版本仍保留独立模型。
来源查找按数据库／WAL 指纹缓存路径索引；曲线和预览使用 listSourceData 在
SQLite 中按来源筛选，并为来源关联列建立索引，避免逐文件列举全工区资料。
area-import 日志记录模块哈希、解析、复制、入库耗时；索引投影复用模型已保存的
SourceIndexEntry 和 SHA，不再逐文件校验。目录快照与模型本身的写入不属于原文件复制。
首次建模仍须解析和入库，只有未建模型的旧导入才保留兼容校验路径。

井实体读取通过 Data_service::listWellInfos 一次取得基础字段，投影到目录稳定
实体 ID 上；井树、详情、ProjectDataFacade 和井位图层导出共用该读口。已有井
按持久化的 areaModelWellId 或唯一精确井名匹配，禁止模糊名静默合并。模型修改
后按 DB/WAL 指纹刷新；坐标缺失保留 NaN，并清除失效的旧经纬度缓存。不猜 CRS
或单位；重新登记时补齐模型里尚未出现在目录的井。KB 等模块尚无字段继续保留。
子模块直接将有明确井名及 X/Y（或横／纵坐标）表头的 XLS/XLSX/CSV 坐标表写为
WellInfo；横纵语义优先于列名前缀，重复／无效行保留问题与原单元格，不虚构坐标。

工区模型任务不再 quiet，准备／扫描／首文件解析时即可在任务页看到阶段；
futuretask 转发文字和范围变化，不再等待百分比变化。后续不再做逐文件分类，
直接根据模型批量登记目录索引；显式井位列映射读取模型原表并写回模型。
宿主默认 4 个解析线程，可用 PALEO_IMPORT_PARSER_THREADS 配置 1–4；
PALEO_IMPORT_PENDING_MB 配置待解析／已解析未入库数据的估计预算（8–1024 MiB，
默认 256 MiB）。XML/LAS 按流式格式估算展开体积，大于 32 MiB 不再无条件占满
整个预算；压缩表格及未知大格式仍隔离处理。最终入库由单个 SQLite 写连接执行。

更新资料设置时，通过子模块 snapshotDatabase 创建包含已提交 WAL 的一致快照，
在暂存模型上增量导入；重复来源保留已有人工修改。成功后在工程会话仍有效时
替换整个模型目录。致命错误、取消、切换工程都不发布暂存模型；单文件失败按模块契约保留已完成文件，并在汇总中报告失败。目录扫描显式排除
本工程输出和暂存树，避免再次导入生成的数据。

XML 预览会话先按 sourceId 查询工区模型；数据列表通过 listData/readTable 获取
持久化数据，综合柱状图通过 readLogSet/readIntervals/readWellInfo 获取模型对象。
不再为这两个标签分别复制和解析 XML。宿主通过显式模板选项保留已确认的“井号／深度／TVD／TVDSS”导出约定；普通表格
不据数值猜单位。测井适配保留 MD、TVD、TVDSS 的独立深度列和缺失值。
当前 RAW 的托管路径显式映射到来源路径；
关闭工程时释放映射。测井数值入口也优先读取工区模型的处理值，缓存指纹包含
工区数据库/WAL 的变化。绑定来源原件修改不会使预览静默切回文件解析。

无工区模型的旧工程仍使用原有读口。多井源文件拒绝合并绘制成一口井。尚未适配
的符号/标准层等图道保留在模型数据表中；历史版本显式模型选择、全部地质算法
读口及图道细节仍需继续迁移，不能据此声称所有系统数据访问已经替换。

## 前两批实现记录

目标是让井、测井、分层、测斜、时深、地震和图件通过 Data_service 的公开
C++ 模型读写与更新。Paleo 保留地质语义、不可变资产版本、工作流与 QGIS
工程装配。不是把所有 QFile/GDAL/QGIS 操作换成一个通用文件接口。

本批先接入**全项目共用的资产目录持久层**：DataCatalog 的实体、资产、版本、
关联四表和目录元数据由子模块 `CatalogStorage` 读写。主项目的 CatalogStore
成为类型适配与事务／恢复编排层。现有 `catalog.sqlite` 路径、表结构、rowid
顺序、版本 ID、显式关联、嵌套 extra、JSON 迁移及备份格式保持兼容。

第二批已打通新建工程、类型化导入与井数据消费：

- 新建工程先初始化共享目录 SQLite，再生成 QGIS 工程及 `project.paleo`；
  清单标记 `data_service_versioned`。旧工程清单收养不会伪造迁移标记。
- 测井、井位、分层、时深、测斜在导入 worker 中写入独立的 Data_service
  数据项目，每个版本绑定 `sourceId`、内容 SHA-256 和模型数据库 SHA-256。
  模型与 RAW/DERIVED 一起通过现有 intent + staging + catalog journal 发布。
- `WellLogRead`/`LasCache`、预览井表、ProjectDataFacade、框架分层读取、岩石物理
  时深读取与分层编辑的输入端优先读取已绑定的模型。旧版本仍用兼容解析器。
- 井位和分层的共享语义对象之外，KB、井底坐标、分层 XY/时间等扩展列，通过
  子模块公开的 SourceTable 存储保留；不在主项目再维护一份可写井数据库。
- 标准化及深度补算的派生井表创建自己的模型。源文件保留原字节，缺失值以 NaN
  入模型，有限负数不套用默认空值规则。无法确认的井身份不创建虚构井。

**尚未完成全部读写替换。** 交互编辑输出、曲线计算成果、岩性/样品、地震体
读口和地质编辑元数据仍有旧链路。路径重定位及通用版本回滚先清除旧模型绑定，
用兼容读口读取新路径，避免把旧 sourceId 当成新路径的验证凭据；后续需要同样
建立模型版本事务。QGIS 继续负责空间文件、布局与工程序列化。

## 结合点与差异

| 数据面 | Paleo 当前入口 | Data_service 对应能力 | 迁移约束／状态 |
|---|---|---|---|
| 实体、资产、不可变版本、关联 | DataCatalog → CatalogStore | 新增 CatalogStorage 与标准 C++ 记录 | 本批实际接入生产读写；保留主项目事务和恢复策略 |
| 井基础资料 | CatalogEntity、EntityView | WellInfo、listWells/readWellInfo/saveWellInfo | 井名不能作为跨库映射的唯一键；保留计划井与真实井区别、KB 与缺失值 |
| 测井数组 | WellLogRead、LasCache、WellLogSet、PreviewDocService | LogSet、CurveSlice、readLogSet/updateCurveValue | 新导入读取已接入；交互编辑输出及旧版本回填待迁移 |
| 分层／岩性／样品 | WellComposite、wellrecords 等 | IntervalTrack、MarkerTrack、SampleTable | 保留多解释道、来源单元格、未钻穿与单位基准，不凭深度合并 |
| 测斜／时深 | Deviationsurvey、TimeDepthModel 与预览门面 | TrajectorySurvey、TimeDepthTable | 导入及主要读取门面已接入；不推测 OWT/TWT 或参考面 |
| 地震按需读 | SegyReader、SeismicTaskService、SBM 引擎 | inspectSeismic/readSeismicTrace/Line/TimeSlice | 共用同版本引擎；保留有界读取、索引缓存、取消和线程内句柄 |
| 图件与附件 | catalog、PreviewDocService、QGIS 层 | GeologicalMap、CorePhotoTrack、Attachment | 登记路径不等于读完内容；空间读写仍使用 QGIS/GDAL |
| 地质编辑与成果语义 | metadata 各 store、版本／发布状态机 | 当前无完整对等模型 | 保留 Paleo 语义；后续通过专用扩展接口落库，不塞进无类型字符串 |
| 工程与空间文件 | QgisProjectService、PaleoProjectStore、QGIS provider | 并非 Data_service 的职责 | .qgz/.gpkg、样式、布局由 QGIS 读写；资源身份与版本可统一登记 |

现有 `src/io` 有大量多格式和专业读写实现，不能因子模块已有 LAS/CSV/XML
接口就删除 DLIS/LIS、缓存、范围读取、结构侧卡或导出协议。应按入口清单迁移，
而不是按扩展名批量替换 include。

## 本批实现边界

1. **可嵌入构建**：主项目关闭子模块演示、工具、示例、测试及安装目标；独立
   构建默认仍保留这些入口。主项目明确传入已有 SBM target 与匹配头目录，
   不在同一进程链接第二套相同符号的地震引擎。
2. **符号隔离**：子模块复制的 ZIP 实现使用独立 namespace，避免与 Paleo
   当前 ZIP 实现发生重复定义或混用版本。子模块保留第三方来源声明。
3. **目录后端**：子模块拥有目录记录结构、兼容 DDL、增量 upsert/delete 和
   快照读取。QtSql 命名连接是过渡接缝，由主项目拥有；子模块不偷偷重开连接、
   提交事务、迁移文件或创建第二份可写目录。公开记录仍使用标准 C++/UTF-8。
4. **安全与恢复**：schema 版本门、只读策略、WAL/FULL、主库完整性检查、
   备份轮转与原子替换仍由主项目编排。坏路径过滤、坏 ord 防护、序号吸收及
   缺失数值标记在适配层保留。不能因搬入 vendor 而删除这些保护。
5. **类型化只读会话**：DataService 新增 openProjectReadOnly。它不执行 schema
   补列／索引升级；导入与重导入在复制来源或启动解析前拒绝。更新由 SQLite
   只读连接拒绝。每个线程独立实例；SQLite WAL 读者可能仍需常规 sidecar，
   只读不等于 immutable，也不忽略正在提交的 WAL。

## 后续全量替换的执行顺序

1. 将交互编辑、通用派生注册、回滚及外链重定位接入同一模型快照发布协议；
   保留 RAW、来源、quality、共享撤销栈与失败回滚，禁止原位修改已发布模型。
2. 用相同的版本/source 映射迁移岩性、样品、地震与附件的专用读口。
   地震仍采用共用引擎的有界读取及线程独立句柄，不把大 SEG-Y 搬进 SQLite。
3. 为旧工程提供显式、可恢复的模型回填；当前不在打开工程时静默批量转换，
   重复导入的 AlreadyStored 也不伪装成已完成回填。
4. 逐条完成后移除相应旧读口，保留未覆盖的 DLIS/LIS 等解析与导出协议。
   地质编辑、约束、成果与发布采用专用模型扩展，QGIS 保持空间执行器。

## 类型化快照的提交与读取契约

`CatalogVersion.extra.nativeModel` 保存 schema=1、相对模型路径、数据库摘要、
来源摘要及子模块 source/object ID。模型位于
`artifacts/raw/<asset>/<version>/native-model/data.sqlite`，即使 RAW 外链也由
工程持有模型；受管来源使用相对引用，因此整个工程目录可以移动。

写入顺序为验证原始内容 → prepare 类型对象 → 写入并关闭子模块 SQLite →
摘要与只读权限 → 写 import intent → 移动全部成果 → 一次 catalog journal
事务。崩溃恢复识别已登记模型；失败行清理及协作取消不提交模型。存储治理
把模型及 SQLite sidecar 作为版本附件保护，不能当作孤儿回收。

读取使用线程内独立只读 DataService 会话，校验数据库和来源摘要。绑定模型
损坏或缺失时明确失败，不悄悄改读 RAW。测井 header 使用已提交的轻量元数据，
样点读取才装载模型；LasCache 的指纹包含模型文件状态，损坏后旧缓存失效。
共享外部路径若对应不一致的版本，路径本身无法判定版本，保留兼容读口，后续
应推动消费接口显式携带版本 ID。绑定按服务实例管理，切换及关闭项目时释放，
关闭的项目不会继续参与当前外部路径的消歧。

## 验收要求

- 主项目现有 catalog／SQLite 回归验证迁移、增量、顺序、未知字段、坏路径、
  未来 schema 拒绝、备份恢复、只读与事务回滚。
- 新增宿主接入测试验证主项目写入后能由子模块读取，缺失值与嵌套 extra 保留；
  验证类型化曲线修改／重开／只读，并把宿主 ZIP 与服务解析器链接进同一程序。
- 上游高清地震合成契约同时在宿主复用的引擎上运行，避免仅凭子模块独立
  引擎测试通过就断言宿主适配兼容。
- 子模块独立测试验证目录标准记录、准备模型的值与空值、事务回滚、摘要、取消、目录迁移及只读会话。
- 宿主链路验证五类井数据、派生版本、受管/外链、重开/迁移、模型损坏导致缓存失效、存储保护和取消。
- QGIS 真实新建后导入、保存、关闭、重开并读取曲线；目录初始化失败保留当前工程。
- 无私人工区数据；夹具在临时目录生成。未运行的平台和消费者不计已迁移。

最终命令和实际结果见 `docs/progress/data-service-integration-20261028.md`。
