# TEST-06 剩余零测试头终态清单（方向58，origin/master f130fb2）

口径：`src/**/*.h` 共 445 个、`tests/**` 328 个文件。「直接零引用」= 无任何测试文件 `#include` 该头（按文件名）：45 个；其中 14 个经被测头**传递包含**可达，剩 **31 个完全不可达**。
扫描脚本见账本 R5（纯文本 include 图，不含链接期覆盖；「不可达」≠ 实现未被执行，只说明无测试编译单元引到该声明）。

## A. 不计入（13）
- 6 行转发垫片（5）：`domain/seismic/{axisdescriptor,sgyreadsession,sgysequentialscan,sgywelllocator}.h`、`io/segysectiongrid.h`——仅 `#include` vendor/sbm 头，**不值得**单测（vendor 侧 SDK 测试承接）。
- `_internal.h` 实现细节头（8）：`workflow/{constraintfactorjobs,constraintworkflow,workflows,workflowerrors}_internal.h`、`io/dataimport_internal.h`、`services/seismictaskservice_internal.h`、`algorithms/welldistance_internal.h`、`ui/datapreview/datapreviewtabs_internal.h`——经公开 API 测试执行，**不值得**直测（直测会把实现细节钉成契约）。

## B. 低风险剩余 18 项及终态建议

| 头 | 层 | 建议 | 理由 |
|---|---|---|---|
| io/attrgridout.h | 数据 | **待补** | 时间切片 GeoTIFF 写/读摘要，纯 GDAL 可子集单测，低成本 |
| algorithms/singlefactor/cartographicworkfile.h | 算法 | **待补** | 制图约束解析/写出纯函数，9 处使用 |
| ai/chat/llmkeystore.h | AI | **待补** | 密钥存取，安全敏感但面小 |
| workflow/mappingsamples.h | 功能 | **待补** | 制图样本组装，纯数据变换 |
| ui/pages/datalistops.h | UI | **待补** | UndoRecord/空态刷新逻辑，非纯视图 |
| ui/pages/versiondialog.h | UI | **待补（仅 versionSizeText 等纯函数）** | 对话框本体不测 |
| qgis/qgisconstrainteditsession.h | QGIS 封装 | **待补** | 约束编辑会话（临时目录+工程存储），需 QGIS 运行时 |
| algorithms/singlefactor/localdirectionalgorithm.h | 算法 | 不值得 | Processing 薄封装；核心引擎与 workflow 端到端已测 |
| algorithms/singlefactor/structuralalgorithm.h | 算法 | 不值得 | 同上（structural.h 已由方向52 测） |
| ai/onnxlocalpredictor.h | AI | 不值得（归 ai 方向） | PALEO_HAVE_ORT 条件编译 |
| selfcheck/perfgroup.h | 自检 | 不值得 | BenchResult 结构体，selfcheck 运行即覆盖 |
| ui/pages/assetentitychoicemodel.h | UI | 不值得 | 薄 QAbstractListModel |
| ui/faciesmapping/faciesmappingpanel.h | UI | 不值得 | 视图壳，意图信号经 workflow 测 |
| ui/dialogs/griddingdialog.h | UI | 不值得 | 参数表单壳 |
| ui/pages/importledgerdialog.h | UI | 不值得 | 只读台账视图 |
| ui/pages/pendinglinkdialog.h | UI | 不值得 | 视图壳 |
| ui/maptools/sitingpicktool.h | UI | 不值得 | 地图工具，需交互画布 |
| ui/pages/stratigraphicwebpage.h | UI | 不值得 | WebView 页壳 |

合计「待补」7、「不值得」11。本方向**不强行补齐**；待补项登记为后续方向输入。
