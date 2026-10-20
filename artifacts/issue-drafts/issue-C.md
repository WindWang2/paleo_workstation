## 问题

Windows 车道 2 项真红，两处都是「删除失败」，指向**文件句柄 / 数据集未释放**（非负载抖动：并行轮失败，直接重跑仍失败）。

## 证据

run 38835970978（windows job，388 项 → 2 failed）：

```
FAIL!  : TestMappingWorkbench::removeDraftDeletesWorkingCopy() '!QDir(workDir).exists()' returned FALSE. ()
FAIL!  : TestVersions::savedBytesAndPdfSurviveWorkingChanges() 'QFile::remove(gpkg)' returned FALSE. ()
```

- 门禁判定：`UNEXPECTED tst_mappingworkbench: 不在环境债名单` / `UNEXPECTED tst_versions: 不在环境债名单` → `env_redset: 2 红 → 0 豁免 / 2 不放行` → job failure。
- 非偶发：10-15 run 38756099683 的 windows job 同样 `failure`。
- 用例位置：`tests/tst_mappingworkbench.cpp:3097`、`tests/tst_versions.cpp:441`。

## 影响

- 若是**产品侧**句柄未释放，则「删除草稿/工作副本」「版本文件覆盖」在真机 Windows 上可能同样失败，是用户可见的行为缺陷，不只是测试问题。
- 直接构成 Windows 车道红灯面（本批 A 的子项）。

## 建议

1. 先判定归属：在上述两条执行路径上枚举仍打开的 GDALDataset / CPL 句柄与 QFile / QLockFile（Windows 可用 handle.exe / Process Explorer 复现），确认是产品代码未释放还是测试夹具未关闭。
2. 产品侧：派生工作副本（GeoPackage）与栅格数据集在删除前显式 close / unregister；版本覆盖路径同理。
3. 测试侧**不要**用「重试等待删除成功」绕过——那会掩盖真实的句柄泄漏。

## 验证

windows 车道全绿，且两个用例连续 3 次复跑稳定通过。

## 关联

父项：本批 A；同族：#461（原子写落盘）
