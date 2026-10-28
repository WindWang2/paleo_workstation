# 工区目录模型与 UI 接入（2026-10-28）

## 实际主链

新建工程先建立 `artifacts/metadata/area-model/data.sqlite`，由子模块的
`createProject` 初始化。新建页“创建并导入目录”直接接受资料目录，后台调用
`importDirectory`；不以目录命名规则、井点映射或旧分类扫描作为必经条件。
“高级资料配置”保留显式坐标、井名及 QGIS 层位配置。默认引用原资料。

UI 不直接调用工作线程或解析器。目录导入通过 QPromise 转发模块进度，取消
通过共享原子标记传入模块。结果展示真实 ImportReport；单文件失败保留已
完成对象，并报告失败，致命错误／取消／工程切换不发布暂存库。

更新已有模型前，通过 `snapshotDatabase` 读取包含 WAL 已提交内容的一致副本，
在副本上增量导入，保留未变来源的手动编辑；成功后替换模型目录。目录遍历
排除宿主输出和暂存树。模型的来源列表驱动资产登记，不使用目录结构作为模型。

XML 列表经 listData/readTable 获取持久数据；综合图经 readLogSet/readIntervals/
readWellInfo 获取同一模型。测井读口优先读取处理值，适配保留 MD/TVD/TVDSS
列序、NaN 与有限负值。预览会话不再复制后重新解析已绑定 XML。来源修改不
静默切回文件解析；关闭工程释放来源映射，当前 RAW 托管路径有显式来源别名。

综合井模板的 MD／米约定由宿主显式选项启用，仅匹配确认的首四列和工作表名；
子模块独立使用时继续保留未知单位。旧工程与历史版本仍保留兼容入口。

## 验证

- 宿主构建：paleo、tst_projectsetup、tst_areamodel、tst_previewdoc、
  tst_nativeimport、tst_projectsvc 成功（MSVC，RelWithDebInfo，-j8）。
- 11/11 CTest 通过：5 个业务套件及 layering_strict/file_budget/i18n/
  ui_tokens/docs_drift/cpp_mainline，130.38 秒。
- 新测试证明：任意目录布局直接创建；重开后依然从模型读列表；修改模型分层
  后表格和地层图一致；破坏源 XML 后不回退；曲线修改后读口读取处理值；
  重复目录导入保留修改；只读连接的 SQLite 快照包含另一写连接的已提交 WAL；
  目录排除与取消。
- 子模块独立 Release：spreadsheet_xml_tests、prepared_import_contract 分别通过。
- 日志：build/area-verified-build.log、build/area-verified-tests.log、
  build/area-sdk-build.log、build/area-sdk-tests.log、build/area-sdk-prepared-tests.log。
- 最后调整高级配置／创建失败后的模式重置，另记录在
  build/area-final-ui-build.log、build/area-final-ui-tests.log。

## 仍保留的范围

资产目录仍管理版本和 GIS 登记，旧文件版本的独立模型作为兼容／版本数据保留。
历史版本显式选择、全部专业算法、单文件编辑输出、符号／标准层等图道细节仍
需继续迁移；本次不声称系统全部读写已替换。工区多井源文件可在列表查看，
禁止把多口井合成一口井的综合图。QGIS 继续承担空间文件和工程序列化。

子模块提交：57ea10266317c367a4c01e9e0166b021e7b99c90。
