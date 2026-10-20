# 预测成果默认保持微相（2026-10-20）

测井相预测、地震相预测（含本地 mock）的编码原本已是微相，版本元数据
也已有 `prediction_level=micro_facies`。主图却沿用普通相图的自动比例尺
显示规则，在中小比例尺聚合为亚相/相，造成预测级别与图面不一致。

数据层 `FaciesHierarchy::productDisplayMode` 统一规定：两种预测产品的
空/auto 模式解析为 micro_facies。新成果写入固定微相的 display_mode；
旧图层在样式恢复时同样迁移。主图与 MappingReferenceCanvas 共用该规则。
保留显式手动级别选择，普通相图继续使用原来的比例尺显示规则。

验证：

- 井点和地震栅格的旧 auto 成果在 1:1,000、1:100,000、1:10,000,000
  下均保持微相；检查主图、图例和参考视图。
- 实际地震 mock 计算流程的新成果保存 display_mode=micro_facies。
- 测井预测新成果保存微相模式，井段审查与持久化回归通过。
- 普通相图和参考视图的自动级别切换回归保持通过。
- 上述五项 QtTest 共 7 passed（含 init/cleanup），无失败；日志
  `build/microfacies-tests.txt`。layering strict 和 diff whitespace 通过。
- 当前用户工程的已实例化预测图层写回微相样式，再次打开检查全部图层
  有效；未打开的层位按已有声明保留，首次载入时由产品规则恢复微相。
  写回前的工程备份在 `build/microfacies-project-backup/`。

当前工程已有的两份 D61 预测均已更新；全部 11 个图层的数据源保持一致，
最终 QGZ 重新打开验证通过。原生 QGZ 打包报告 Unable to perform zip 时，
改为由 QGIS 导出已验证的 QGS，将两个预测图层的渲染器、标注、显示属性
写回原 QGZ；其它 ZIP 成员逐字节保持不变，再原子替换。
日志：`build/microfacies-project-save.log`、`build/microfacies-project-reopen.log`。

便携版两处程序已更新并核对与编译产物 SHA-256 一致：
`c8766f7f4f19147b8594cb3ad8ce53305f2641db13f26eea14298e2ed1b4ede1`。

原始测井、预测编码、栅格数值和工程底图均未改写。
