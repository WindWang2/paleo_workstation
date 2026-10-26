# 综合柱状图 XML 与测量坐标表适配（2026-10-26）

基线：`master`，`829ffb1a`。在独立 worktree / `codex/hxn-data-support`
实现；原工区文件及含井名、坐标的问题清单仅保留在本机，未入库。

## 兼容原则（用户补充确认）

HXN 综合柱状图是新增的数据模式。原 LAS / DLIS / LIS、CSV / TXT / TSV、
已有曲线 XML、井位／分层／轨迹／时深表和已有工程读取路径继续保留；
不迁移旧资料，不批量修改历史版本元数据，不把 EPSG:2436 或 MD／米设置
成所有资料的默认口径。新旧资料可以在同一工程和同一口井下共存。

新模板按原始四列表头精确识别。已有 XML 中带 `[m]`、`[ft]` 或 `(m)`
的深度列继续使用原来的单位与基准规则，不因为移除单位括号后与新模板
同名就进入新模式。普通 CSV 即使使用相同列名也不触发 XML 模板规则。
旧资产没有 `logCompositeTemplate` 标记时仍走原预览路径；只有识别到的
新增模式登记该标记，关闭重开后继续按各自模式读取。

兼容回归覆盖：带单位旧 XML、普通文本的未知深度口径；旧 XML＋英尺 CSV
与新综合 XML 混合导入、版本元数据不变与重开；无新元数据的旧 LAS 工程
追加综合 XML；新旧 XML 各自的实际预览入口。

本轮兼容验证的 10 个套件通过：`tst_las`、`tst_dlisparser`、`tst_lisparser`、
`tst_folderimport`、`tst_welllogtext`、`tst_import_ux`、`tst_import_preview`、
`tst_projectsetup`、`tst_welllog_consumers`、`tst_welllog_multiformat`。
Windows 串行执行。入库测试对旧版本的 ID、持久化元数据内容和原始曲线值
做前后比较，预览测试分别检查旧入口和新增入口，均包含关闭重开。

## 来源与已确认口径

本机资料包含 57 份 Excel 2003 SpreadsheetML 综合柱状图 XML、1 份井坐标
工作簿、14 张相图图片和 1 份构造图 PPTX。XML 合计约 2.33 GiB，最大
552,562,592 字节（526.96 MiB）；连续曲线共 1,960,256 行，离散曲线
4,696 行，12 张离散曲线表仅有表头。

用户给定水平坐标系 EPSG:2436，并确认综合柱状图的深度／顶深／底深为
MD、单位米。没有可用的带号转换规则。坐标表使用测量命名
`X（纵坐标）/ Y（横坐标）`，并混有前缀、轴序与缺失值问题；井身份也有
文件名与内部井号不一致的情况，必须保留显式归属确认。

## 实现

- `outsourceworkbook` 提供复用原单元格校验的行访问接口，文件通过
  `QIODevice` 流式读取。专用综合 XML 路径受 1 GiB / 1,600 万输入单元格
  预算约束；通用工作簿整读的旧预算不变。
- `WellLogRead` 的 XML 分支逐行产出 double 采样，曲线并集限制为 2,400 万
  单元格。连续与离散数据按实际深度精确对齐；同名 TVD/TVDSS 保留工作表
  来源，其他物理曲线重名仍拒收。空离散表不使有效连续表失败。
- 仅识别完整的 `井号 / 深度 / TVD / TVDSS` 综合模板为已确认的 MD／米
  口径，并记录提示；其他模板和普通文本仍要求明确单位／基准，物理曲线
  的缺失单位在统一曲线契约中保留为空并提示。既有井道图的助记名单位
  显示规则不作为原始文件已声明单位的证据。
- 综合柱状图按最多 4096 个数据行一块装配曲线；地质道沿用既有解释。
  截断、畸形 XML 失败时不向调用方交付部分图。数据列表只保存每表前
  1000 行，曲线和原始数据不截断。
- 导入元数据登记 `logCompositeTemplate`；此模板复用已有井道图／数据列表
  视图和后台 XML 会话，作为测区井显示。普通曲线 XML、LAS 与多来源井
  曲线并集继续使用现有流程，不增加视觉样式或 IO 到视图层的依赖。
- 井点字段的明确横／纵、东／北含义优先于 X/Y 字母，用户显式映射优先。
  EPSG:2436 对明显不合理的东／北数值报因拒收，即使源与目标 CRS 相同也
  校验；不猜来源投影、不删除带号、不修改原件。
- 实文件受管导入暴露出 Windows 深目录＋长中文文件名的原子替换失败。
  原生 Win32 替换采用绝对扩展路径，保留 UNC 和已有扩展路径；
  仍使用替换语义，不预删目标。依据
  [Microsoft MoveFileExW 文档](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw)。

## 验证

- Windows 本机统一 Qt 6.11.2 / QGIS 4.2 依赖链，编译并链接 `paleo.exe`
  与相关测试；构建并行度为 8，Windows 测试串行。
- 57/57 私有 XML 均通过统一曲线入口和综合柱状图入口，整批约 157 秒，
  包含 527 MiB / 373,301 行连续采样的大文件。
- 最大源文件经实际受管 RAW 导入、曲线目录查询和关闭重开验证通过，约
  54 秒；原件入库前后 SHA-256 相同。没有坐标证据的井保持 unknown。
- 原始坐标表经实际读取与 EPSG:2436 拒收验证通过（55 条数值记录、1 条
  缺失记录），未生成错误井点或规范化派生文件。
- `tst_welllogtext` 覆盖重复垂向坐标来源、空离散表、超过原 64 MiB 门槛的
  文件以及既有错误／并集预算契约。私有资料验收由
  `PALEO_HXN_DATA_DIR` 显式启用，不依赖仓外数据跑 CI。
- `tst_import_preview` 的合成综合模板经公开导入和打开资产入口，断言
  8300 点跨块完整性、地层与岩性道、测区井身份和 1000 行列表上限。
  还断言 GR 默认可见；XML 预览的布局记忆按工程隔离，避免其他工程的
  同名井配置污染当前图。图形检查使用合成数据，不将私有工区图上传。
- `tst_projectcoordinates` 覆盖测量 X/Y 表头、显式映射优先、同 CRS 异常
  拒收和 EPSG:2436 到经纬度的有效转换。
- `tst_metadata_atomicfile` 覆盖超过 260 字符的中文路径、新建、替换、
  替换前故障保留旧内容，以及 Windows UNC／已有扩展路径规范化。
- 修正一条原有预览回归的测斜夹具：断言 TVD 150 → MD 300 的插值现在
  使用从 MD 0 开始的 60° 测斜；旧夹具从 MD 500 开始且为直井，与该
  断言不符。保留全部原断言和超出测斜范围拒绝外推的其他测试。

源码闸门：layering strict / symbol / qgs-audit、qgis-spread、UI invariants、
UI size、file budget、i18n 与 docs drift 通过。`buildContent` 拆出支持类型
查询后已低于函数体上限，file-budget 删除 1 条基线（55 → 54）；其他
基线未增长，Qgs 两基线删行数均为 0。

两项已有红灯用未修改的主 checkout 脚本复核，改前／改后完全相同：

- `check_ts_stale.py`：47 个缺失翻译源串，旧冻结值 31；本次新增提示均已
  补入翻译，未扩大缺口。
- `check_layering.py --transitive`（显式 QGIS_PREFIX）：
  `ui_qtwidgets_notes=6 > 5`，其余计数为 func=0、heavy headers=40、
  heavy TUs=49；报告的 UI include 边均不属于本次修改。

Windows 的 `tst_metadata_atomicfile` 原有
`durabilityRegression_parentDirectoryFsync` 仍失败：旧 `paleoSyncDir` 以
FILE_READ_ATTRIBUTES 打开目录后 FlushFileBuffers 返回失败；该函数保持
不变。新增长路径替换和故障保留旧文件用例及其余原子文件用例通过；不将
这一原有目录刷盘失败宣称为本次已解决。

## 数据使用边界

原坐标表当前不具备统一、可信的 EPSG:2436 数值口径，需由资料方核实
带号／轴序后再导入井位。无坐标的井可保管曲线，不建立伪造地图位置。
相图图片与 PPTX 沿用既有参考／配准／Office 原件路径，不自动矢量化，也
不冒充可计算的因素值。完整原始工作表保留在 RAW；未有专用业务解释的
道仍可在数据列表查看，不自动转换为训练标签或平级地层。
