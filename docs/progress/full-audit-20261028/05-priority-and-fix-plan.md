# 修复优先级与计划（Phase 2）

修复顺序（根因修复，每簇独立 commit，先写红测再修）：

1. **F-01**（P1）paleotaskservice 层忙条目各自释放 —— 删计数闸，每任务终态无条件
   releaseLayerBusy 自己；回归测试：同层两任务先后终态 → layerBusy 最终为假。
   tst_taskservice。
2. **F-02+F-03**（P2/P3）aichatcontroller 空 id 不参与去重 + 恢复 distinctCalls 快照迭代。
   tst_aichatcontroller / tst_aichattoolloop。
3. **F-04**（P3）dataopsimportui runPending 尾部 pending 非空时确保驱动拍在转。
   tst_importqueue_progress。
4. **F-05**（P3）mappingartifactwriter 失败路径删 .partial。
5. **F-06**（P3）trackops CSV 全自由文本走 csvCell。tst_wellcomposite。
6. **F-07**（P3）constraintstore copyAudit 判终改「整页无有效行」。tst_constraintstore。
7. **F-08**（P3）localidw k 两分支统一 min(·, kMaxByMatrixBudget)。
8. **F-09+F-10+F-11+F-21**（P3 族）坐标工具族：tooltip 双分支、包装随内层对话框
   销毁、顶替前广播 cancelled、小数位按 CRS。tst 见现有 qgis/ui 测试。
9. **F-12**（P3）wellattributestore 索引保持首行命中 + addFeature 后登记新 fid。
   tst_wellattributes。
10. **F-13**（P3）polygonize compute 捕获 QPointer + 判空失败。
11. **F-14**（P3）factor 同步路径 publish 失败补 cleanup。
12. **F-15**（P3）llmclient SSE data 行 \n 连接。
13. **F-16**（P3）previewdoc_xml 64 表软上限（capped 旗标 + issues 留痕）。tst_previewdoc。
14. **F-17**（P3）faciesqa 检测器 5 补包围盒预筛。
15. **F-18**（P3）upscale 域界外跳过留痕（qWarning）。
16. **F-19**（P3）previewwellbases sha 前置快失败 + stat 变化才复验。
17. **F-20**（P3）rasterpyramid tile 锁内重算网格 + 直读路径复验。tst_cache_pyramid。
18. **F-22**（P4）基线头注拼写（随最近 commit 捎带）。

每簇：红测（可行处）→ 最小修改 → 定向 ctest → 同域回归 → commit。
基线对照纪律：master 本身失败的测试不视为本轮回归（#774 在册）。
