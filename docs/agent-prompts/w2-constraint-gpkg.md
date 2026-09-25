你在 paleo_workstation 项目把约束要素从内存伪源升级为真实 GeoPackage 持久化。

【执行方式】本任务必须使用 teamwork-preview 模式执行：自行拆解子任务
（ConstraintStore 层 / workflow 改造 / 持久化测试），能并行的部分用
teamwork-preview 并行开 worker，不能并行的串行。

【环境】仓库 /home/kevin/projects/paleo_workstation。C++20，Qt 6.11.2，
QGIS 4.2.2，GDAL 3.13（gdal.h 直接可用，link -lgdal），
moc=/usr/lib/qt6/moc，QT_QPA_PLATFORM=offscreen。

【第一步：建 worktree】
cd /home/kevin/projects/paleo_workstation
git worktree add ../pw-constgpkg -b wave/constraint-gpkg
cd ../pw-constgpkg

【现状】src/workflow/workflows.cpp 的 ConstraintWorkflow::addConstraint 用
"memory|wkt|wkt|..." 伪源声明 constraints.<horizon> 层 — 重启即丢。
PaleoProjectStore::enqueueWrite 是串行化写队列（读 src/metadata/paleoprojectstore.h）。
§41.2 契约：gpkg 是数据权威，先写 gpkg 再写 qgz。

【交付物】
1. src/io/constraintstore.h/.cpp — 用 GDAL C API 在 project.gpkg 建/追加
   "constraints" 层（字段：id TEXT，horizon TEXT，type TEXT，facies_code INT，
   weight REAL；几何 WKB from WKT）。API：
     class ConstraintStore {
       explicit ConstraintStore(const QString &gpkgPath);
       bool append(const QString &horizon, const QString &id, const QString &wkt,
                   const QString &type, int faciesCode, QString *error=nullptr);
       QVector<QVariantMap> load(const QString &horizon=QString()) const;
       bool remove(const QString &id, QString *error=nullptr);
     };
   所有写路径只能经调用方传入的 store->enqueueWrite 包裹（你的类内部不要
   直接开 GDAL 写；构造注入一个 enqueue 函数指针即可，
   参考 io/dataimportservice.cpp 的 enqueueWrite 用法）。
2. src/workflow/workflows.cpp — addConstraint 改走 ConstraintStore：
   geometry WKT 写 gpkg → manifest 声明改指
   "<gpkg>|layername=constraints|subset=horizon='X'"；新增公共
   loadConstraints(horizon) 从库恢复进内存列表（供 ValidationWorkflow/UI 用）。
   workflows.h 只许加 loadConstraints 声明 + 必要的接线 setter，
   不改现有签名。
3. tests/tst_constraintstore.cpp — 真实栈（projectSvc+manifest+layerSvc+store+
   workflow）：addConstraint×2 → QTemporaryDir 整个销毁重建 → 新对象
   loadConstraints("T1") 拿回两条；remove 掉一条后 gpkg 里确实没了；
   声明的 manifest 层 source 指向真 gpkg。

【纪律】TDD 红→绿；/tmp standalone 编译。不碰 CMakeLists.txt、mainwindow、
panels（父层收尾）。在 worktree 分支 git commit（仓库惯例 trailer），
报告测试数+文件清单+集成注意。
