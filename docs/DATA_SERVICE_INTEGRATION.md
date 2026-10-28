# Data_service 接入与全项目数据迁移

## 目标与当前落点

目标是让井、测井、分层、测斜、时深、地震和图件通过 Data_service 的公开
C++ 模型读写与更新。Paleo 保留地质语义、不可变资产版本、工作流与 QGIS
工程装配。不是把所有 QFile/GDAL/QGIS 操作换成一个通用文件接口。

本批先接入**全项目共用的资产目录持久层**：DataCatalog 的实体、资产、版本、
关联四表和目录元数据由子模块 `CatalogStorage` 读写。主项目的 CatalogStore
成为类型适配与事务／恢复编排层。现有 `catalog.sqlite` 路径、表结构、rowid
顺序、版本 ID、显式关联、嵌套 extra、JSON 迁移及备份格式保持兼容。

**尚未完成全部读写替换。** 子模块原有的类型化 `DataService` 和本批的目录
扩展是互补接口；没有建立井对象与版本内容之间的自动双写，也不把目录迁移
误称为曲线、分层、地震和编辑元数据已经全量迁移。

## 结合点与差异

| 数据面 | Paleo 当前入口 | Data_service 对应能力 | 迁移约束／状态 |
|---|---|---|---|
| 实体、资产、不可变版本、关联 | DataCatalog → CatalogStore | 新增 CatalogStorage 与标准 C++ 记录 | 本批实际接入生产读写；保留主项目事务和恢复策略 |
| 井基础资料 | CatalogEntity、EntityView | WellInfo、listWells/readWellInfo/saveWellInfo | 井名不能作为跨库映射的唯一键；保留计划井与真实井区别、KB 与缺失值 |
| 测井数组 | WellLogRead、LasCache、WellLogSet、PreviewDocService | LogSet、CurveSlice、readLogSet/updateCurveValue | 原始值、处理值、quality、MD/TVD/TVDSS、多文件并集与主文件必须逐项对拍 |
| 分层／岩性／样品 | WellComposite、wellrecords 等 | IntervalTrack、MarkerTrack、SampleTable | 保留多解释道、来源单元格、未钻穿与单位基准，不凭深度合并 |
| 测斜／时深 | Deviationsurvey、TimeDepthModel 与预览门面 | TrajectorySurvey、TimeDepthTable | 井关联、角度约定、OWT/TWT、参考面与单位显式映射 |
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

1. 固定跨模型身份：CatalogVersion ID ↔ source/log_set/track/volume 稳定编号。
   保存显式映射及输入版本，不按井名、文件名或当前列表下标推断。
2. 给类型化更新增加版本事务：当前 updateCurveValue/saveWellInfo 是原位编辑，
   不能直接绑定到 Paleo 的 RAW/DERIVED 历史版本。需先支持冻结输入、派生新版本、
   来源／质量标记与回滚，再接入编辑命令和共享撤销栈。
3. 在 prepare/worker/publish 中解析并保存类型化对象。目录版本登记和类型化
   对象发布必须共享可恢复的提交协议；不在 catalog 提交后盲目双写 data.sqlite。
4. 先迁移测井一条纵向链：导入 → 版本绑定 → readLogSet/CurveSlice → 预览／
   单因素／预测 → 编辑产生新版本 → 关闭重开。通过之后移除该链旧解析读口。
5. 同法迁移分层、测斜、时深、样品、地震和图件；逐条记录覆盖消费者与剩余
   兼容格式。旧工程只在显式、事务性的迁移成功后切换唯一权威，失败保留原件。
6. 对地质编辑、约束、成果和发布设计子模块扩展；QGIS 保持空间文件的执行器。
   数据库是否物理合并另作决策，不能用改路径替代 schema 与事务迁移。

## 验收要求

- 主项目现有 catalog／SQLite 回归验证迁移、增量、顺序、未知字段、坏路径、
  未来 schema 拒绝、备份恢复、只读与事务回滚。
- 新增宿主接入测试验证主项目写入后能由子模块读取，缺失值与嵌套 extra 保留；
  验证类型化曲线修改／重开／只读，并把宿主 ZIP 与服务解析器链接进同一程序。
- 上游高清地震合成契约同时在宿主复用的引擎上运行，避免仅凭子模块独立
  引擎测试通过就断言宿主适配兼容。
- 子模块独立测试验证目录标准记录的读写／回滚／重开及只读会话。
- 无私人工区数据；夹具在临时目录生成。未运行的平台和消费者不计已迁移。

最终命令和实际结果见 `docs/progress/data-service-integration-20261028.md`。
