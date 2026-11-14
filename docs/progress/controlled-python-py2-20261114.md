# 受控 Python PY2：有界井曲线分析

本批依赖 PY1 和 SDK 配套 [Data_service #21](https://github.com/changyanyanchang/Data_service/pull/21)。
主仓基底为 `17bdfd0f`，SDK 固定为 `9d80d1f6bbe9e1e3a71ff65d608f5290aeb75b14`。
完整应用和最终原生运行验收均已完成，WSL 拒绝执行的结果与原生成功结果分别记录。

## 行为与数据边界

新增 `paleo.well_curve_quality`、`paleo.attribute_correlation` 两项固定模板工具。
生产 app 将工具绑定到同一个 CapabilityHost、DocumentSession 和 RuntimeProvider；
输入来自 `query_project(topic=well_curves)` 的精确曲线引用。宿主解析模型位置、井映射、
来源版本与 SDK head，工具参数不接受文件路径、解释器、策略或效果批准。

当前仅支持已声明的 MD 轴和 m/ft 单位，每通道保留原单位，轴必须完全一致；
不自动插值、换算单位或推断深度语义。每次最多 16 通道、每通道 8,192 样本，
SDK 数组和实际 UTF-8 元数据共同受 8 MiB 读取预算约束。缺失、异常、编辑状态有独立
质量位。读取前、各窗口及末尾核对完整模型 head；旧历史不可读或中途变化明确失败。
首批相关分析是所选井数值窗口的 Pearson 与冗余报告，不宣称完成地震体属性优选或融合。

SDK 查询使用元数据专用窗口和 after-key 页；先检查源记录预算，再取曲线元数据，
不会为了列表读取数组。SQLite 用 `octet_length` 和条件表达式在取出文本之前拒绝超大
字段；UTF-8 必须完整结束。深度语义区分 declared/inferred/unknown，生产工具只接受
declared。旧 SDK 三参数接口和默认读取行为保持，修改头布局的消费者全部重新编译。

私有输入冻结 code/request/data 的内容 SHA，进程消费同一份已认证字节。
质检给出覆盖率、空值/异常/编辑计数、均值、总体标准差和 z-score 诊断；相关分析只用
联合有效行，常量通道返回 null，记录显式采样方法、种子 0 和 selected_window_only。
宿主严格检查重复 JSON 键、完整 UTF-8、有限数值、计数、矩阵对称、来源和报告端口。
仅 REPORT 可发布；输出不超过 256 KiB，聊天摘要不超过 4 KiB，详细结果复用既有认证分页。

准备的 SDK 读取在工作线程执行，使用现有任务服务并先取得进程共享许可；
owner 线程只解析身份和冻结宿主上下文。同步 taskAdded 重入删除宿主/任务/服务、
验证回调更换 adapter/文档和准备中取消均纳入回归。工作线程不能越过失效 token 晚交付。
恢复使用冻结输入，不重新读取当前 SDK head 或依赖旧脚本供货目录；发布复用既有
成组登记、父版本、写 claim、提交事实核对和幂等流程。

环境查询另报告真正的 trusted execution readiness；运行时文件核验与模板存在不再
冒充执行后端可用。固定 NumPy 探针在工作线程运行，工具列表只读非阻塞快照。
缺 CPU controller、缺辅助程序或策略未允许时，新计算如实 unavailable；已有冻结成果
发布不依赖原模板目录。离线脚本包使用独立不可变代际，仍无 Agent 安装入口。

## 已取得的独立证据

- SDK Windows 与 Linux 原生各 7/7，含超大元数据、不完整 UTF-8、旧 API、空数组元数据页。
- 原生数据物化 17/17；两项生产 adapter 的实际运行结果与独立线性参考一致。
  两通道各选 100 样本，按 SDK 实际读取块计 payload 34,816 B、元数据 734 B；sourceRows=1、windows=2、
  axisBlocks=2、valueBlocks=2、legacyBytes=0。它们是 SDK 消费字节，不是物理磁盘计数。
- 元数据曲线页 11/11，三页实际文本分别 340/340/338 B，数值读取始终 0；
  换 SourceResolver 或同一文档重绑会撤销旧游标，初始页中的重入也校验 binding epoch。
- 报告校验原生 Windows 27/27，实际模板用例另计；修复相关矩阵临时 QJsonArray 的
  悬空引用后，密集、全 null、混合矩阵多轮解析回归通过。
- trusted readiness 原生 Windows/Linux 各 8/8；WSL 的预期拒绝另列。
- 新任务准备重入测试在完整应用链接集上 11/11，零失败/跳过。

## 完整编译、运行与源码门禁

默认及 no-ORT 均已完整编译 `paleo` 与 41 个相关 C++ 测试，包含 WebEngine Widgets；
Office 组件已接入构建，配置没有组件缺失警告。锁版 QGIS 4.2.3、GDAL 3.12.2、Qt 6.11.2、
GCC 16.2.1、CMake 4.4.3；默认 ORT 1.30.0，另一套明确关闭 ORT。构建总并行不超过 8。

WSL 两套各运行 45 项。实际执行六套 Python 测试被缺失 CPU controller 拒绝，
这是负例，不算成功执行验收。另发现旧查询工具精确清单遗漏新增两工具，已修正并
重编相应目标。不以旧清单故障或负例结果充当绿灯，最终原生结果见下文。

开 PR 前已 fetch/rebase `origin/master`，CI 清单加补充自检的 Source gates **37/37**；
显式提供锁版 QGIS 头目录，包含 transitive 全指标、严格分层和规模闸，基线无增长。
最终文档收口后重新运行同一清单，PR 依赖 PY1 与 SDK #21，未自动合并任何实现批次。

原始证据在本工作树 `local/python-validation/`、`local/review-20261114/source-gates/`，
完整 build 为 `/home/kevin/build-paleo-python-analysis{,-noort}`。
原生 Linux 私有验证包复用 PY1 已核验依赖闭包，使用独立工作目录和只读干净运行时代际；
保留首轮失败日志。该记录不将 QtCore 原生薄库称为 Windows 完整应用构建。

原生 no-ORT 首轮完整 45 项：44 通过，进程资源套件中的内存用例在并发冷快照时
超时，总计 818.60 秒。该窗口没有观察到 OOM，不能把它改标为内存耗尽。
同一二进制、同一 96 MiB/15 秒策略在虚拟机独占后单用例 3/3，通过 3.037 秒；
旁路读取实际 memory.max=100663296、swap=0、pids=8、cpu.max=100000/100000，
peak=100663296、oom=1、oom_kill=3。随后完整进程套件通过 **18/18**，零失败/跳过，
CTest 用时 18.78 秒。没有修改生产代码、预算、容差或断言。
其余 44 个套件保留首轮完整结果，失败套件保留独占重试结果，不把首轮改写为 45/45。
no-ORT 应用首帧与 startup_screen_closed 已实际出现，Office 组件在位；
运行后 2143 个运行时文件再次完整 SHA 核验通过。

原生默认配置完整 **45/45**，QtTest **613 通过、0 失败、0 跳过**，CTest **372.73 秒**。
no-ORT 的 45 个选定套件保留上述首轮及完整失败套件重试证据，最终 QtTest 汇总为
**607 通过、0 失败、0 跳过**。编译与验证的功能源码为 `4b98bfd9`，其后只有文档收口。
两套运行均使用实际生产 Host/Recovery、Catalog、Agent runner 和 app provider，
覆盖 SDK 发现→可信分析→幂等回放→128 B 认证页→重建宿主→原操作发布与父版本。
六组 `ldd -r`（应用、QtCore、QtGui、WebEngine、QGIS、GDAL）无缺失或未定义符号，
身份为 UID 1000、零 capabilities，CPU/memory/pids controller 均有效。
每套运行前后核对干净、只读运行时代际的 2143 文件精确集、大小和 SHA。
保留默认包 manifest `f076355c37c2793157a60de9c6870bc7dd583a43d533187efe43ef20ecc6cd39`，
no-ORT 包 manifest `b73fb29ed7ab46dbd88ab5e43b12d14df0a5a2b6989af2caf46cb97dfd3c029a`；
完整原始证据在 `local/python-validation/native-full/`。
