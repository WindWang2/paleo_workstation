# 井附件图片道深化（方向 79）

日期：2026-10-09　分支：`goal/imagetrack-deep-20261009`
前置：#265/#268（图片道按 depthMd 锚挂进综合柱状图与连井剖面）。

本文钉死三份契约：锚深元数据的**文件名惯例与后补编辑语义**、装载
**LOD 口径**、井附件**管理面板职责**。判据与决策过程见仓内 ledger
`.goal-loop-ledger-imagetrack-deep.md`。

## 1. 锚深契约（catalog 元数据）

- **文件名惯例（导入侧，不变）**：`<井名>,<深度>m.<ext>`，如
  `A1,1849.35m.JPG`；正则 `(\d+(?:\.\d+)?)\s*m(?![A-Za-z0-9])` 只认
  「数字 + m」——无单位深度不猜（薄片照片留在未锚定，待后补）。
- **元数据位置**：`version.extra["depthMd"]`（米，>0）。
- **来源标注**：`extra["depthMd#source"]`——`filename`（导入提取）/
  `manual`（后补编辑）。
- **审计面**：`extra["depthMd#history"]`，数组 FIFO ≤8 条，条目
  `{v: 旧值, had: bool, at: ISO-UTC}`（had=false 表示此前无锚——补锚
  场景 v 省略）。
- **更新通道**：`DataCatalog::updateVersionExtra(versionId, key, value)`，
  **就地更新**——不新建版本、不动 versionNumber/path/sha256/
  parentVersionIds。定案判据（审计面与撤销语义）：
  1. 锚深是导入元数据修正，非派生结果——落 DERIVED 新版本会 fork
     血统（元数据修正无血缘意义）；
  2. 图片道读 `currentVersion`（最高 versionNumber）——手工 DERIVED
     副本会与后续 RAW 重导入竞争版本号，读取面语义复杂化；
  3. 撤销 = 用 history 旧值再调同一 mutator（对称操作），不需要版本
     回滚机制。
- **清锚**：`value` 为无效 QVariant = 删键（`#source` 一并清，
  `#history` 保留）——图片回到「未锚定」态，面板如实列出。
- **手工输入口径**：输入面单位固定米；接受有限正数（容忍 `m`/`米`
  后缀与首尾空白）；非有限 / ≤0 / 未知单位后缀拒收并给结构化 reason
  （`WellAttachmentOps::parseDepthInput` → `reasonText`）。空输入不是
  清锚（误触防线）——清锚必须走显式按钮 + 确认。

## 2. 装载 LOD 口径（src/services/imagelod.h，LodPolicy）

两级装载，所有消费方（连井剖面 / 综合柱状图 / 井附件面板 / 锚深
对话框）共用同一装载面：

| 级 | 内容 | 驻留 |
|---|---|---|
| 缩略级 | 最长边 256px（面板清单 64px） | 内存常驻 |
| 原图级 | 全分辨率 | 磁盘；按需全载 + LRU（8 张） |

- **解码期降采样**：`QImageReader::setScaledSize`——装载内存峰值 =
  缩略字节而非全图字节（JPEG 走 libjpeg 1/2^n 快速路径）。验收比率门：
  200 张 1024² 夹具，Σ`sizeInBytes`（LOD）/ Σ（全分辨率）< 0.10
  （理论 256²/1024² ≈ 0.0625）。
- **全载触发**：道内可见宽（场景宽 × 视图缩放 lod）> 缩略宽 × 2.0
  （`kFullLoadFactor`）。放大检视是单张行为——绘制路径同步全载一次
  进 LRU；批量装载路径永不触发原图级。
- **EXIF Orientation**：`QImageReader::setAutoTransform(true)` 统一
  应用（装载路径唯一——旋转样张在道内正立是本策略行为面）。
- **场景位图缓存**：QImage→QPixmap 转换缓存按**代际失效**——
  `imageVersion` 变化整体清缓存，键只编码 (wellIdx<<32)|anchorIdx。
  旧实现把 imageVersion 截 8 位编进键，256 次数据变更后假命中
  （回归测试 `tst_wellsection_imagetrack::cacheKeySurvivesVersionWraparound`）。
- **透明图棋盘底**：中性灰固定双色（#EEE/#BBB，6px 格）——纸面图件
  口径，不随 UI 暗色翻转。缩小绘制统一 `SmoothPixmapTransform`。

## 3. 井附件管理面板（ui/pages/wellattachmentpanel）

- **入口**：导航树「岩心照片 (N)」分支双击；剖面图片道双击单张照片
  （打开锚深对话框——预览 + 深度输入）。
- **清单列**：缩略 / 文件名 / 锚深（行内可编辑）/ 来源（文件名·手工）/
  角色（岩心·薄片）/ 版本（v-id #N + stage/受管提示）/ 状态（已锚定·
  未锚定·未决关联）。未锚定照片**如实列出**——正是后补编辑的入口面。
- **编辑**：行内编辑（非法输入拒收列因 + 回滚）与对话框编辑（预览 +
  输入 + 清锚按钮）共用 `WellAttachmentOps` 校验。
- **删除**：软删意图信号回 DataListPanel 命令栈（BatchRemoveCmd——
  一次落盘、可撤销、进可回收清单）；**版本级 catalog 操作，磁盘文件
  不动**（测试断言文件仍在）。

## 4. 行为回归面

- `tst_catalog::updateVersionExtraContract`——就地更新/审计/清锚/
  拒收/round-trip/journal 重放。
- `tst_imagelod`——缩略几何（等比、只缩不放）、200 张内存比率门、
  EXIF 样张（手工注入 APP1 Orientation=6：尺寸互换 + 像素采样）、
  LRU 封顶、棋盘模式。
- `tst_wellattachmentpanel`——面板 CRUD offscreen 全绿（三态清单、
  round-trip、拒收回滚、软删不动物理文件、重开落盘）。
- `tst_wellsection_imagetrack`——缓存键 256 次假命中回归、LOD 放大
  全载（缩略/原图两级来源像素断言）、锚双击回调。
- 既有回归零改动：`tst_import`（图片锚面）、
  `tst_wellsection_workflow`（collectCoreImages 全链）。
