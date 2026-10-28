# 引用导入的复制与耗时修正

## 代码定位

- 截图标题“导入所选文件”来自 FolderImportWorkflow::importFiles；目录模型建立
  后的资产登记也会进入该流程。默认引用并未阻止旧 importOneFile 的测井临时快照。
- 旧流程对测井文件执行来源哈希、完整临时复制、快照哈希、解析，然后为每个
  RAW 版本创建 DataService 项目、再次校验来源、写曲线并计算数据库哈希。
- openSource 每次打开工区库、列出全部来源并规范化每个路径；readLog 和预览
  又列出全工区曲线／数据后按 sourceId 筛选。逐文件处理使元数据扫描接近二次增长。
- 任务剩余时间按已完成文件数的滑动速度外推，不考虑单文件大小、曲线数量与
  解析复杂度；1% 时的 161 分钟不能视作实测总耗时。尚无该用户数据集的分阶段
  运行记录，不能给出各原因占比或整批加速倍数。

## 改动

- 外链测井直接读取，以读取后真实 SHA-256 复核来源；显式受管模式保留既有契约。
- 创建工区的后续登记强制外链，不继承旧确认表的复制设置。
- 已绑定工区模型的 RAW 外链验证来源后复用模型，跳过逐文件独立数据库。
- 缓存来源路径索引，以数据库／WAL 的尺寸、修改、元数据变化及创建时间失效。
- 子模块增加 listSourceData，在 SQLite 筛选来源；增加 source_id 索引，并将
  additive performance schema 从 1 升到 2。只读打开不执行升级。
- 记录 area-import（elapsed/hash/parse/copy/store/hashedBytes）、catalog-import
  （elapsed/hash/validation/native）与 catalog-batch（含提交）阶段耗时。
  模块并行 worker 的累计阶段时间可大于实际墙钟时间。

## 验证设计

- 已编辑曲线的工区模型在外链登记后继续生效；不生成 RAW 原件或逐版本数据库。
- 24 个 LAS、48,000 个采样点的批量目录建模与资产登记，检查单一工区模型并打印耗时。
- 来源缓存中的未命中在新增来源后失效；按来源查询不泄漏其他来源，非法 ID 被拒绝。
- 保留受管、独立单文件、派生版本、取消、预览和项目创建回归。

验证日志：build/no-copy-final-build.log、build/no-copy-tests.log。

## 验证结果

- 主程序 paleo 与相关测试编译成功（MSVC RelWithDebInfo）。
- 宿主 8/8 CTest 通过，273.02 秒：layering_strict、file_budget、docs_drift、
  cpp_mainline、tst_previewdoc、tst_projectsetup、tst_nativeimport、tst_areamodel。
- 子模块 import_performance_tests 与 readonly_service_contract 2/2 通过，4.92 秒；
  日志 build/no-copy-sdk-build.log、build/no-copy-sdk-tests.log。
- 合成 LAS 批量用例：24 文件、48,000 点、355,118 字节；工区建模 2,084 ms，
  兼容资产登记 4,686 ms，提交 96 ms；模块 copy_ms=0，文件系统检查无原件副本
  和逐文件 native-model。测量包含当前机器其他构建任务的影响；不是用户数据集
  的基准，未做前后对照，不据此宣称加速倍数。详细数值保存在
  build/Testing/qtest-first-run/tst_nativeimport.txt。
