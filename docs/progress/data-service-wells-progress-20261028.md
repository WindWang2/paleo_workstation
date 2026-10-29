# 工区井坐标、导入准备阶段与解析并行

## 等待阶段的实际路径

点击创建后先校验路径、创建 QGIS 工程、目录库及工区库，再准备工区模型一致快照、
扫描来源并校验／解析／入库。模型完成后还需读取来源、识别旧资产类型、登记资产。
此前模型任务 quiet=true，不自动展开任务中心；进度转发只监听数值，0% 时的扫描
与首文件解析文字不可见。GUI 上随后逐个分类来源也会造成可见停顿。这解释了
为什么后续“导入所选文件”出现之前看不到主要工作，并不等于那时才开始解析。

本次取消 quiet，转发 progressTextChanged/progressRangeChanged，扫描时使用未知
总量进度，文件总量确定后改为文件计数。QPromise 本身可能忽略同值文字，因此
模型任务在启动 worker 前创建，并用独立的排队阶段通道更新 0% 时的文字与范围。
初始化、快照、来源索引、准备资产登记
均有阶段文字。分类移出 GUI；project-create 日志区分路径校验、QGIS/数据库创建
与调度，总耗时和前一批 area-import/catalog-import 日志配合判断实际目录瓶颈。
未测量该用户数据集各阶段百分比，不声称所有创建前等待已经消除。

## 并行机制

宿主默认 parserThreads=4，可配置 1–4；解析 worker 使用独立输入，不持有 SQLite
写连接。调用线程按确定顺序提交，避免多进程争写同一个库。内存估计预算默认
256 MiB，可配置 8–1024 MiB；预算不足会减少同时在途的文件。XML/LAS 使用 4 倍
输入字节估算，支持大于 32 MiB 的流式文件在预算允许时并行。压缩／未知大文件
继续独占预算。来源不复制。

## 井实体读取

新增 listWellInfos 基础字段批量查询，不走 listWells 的逐井统计子查询。目录
子模块补齐 XLS/XLSX/CSV 的明确井位表头解析，直接写入 WellInfo；如
X（纵坐标）/Y（横坐标）按纵／横语义映射，不按列名前缀或数值大小猜轴序。
多井坐标工作表不套用单井工作簿的冲突判据；重复井位或非法行保留问题和原表。
读口保留实体 ID、链接及 KB 等未覆盖字段，以工区模型覆盖 X/Y、总井深、坐标系、
单位、井型、备注。匹配用持久化模型井 ID 或唯一精确井名。缓存按 DB/WAL 的大小
及时间戳失效。缺坐标不回落旧坐标；新坐标变化后清除旧经纬度缓存。原 CRS/单位
保留，未经确认不猜投影。模型独有井在资产导入完成后登记，避免干扰用户确认的
“新建井”选择。井树／详情、ProjectDataFacade 与井位 GeoJSON 因此共用模型事实。

## 模型直登记（后续修正）

模型与目录索引分开保存：Data_service 继续管理业务对象，独立 catalog.sqlite 只承担
资产、版本与关联索引。新增 SourceIndexEntry 提供模型已保存的路径、存储路径和 SHA，
不改变原 SourceEntry 或 ParseOptions 契约。目录创建完成后由模型来源表和对象关系
批量生成索引，不再运行“导入所选文件”第二遍逐文件导入，也不再做来源分类扫描。
同一来源重复登记复用资产，内容变化追加版本；原文件删除后仍可完成索引。显式井位
well/x/y 列映射直接读取模型中的 SourceTable，再用 createWell/saveWellInfo 写回同一
模型，不读取原件，也不改变子模块公共解析选项。

## 批量写入、工程 EPSG 与专用向导

原始单元格、层位点、层段和测量值按 256 行一批执行 execBatch；批次复用同一 SQL，
在批次间检查取消。新增工程级 projectCrs/projectCrsUnit 元数据及 setProjectCrs，EPSG
同时写入 ds_meta 与模型坐标约定，重开保留。新建工程改用 Data_service 专用 QWizard，
收集目录、EPSG、单位、线程／内存预算和可选井位列映射；补充资料仍走原向导。

## 验证

新增测试：百分比保持零时阶段文字与范围仍更新；模型坐标修改／更名／缺失值在
目录、编图门面、井位导出中一致；模型独有井登记；目录模型任务为非静默任务。
并行／串行结果一致、取消和小内存预算沿用子模块 import_performance_tests。

最终复验：宿主 11/11（6 个业务套件 + 5 个源码闸门），子模块独立 Release 20/20。
新增向导端到端测试覆盖 EPSG 元数据、模型井位及目录索引；批量写入覆盖数值、回滚与取消。
日志：build/data-service-wizard-final-build.log、build/data-service-wizard-final-tests.log、
build/data-service-crs-batch-full-build.log、build/data-service-crs-batch-full-tests.log。
