# Fix 2 结果（full-audit-20261021）

独立复审（06-independent-review-r2.md）意见的逐项处置：

| R2 意见 | 分类 | 二次修改 | 测试证据 | 状态 |
|---|---|---|---|---|
| MF-1 重映射不避让同批 journal 自有 id | MUST_FIX | mapexport.cpp 分配循环改 live/journal 双避让（QSet 预扫 journal Add* id，nextAssetId/nextVersionId 循环跳过） | tst_exportpipeline 扩展：双格式（同 journal 两个 AddAsset）+ 噪声只消耗 ast-1 的红形状——未撞号原号保留、撞号映到 ≥ast-3、双资产全部可解析（修复前重放 duplicate 整批失败）；修复前红/修复后绿由反例推演与代码路径双证 | FIXED fc712cf2 |
| SF-1 厚度链 publish 不消费 idMap → decl.source 死链 | SHOULD_FIX | mappingworkflow 传 idMap + 声明源路径 ast-N/ver-N 段随映射改写 | tst_workflows（厚度链回归面）绿；路径段改写逻辑与 publish 内 rewritePathSegments 同构 | FIXED fc712cf2 |
| SF-2 B-2 线号距离冒充米（两支） | SHOULD_FIX | BuildLineSection 回落（1d28646f）+ 终审结转的引擎路径仿射不可得 else 分支（F-1，Q_UNUSED 弃用不外送） | 注释留档 + 终审复核 | FIXED 1d28646f + 终审补丁 |
| SF-3 逐道族缺终填 | SHOULD_FIX | join 后补 reportBytes(totalUnits,totalUnits)（与相干分支同款） | 既有属性任务测试面绿；取消路径早退不受影响 | FIXED 1d28646f |
| SF-4 重复 include | SHOULD_FIX | 删 sgycoordinatemapper.h 重复行 | 编译通过 | FIXED 1d28646f |
| A-01 锁序疑虑 | 深查后无问题 | —（复审确认最终版无嵌套取锁） | 死锁曾在中间版出现并已修复（flushDisk 拆公共/持锁两入口）；终审复核维持 | CLOSED |
| shacache.h 头注措辞 | DOCUMENT_ONLY | 对齐实现（换代后首 miss 立即落盘；invalidate 强制刷） | — | FIXED 1d28646f |
| B-1 注释数值质疑 | FALSE_POSITIVE | 数值复算：2π（≈6.28）代替 π²（≈9.87）→ 零点半宽 1/(2f√π)≈11.28ms、等效主频 √(2/π)≈0.798×——原注释正确；复审按 2π²（≈19.7）情形误算 | python3 复算留档；注释补防误读括注 | CLOSED |
| E-2 基线期 identifytool +2 已红 | EXTERNAL_OWNED | 维持册外（#604 在修） | 闸后状态与 master 同面 | CLOSED |

## Fix 2 验证

- 定向：tst_exportpipeline（含 MF-1 扩展）/ tst_seismic_index / tst_seismic_welltie /
  tst_mappingworkbench / tst_workflows 全绿。
- 全量：见 08-final-independent-review.md 与终局对账（12 红全为基线已知在册，零新增）。
