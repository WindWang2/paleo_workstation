# Ledger — goal/imagetrack-deep-20261009（方向 79：图片道深化）

工作树 `.worktrees/imagetrack-deep`，基线 origin/master `f31ee461`
（任务书行号口径 `e3c8d31d`，两点间仅 #297 open-issue 批，图片道相关面未动，
行号微移已对照）。

## 契约定案（无人值守决策，判据：审计面与撤销语义）

1. **锚深更新走就地 update，不落 DERIVED 新版本**。
   - 判据：锚深是导入元数据修正，不是派生结果。新版本会 (a) fork
     parentVersionIds 血统（元数据修正无血缘意义）；(b) 让 currentVersion
     （最高 versionNumber）读取面复杂化——后续 RAW 重导入要跟手工 DERIVED
     竞争版本号；(c) 撤销语义变成「回滚到旧版本」而非对称的字段改回。
   - 审计面：改前值压入 `extra["depthMd#history"]`（FIFO ≤8 条
     {v,at}），`extra["depthMd#source"]` 标 "filename"（导入）/ "manual"
     （后补）。撤销 = 用 history 里的旧值再调同一 mutator（对称操作）。
   - mutator 形态：通用 `updateVersionExtra(versionId, key, value)`
     （value 无效 QVariant = 删键），depthMd 是第一个消费者；journal 记
     UpdateVersionExtra op（id+key+value），重放重算 history（同基线
     mutationSeq ⇒ 确定性成立——两次连改的历史数组重放结果一致）。
   - 键保留字：'#' 前缀（`<key>#history` / `<key>#source` 审计面）不许
     作用户 key。
2. **手工输入口径**：对话框单位固定 m，接受有限正数（可带 m/米 后缀，
   容忍首尾空白）；非有限/≤0/不可解析 → 拒收并给结构化 reason（视图层
   tr() 出文案）。「无单位」在手工语境 = 空输入清锚（回到未锚定态，面板
   如实列「未锚定」）——与导入侧「文件名无单位不猜」不冲突：手工输入
   的单位由对话框固定，不存在猜测。
3. **LOD 口径（LodPolicy 钉死在 src/services/imagelod.h）**：
   - 两级：缩略级最长边 256px，装载即生成常驻内存；原图驻磁盘按需全载。
   - 全载触发：道内可见宽（场景宽 × 视图缩放）> 缩略宽 × 2.0。
   - 解码期降采样（QImageReader::setScaledSize）——装载内存峰值 =
     缩略字节而非全图字节；JPEG 走 libjpeg 1/2^n 快速路径。
   - EXIF Orientation 统一 QImageReader::setAutoTransform(true)（装载
     路径唯一：剖面/综合柱状图/面板/对话框共用）。
   - 内存断言口径：夹具 200 张 1024×1024，Σ QImage::sizeInBytes，
     LOD/全分辨率 比率门 < 0.10（256² /1024² = 1/16 ≈ 0.0625 + 头部
     余量）。禁绝对 MB 墙（任务书纪律）。
4. **场景缓存键**：代际失效替代键内版本编码——`m_cacheVersion !=
   st->imageVersion` 整体 clear，键 = (wellIdx<<32)|anchorIdx（64 位，
   无截断）。修掉旧 8 位 version 截断在 256 次变更后的假命中。
5. **面板删除**：软删（dataops RecycleBin 同语义——版本级 catalog
   操作，磁盘文件不动）。断言：删除后文件仍在磁盘。

## 环境

- pwsh 7 必需（powershell 5.1 解析 PS7 语法失败）；cmake 需 3.28+
  （本机 `C:/Qt/Tools/CMake_64/bin` 前置 PATH，paleo-dev.ps1 从 PATH
  取 cmake）。
- 视图→workflow include 有先例（versiondialog.h 引 assetops.h）——
  depthanchordialog/附件面板可直接用 workflow 编排类。

## 轮次

| 轮 | 改动 | 验证 | 结果 | 下一步 |
|---|------|------|------|--------|
| 0 | worktree 接线 + 全新构建 | build 进行中 | - | 基线 ctest R0 红集合 |
| 1 | 环境：pwsh7 + CMake 3.29 PATH 前置；首构被中途编辑污染 → 停掉重建；R0 改为收尾 git stash 对照法（同机同时刻更诚实） | build-r1 失败：imagelod 缺 `<QHash>`（C2079） | 修正后 build-r2 进行中 | 等 r2 绿 |
| 2 | 批1-4 全量落码：catalog updateVersionExtra（就地+审计+journal op）+ 导入 #source + wellattachmentops（校验/清单）+ depthanchordialog + 树「编辑锚深…」+ 剖面锚双击回调 + 场景 LOD/代际缓存键/棋盘/平滑 + composite 缩略装载 + 井附件面板 + 树分支双击跳转 + 壳层接线；测试 4 件（tst_catalog 契约 / tst_imagelod / tst_wellattachmentpanel / tst_wellsection_imagetrack）；文档 docs/progress/well-image-track.md | 待构建 | - | 构建→ctest→修 |
| 3 | 构建排障：r2/r3 中断根因=先前 TaskStop 留下的孤儿 ninja/cl 并发写 pdb（静默崩溃无 error 行）——taskkill 清光后干净续跑；r4 暴露真错误：QImageReader 不可拷贝/移动，按值返回的读取器工厂不成立 → 就地构造 | r5 构建中 | - | 等 r5 |
| 4 | layering --strict 绿；diff 自审第一轮：修 datalist_batch 意外缩进、wellattachmentops 加 null catalog 守卫 | 静态 | 通过 | 等 r5 → ctest |
| 5 | 环境收口：worktree 缺 gitignored vendor/onnxruntime → junction 共享 hub-merge 的副本（BUILDING.md worktree 节口径）；moc 陈旧态 → 清 build 全新构建 r9 全绿（2007/2007）；测试沙箱：TEMP/TMP 必须指 build/paleo-tmp（ps1 test 内置） | r9 绿 | 通过 | 跑测试 |
| 6 | 测试修绿：EXIF 样张改走 QImageWriter::setTransformation（手工拼 APP1 被 Qt 忽略+告警——废弃）；场景 LOD 键补层级位（缩略缓存短路放大升级——真缺陷）；catalog 同值重写真 no-op（不涨 revision 不留 history 噪声）；面板 itemChanged 再入 UAF → 编辑 deferral（QTimer 0ms）；面板测试模态确认走 tst_panels 的 driveModal 先例（点 YesRole 按钮——accept() 的 result 是 QDialog::Accepted≠QMessageBox::Yes）；十进制格式对齐两位 | tst_imagelod 7/7 ✓、tst_catalog 全套 ✓、tst_wellsection_imagetrack 4/4 ✓、tst_wellattachmentpanel ✓、tst_wellsection_workflow 零改动 ✓ | 通过 | tst_import 一红待 R0 对照（previewFolder 排序——本方向未触碰路径）；全量 R1 跑着 |
| 7 | i18n：lupdate(6.11, MSVC env, 生成 .pro) 全量跑通但 26K 行 location churn → 回退；本方向新串被 same-text 启发式自动填平（中文源=同文），.ts 收口留给标准脚本（PR body 记账）；二轮自审：对话框标题带井名（待全量后重编） | - | - | 全量 R1 → R0 对照 → 提交 |
| 8 | 全量 R1：25 红/372；全量 R0（stash 基线 + 全新构建）：24 红/369。diff：R1 多 tst_correlation_full、tst_perfbudget；R0 多 tst_errorhub_storm。基线单跑复核：tst_perfbudget 红（folder enum 533ms>500ms 机器抖动）、tst_errorhub_storm 红（maxLag 121ms>50ms 机器抖动）——实锤环境 flaky；tst_correlation_full 基线单跑绿、与 diff 无文件交集，R1 红为满载 flake（待 R1 构建单跑复核） | 红集合实质 diff = 空（三差全为机器抖动类） | 通过 | unstash 复核 |
| 9 | **事故与恢复**：stash pop 只恢复了未跟踪文件（"stash entry is kept"），未细看即 drop——跟踪文件 20 项改动丢失。凭 drop 打印的提交号 `fbd8517c` 从对象库 checkout -- 恢复全部跟踪文件（含 stash 前最后两处小改）。教训：pop 输出含 "kept" = 部分失败，必须 status 核对全量后才可 drop | 恢复后 grep 验证 5 项关键内容 ✓ | 恢复 | 重建 → 复跑 → 提交 |
| 10 | 恢复重建 r12 全绿（445/445；中途 1 错：恢复版 lambda 首参匿名缺 wellId——命名修复）；验证组两遍全绿：tst_imagelod / tst_wellattachmentpanel / tst_catalog（全套）/ tst_wellsection_workflow（零改动）/ tst_wellsection_imagetrack + tst_correlation_full 单跑绿（R1 满载 flake 定性收口）；layering --strict 终态绿；测试残留清理（fixture.gpkg 还原 + 乱名 json 删除） | 5/5 × 2 遍 | **Oracle 5 条全过** | 原子提交 ×4 |

## 终验（Oracle 对照）

1. **锚深编辑 round-trip** ✓——`tst_catalog::updateVersionExtraContract`（就地不 fork 版本/#history/#source 审计/清锚/FIFO 封顶/sqlite 重开 round-trip/journal 重放）+ `tst_wellattachmentpanel`（面板行内编辑 round-trip）。拒收列因：NotFinite/NonPositive/BadUnit/Clear 四态 reasonText，面板状态行出文案。
2. **LOD** ✓——200 张 1024² 夹具 ΣsizeInBytes 比率 < 0.10（`memoryRatioGate200`）；放大触发全载（`lodFullLoadOnZoomIn`：缩略蓝 → ×2 后原图黄，两级来源像素断言 + LRU 驻留断言）；缓存键假命中回归（`cacheKeySurvivesVersionWraparound`：v+256 换图画新图）。
3. **EXIF/透明** ✓——`exifOrientationApplied`（QImageWriter::setTransformation 落 orientation 标签的样张：尺寸互换 + 上下半像素采样）；棋盘底（`alphaCheckerboardPattern` 模式断言 + 道内/对话框/composite 三处填充）。
4. **面板** ✓——`tst_wellattachmentpanel` CRUD offscreen 全绿；未锚定/已锚定两态如实（unresolved 链接 entityId 为空不属井——排除探针断言）；删除=软删意图信号 + 磁盘文件保留断言。
5. **回归** ✓——tst_import 图片锚面绿零改动（`wellAttachmentLinksCoreCuttingsLabAndReference` PASS；整件红=previewFolder 排序，R0 同款基线红）；tst_wellsection_workflow 全绿零改动；全量 R0（stash 基线全新构建）24 红/369 vs R1 25 红/372，实质 diff 空——三处差异（perfbudget/errorhub_storm 基线单跑亦红=机器抖动；correlation_full R1 后单跑绿=满载 flake）。

## 遗留（PR body 记账）

- translations/paleo_zh_CN.ts 未随本分支更新：Windows 侧 lupdate 全量跑通但产生 26K 行 location churn（污染 PR），本方向新串（中文源）已被 same-text 启发式自动填平、无功能影响——留标准脚本（tools/update_translations.sh，Linux）收口。
- 面板/对话框新串约 60 条走 tr()，.ts 骨架下次统一扫描进账。
