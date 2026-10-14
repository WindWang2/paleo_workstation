# 地图版本快照 I/O 与线程风险（静态评估）

本记录基于 `origin/master` b0520429 上的本任务实现。没有运行吞吐或 UI 响应基准；以下是代码路径与读写次数分析，不代表已经达到性能验收指标。

## 实际调用路径

现有 UI 已接 `MapVersionController::startSaveVersion/startPublish`，复用 `PaleoTaskService` 和 `JobRunner` 的 prepare / compute / commit。同步 `saveVersion/publish` 保留作兼容和测试调用，无任务服务时显式退回同步路径。

1. 工程线程 prepare 提交 QGIS 原生编辑、验证图斑 JSON 与身份、聚合状态，并捕获 Catalog 证据和独立的 `QgsVectorLayerFeatureSource`。快照涵盖本层位全部声明图层，包括隐藏图层。活的 `QgsMapLayer*` 与主线程 SQLite 连接不交给 worker。
2. Worker 逐要素流式导出独立 GPKG，随后分块复制、核对来源 SHA / 复制 SHA / 目标 SHA。4 MB 文件块和 256 要素进度批次提供协作取消；仅写入新 UUID 暂存目录。原始 provider 文件在导出前后检查大小与修改时间；临时快照本身完整计算 SHA。矢量不同 subset / 原生 layer 身份不按 URI 合并；文件复制按规范来源路径与内容 SHA 安全去重，保留每个声明引用。
3. 工程线程 commit 验证工程 epoch、路径、版本与来源仍存在，以及已由 worker 完整校验的只读文件大小 / 修改时间 / 非符号链接证明。经项目写队列提交版本或发布状态。发布原子重命名后 SQL 失败会清理新目录。旧 epoch 终态不写新工程 UI，取消释放原工程 ID 的协调 token。

`MapVersionStore::snapshotInputForSave/Publish` 和 `commitPreparedVersion/Publication` 只在 SQLite owner 线程调用；纯值 `MapSnapshotInput` / `PreparedMapSnapshot` 和 `prepareMapSnapshot` 不访问 SQL。发布始终读保存时冻结的文件和证据，工作源后续修改不回写旧快照。证据清单与 PDF 指纹均核查，Blocking 才阻断，Warning 保留。

## 规模与可验证成本

文件总计 B 字节时，新快照通常读取源 SHA、复制读和目标 SHA 共约 3B，写入 B；已有绑定来源 SHA 时省去首次源哈希。发布再读源、复制与目标校验，保持完整 SHA，耗时 I/O 已移入 worker。矢量导出另有 provider 要素读取和临时 GPKG 写入。十万个图斑不会再复制进 Catalog extra，但 GPKG 必须保存业务属性与解释历史。

剩余风险是 owner prepare 的原生 commit、身份/解释预检与状态聚合仍按要素读取，会受十万复杂多边形或长历史影响；`QgsVectorLayerFeatureSource` 的编辑缓冲和 provider 快照构造也有主线程成本。不能据异步文件操作宣称所有保存过程无 UI 冻结。没有运行吞吐和 UI 延迟基准，数值目标仍待测。

自动化保护新增异步保存/发布成功、同源不同 provider subset、工作源变化后发布旧 bytes、取消、工程 epoch 丢弃、SQL 发布失败清理/重试，以及 prepared 文件修改拒绝登记。测试使用队列信号与 semaphore 建立确定性取消点，不使用固定 sleep，也不在 worker 触碰 QtSQL。

## 2026-10-14 本机实测

`tst_mapsnapshot_scale` 在本机单独跑完。点要素，解释历史 1 条或 16 条。每次保存 D61 两次并发布最新版，同时另存 32 个要素的 D62。重开冻结 GPKG 后要素数、`f-0` 的修订号和第一版 provenance 保持不变。取消发生在下一次保存的准备返回之后，四组都没有多出一个版本。

时间是单次观测，不是验收阈值。`ui_gap_ms` 是保存脉冲里最长的一次事件循环间隔，包含所有者线程上的准备扫描。

| 图斑 | 历史 | 准备 ms | 保存 worker ms | UI 间隔 ms | 第二次准备 ms | 发布 worker ms | 取消 ms | 冻结字节 | VmHWM KiB |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 10,000 | 1 | 875 | 185 | 876 | 883 | 9 | 876 | 4,390,912 | 269,596 |
| 10,000 | 16 | 2,899 | 458 | 2,900 | 2,757 | 69 | 2,758 | 41,701,376 | 268,728 |
| 100,000 | 1 | 8,851 | 1,609 | 8,851 | 9,086 | 73 | 8,998 | 43,114,496 | 335,048 |
| 100,000 | 16 | 27,857 | 3,862 | 27,857 | 28,150 | 606 | 27,843 | 416,391,168 | 335,824 |

D62 的 worker 约 19–21 ms。发布准备本身是 0 ms，因为发布读的是已冻结文件。界面停顿来自 `captureVersion` 在所有者线程逐要素校验解释 JSON。文件复制在 worker 里，不是这 28 秒的主要部分。不能据此声称保存无界面冻结。
