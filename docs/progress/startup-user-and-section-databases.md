# 用户坐标库及连井剖面库初始化（2026-10-24）

现场日志：`no such table: tbl_srs`、`WellSectionStore 未绑定工程库路径`，以及由后者
引起的“连井剖面编辑库打开失败”“解释岩性来源读回失败”。这是两条不同的初始化缺陷，
无需改写层位/地震数据或重新创建工程。

## 用户坐标系数据库

只读核对表明，系统资源 `srs.db` 有 `tbl_srs`（13720 条记录），但 Paleo 当前 profile 的
`qgis.db` 为 0 字节。此前嵌入式启动只调用 `initQgis()`，没有调用用户数据库创建/迁移。

修复在 prefix 就位后显式调用 `QgsApplication::init()` 解析 profile，再调用
`QgsApplication::createDatabase()`，最后呈现启动界面并加载 provider。嵌入模式下构造
`QgsApplication` 并未完成 profile 初始化，过早调用迁移会访问工作目录里的相对
`qgis.db`；因此顺序本身是修复的一部分。

使用 [QGIS 原生 createDatabase 迁移](https://github.com/qgis/QGIS/blob/final-4_2_0/src/core/qgsapplication.cpp#L2080)
补齐空库并保留已有自定义 CRS，不删除、替换现有用户数据库。迁移失败时拒绝继续启动，
诊断包含实际库路径和底层原因；坐标运行报告新增 `userDatabase` 路径。

## 连井剖面编辑数据库

面板在无工程时就用空 `metaDbPath` 构造、打开、读取存储，造成日志中的两条警告。
原有绑定又依赖 catalog 路径变化和 300ms 去抖，首次打开工程后立即编辑可能尚未写入正确库。

修复把存储绑定抽为独立方法：

- 没有工程库路径时保持空绑定，编辑仅驻内存；不尝试打开或读回数据库。
- `projectOpened` 在 AppContext 完成工程路径绑定后立即恢复存储和编辑状态。
- `projectAboutToClose` 取消在途请求、解除 fence/workflow 指针、释放存储并清理解释选择。
  关闭期间不会因 store 残留的旧路径而重绑旧库。
- 同路径同读写模式复用实例；新建实例在 open 前继承工程只读模式；打开失败不保留半绑定指针。

## 验证范围

`tst_startupdatabases`：**6 passed / 0 failed / 0 skipped**，包括初始化/清理计数。
使用实际 AppContext、QGIS 用户数据库及主窗口绑定流程：

- 0 字节用户库的迁移、旧查询 `select srs_id from tbl_srs where srs_id >= 100000` 可执行。
- 自定义 CRS 行在再次原生迁移后保留。
- 损坏库在独立测试子进程中拒绝启动，并输出带路径的明确诊断。
- 无工程装配及编辑不再产生未绑定库/解释读回警告。
- 打开 A 工程立即保存、关闭后编辑不写 A、创建 B 后只写 B，A 的记录保持不变。

修复前的隔离测试复现了缺失 `tbl_srs` 和首次打开工程立即编辑未落库两项失败。
本次在隔离目录重编修改的翻译单元并链接静态库副本，未进行完整应用重新构建或替换
正在运行的用户程序。测试使用私有配置、注册表沙箱及临时工程；真实用户数据库只读检查。
无界面测试仍会输出既有 OpenGL 平台及局部 ENGCRS 的 PROJ 诊断，未扩大到这些独立问题。

严格分层、文件预算、UI 大小/tokens/invariants、i18n、文档漂移及 diff 空白检查通过；
拆分后的 attachWellSection 函数预算由 477 行收紧至 433 行。
