# 受控 Python PY4：生成数值报告与端到端收口

本批依赖 PY2 #948、Windows 后端 #949、Linux 后端 #950，以及 SDK #21。
Shell 保持后置。功能源码冻结与最终完整矩阵结果在验收结束后补录，不用此前批次的
结果代替本批验证。

## 入口与执行身份

新增 `paleo.python_compute` / `python.generated_compute`，只接受有界 UTF-8 源码与
既有 well_curves 精确引用。代码最多 16 KiB；输入最多同井 16 列、8192 点，
保留声明的 MD 轴、逐列单位、显式范围和质量位。模型不能选择路径、解释器、
运行参数、效果批准或更弱后端。原始资料不被修改，报告不冒充地震体优选/融合结果。

宿主独立控制 `PALEO_AGENT_PYTHON_ENABLE_GENERATED=1`，与 trusted 开关互不授予。
源码、输入、运行时清单和策略按内容身份冻结。新程序每次新建进程，不共享 REPL
状态。生成计算仍需 LocalExecute；prepare/compute-only 只产生私有输入/报告，
publish 只引用原操作并由原有 Host、写 claim、输出集和 Catalog 成组提交授权。

SDK 准备在首次 I/O 前保守计算数值预留：向量、打包副本、受限 SDK 解码 scratch、
控制 JSON 与代码都纳入；不足返回 `python_input_memory_budget`，SDK 读取计数为零。
记录 `hostNumericPreparationReservedBytes` 和 `reservation_not_process_rss`，
它不是 GUI RSS。实际 SDK 字节、子进程峰值及强后端快照预留分别报告，不混称物理磁盘
或整个 GUI 的内存计量。

## 不可信报告与真实就绪

只接受 REPORT 的 `paleo.python-generated-report.v1`：最多 64 指标、4 表、每表
16 列/128 行、总 8192 单元格、16 条有界说明；最终报告最多 256 KiB。
严格拒绝重复转义键、不完整 UTF-8、非有限值、越界形状和额外路径/来源/摘要声明。
来源、代码 SHA、运行时 SHA 和输入列由宿主补入。报告明确标记
`agent_generated_values_not_independently_recomputed`，不把程序自报数值当作独立重算结论。
stderr 和失败 stdout 前缀保持私有诊断，不能转为发布端口。

SandboxProvider 只在工作线程运行；工具列表读非阻塞快照。它用宿主固定源码与
JSON 输入，通过当前强后端实际运行 NumPy 2×2 矩阵乘法，精确核对版本及输出字节。
只有真实成功才置 `ready/runtimeProbeVerified`，缓存绑定运行时 SHA、策略和探针 SHA。
原生接口存在或普通环境导入成功都不能代替这一证明；每次运行还复核身份和 OS 限制。

最后 review 发现普通运行时探测与强隔离之间有校验后替换窗口，本批已修复：
生成模式的准备、worker、StrongProvider、app 初始预热和环境查询使用
`RuntimeProvider::inspectIntegrity()`，只检查清单、文件 SHA 和保留租约，完全不启动
解释器。实际版本/NumPy 验证发生在锁定或密封的快照内。环境查询以
`runtimeVerification=content_identity` 区分身份状态和强执行状态。
旧 trusted 接口保留原语义，GeneratedOnly 不能绕入它。

安全回归不运行被修改的模块：使用临时目录的 7 字节、非可执行格式文件，证明
身份核验不要求解释器可运行；在 hash 后的目录遍历回调中将其改成另 7 字节，
随后强后端重新认证必须拒绝，CPU=0、stdout/log 为空，再次核验也拒绝。
测试不修改真实共享运行时，不使用代码注入或外部写入标记。

## 生产宿主与生命周期

CapabilityHost 保留原生默认工厂，也支持显式无原生回退的 Core profile；
原生执行、参数规范化和旧聊天执行实现移到独立 translation unit，公共冻结、
恢复、提交与聊天 codec 共用实际生产代码。未使用的 Q_OBJECT 已移除，保留 QObject
生命周期/QPointer；仓内无 signals/slots/properties、反射构造或相关 cast 依赖。
证据组装的 QGIS 版本改由真实 QgisRuntime 初始化注入，未装配 GIS 时不伪造软件版本。
参见[收敛记录](agent-python-evidence-runtime.md)：qgs-audit 删除 1 行（135→134），
qgis-spread 删除 1 行（170→169），其余基线无增长。

生产 app 预热发生同步重入时，后一次绑定优先；原对象仍存活也不能被旧绑定覆盖。
来源解析在 Host context 回调后重新检查 owner，避免已销毁 Catalog 继续被使用。
相关回归与既有 Host/Recovery、Dispatcher、任务、输出集、保留/清理和控制台一起验收。

## 已取得的原生组件证据

Windows MSVC/Qt 6.8 同树真实静态库和 SDK 9d80，没有替代 execute、registry 或 submit：

- runtime **20/20**，含新增仅身份检查及晚替换拒绝；
- 强后端就绪 **8/8**，显式 require execution、expected sandbox=ok；
- 生成操作 **9/9**，公开 schema/decoder→Host→真实 SDK→LPAC NumPy→128 B 报告页→
  重建 Host 后只发布原报告，保持父版本、计算次数和幂等；
- 语法错误、伪造报告、准备后 code/data 变化、认证首段后的报告变化均拒绝，
  无正式成果或额外执行。allowGenerated=true、allowTrusted=false；
- 上述新安全修复后的三组共 93.862 秒，零失败/跳过，关键对象依赖 VALID，
  源码/头/对象 SHA 和时间在 `local/python-validation/host-native-core/`；
- 无 GIS 证据单元另有 3/3。上述是原生组件及实际 Core Host 链，不是 Windows GUI
  完整构建；Linux 完整应用另行验收。

平台契约、限制和打包说明见 [SANDBOX_SUPPORT](../../vendor/agent-python/SANDBOX_SUPPORT.md)。
Windows 允许的任意原生线程数量没有独立 Job 上限，诊断明确为 false；全部线程共享
单核并行度、CPU、提交内存和墙钟硬预算。Linux 还拒绝 clone。两后端子进程均零磁盘写，
不支持网络、子进程、安装包、持久解释器或通用宿主文件访问。

## 完整应用与依赖资源

默认/no-ORT 各完整编译 `paleo` 与 49 C++ 目标，原生最终测试选择为各 53 项
（另 4 项供货检查）。WSL 缺 CPU controller 的负例与原生成功验收分列；本批最终
安全修复后的完整编译及最终原生结果待补录，不使用旧源码结果充当新结果。

新加入完整矩阵的既有 `tst_runtime` 发现私有 QGIS prefix 漏解包 postinst 资源步骤。
已用仓库现有 `ensure_qgis_resources.py`，从锁版 qgis-providers-common 4.2.3+44 的
已核验模板补齐，未造数据库或削弱断言。SRS 4,349,952 B，SHA
`184bb6c53e463becd28b7c3269655ff9098ab7eebecf927ee49c1a3dcd482908`，SQLite 完整性正常，
13,720 CRS、EPSG 4326/3857 齐全；原 ELF 两配置各 13/13 复验通过。
VM2 用独立 `qgis-srs-supplement.json` 锚 `b92286865f607a85c055e83644c27657b167bd178cfaffa7ee2a05225cada038`，
不声称原共享闭包清单已包含新增文件。各应用须再次通过六组 ABI、Office 组件、
首帧及运行时 2143 文件前后 SHA 检查后，才能按计划提交本批 PR。
