# 受控 Python 合并后 review 与整理

## 合并、基底与范围

2026-11-15 按依赖 squash 合入 SDK #21 与主仓 #945、#947、#948、#949、#950、#951。
主仓 master 为 `c756cdbe401e77d13f0eb7192034ab1067a78e51`，其完整 Git 树与已验收的
PY4 `fd8f07c2` 相同。各批重新接到最新 master 后分别核对树不变并复跑 Source gates
（PY1 35 项，后续各 37 项），不把旧基底的绿直接用于新基底。
SDK 合并提交 `9160615d2e3a633bee9dd3ed8816f6a38d58f8ae` 与原 `9d80d1f6` 树相同；
整理分支将 gitlink 指向正式合并身份，避免长期依赖已关闭 PR 的工作分支提交。

每次临时移除 required checks 后立即恢复原 strict/admin/lint/linux/windows 配置并核对。
已合并的七个工作分支本地、远程均清理，既有 worktree 改为 detached，验证文件保留。
主 checkout 原未跟踪文件及 6 份 stash 保留。新整理 PR 单独提审，不自行合并。

本轮沿现有分层和操作/成果链路整理，不新增执行环境、registry、队列或 UI。
Shell、网络、包安装、GPU、持久解释器和任意宿主文件访问继续后置。

## Review 结论与处理

| 维度 | 结论 | 本轮处理 |
|---|---|---|
| 正确性 | 发现需修复的生命周期和就绪缺口 | Host/恢复回调销毁检查、完成连接回收、同进程 generation 租约交接 |
| 安全与完整性 | 保留既有强边界 | 生成路径仍仅做身份核验后进入强后端；每次全 SHA、跨进程磁盘锁和权限模型保留 |
| 性能 | 有可明确消除的重复工作 | 同次启动 helper 只完整认证一次；内容 SHA 宏只落到真正消费的源文件 |
| 可维护性 | 功能边界基本清晰，装配和构建归属需收口 | Python 装配归现有模块；通用 Host TU 归通用构建挂点；移除死探针/生产示范装配及游离测试 |

保留运行时供货与模板供货各自的 store，也保留人工 Console/REPL：它们的授权与持久化
契约不同，不能仅因都涉及 Python 就合并。保留生成报告“数值未经宿主独立重算”声明，
不把结构验收扩大为指标或地质结论证明。

### 生命周期与租约

`binding`、adapter 和恢复 hook 可能同步切换工程或销毁 owner。调用前复制回调，返回后
检查 QPointer 与当前绑定失效标记，拒绝已销毁或已失效的请求；app 在尾部 context
回调后也复核绑定世代，避免旧装配覆盖新工程。首次/第二次 binding、capture、预热
三阶段重入及恢复 hook 均以不运行用户代码的生命周期回归覆盖。

恢复闭包还绑定本次 Host preparation generation，覆盖 owner 存活但已 invalidate 或
同 context 重绑的情况。Recovery 在 `live()` 正常初始化后保留执行 token，各恢复 hook
后复核，并把同一个 token 交给 attempt；不能在 hook 后领取新 token 继续旧请求。
新增 12 组 adapter/member、三类 hook、失效/重绑组合，要求同步拒绝且计算计数为零；
另有新 Host 首次直接 start 成功正例，防止误伤正常初始化。

Preparation 的 service/parent/owner/receiver 生命周期连接改用弱捕获并集中保存，完成
交付时断开；先落 delivered 防重入，worker 仍持 state，未结束的准备不提前清 frozen。
完成任务不再向长寿命 service 线性累积销毁连接。

RuntimeProvider 仅在同进程、同 canonicalRoot＋manifest 间共享 generation 保留租约，
不是共享验证结果。每次重新检查全部文件；raw verifyRuntime 的独占锁契约不变。
池锁只覆盖弱引用与磁盘锁获取，不覆盖哈希和取消回调。清空缓存前先持有强租约，避免
重绑策略时旧 provider 持锁导致新 provider 永久缓存 busy。两个显式 inspect 入口保留，
只有 ImportProbe 路径能运行原普通解释器探测，GeneratedOnly 不走它。

### 结构与删除项

- Python config/provider 复用、readiness、adapter 与预热收进既有
  `app/pythonanalysiswiring`，`aiwiring` 保留工程绑定和通用装配。
- `trusted_summary` 是 Internal 验收能力，删除不可调用的生产装配；内部操作/恢复测试
  和锁版示范模板保留。
- 删除无生产调用的 `linuxSandboxReadiness` 旧探针，真正就绪仍由强后端 NumPy 固定
  计算证明。unsupported 测试直接验证真实执行接口拒绝。
- Prepared 携带本次已认证的 helper profile，消除相邻重复认证；不引入跨请求缓存。
- 通用 Host、源操作发布和聊天 native TU 及 evidence 测试从 generated extra 移到
  `extra-capability-host.cmake`，target 和注册集合不变。
- 四个 SHA 宏只附到三个真实消费源；configure 依赖保留。默认/no-ORT 实际
  `compile_commands.json` 各 2200/2160 项，已核对无无关 TU 获得这些定义。
- 游离的 `test_python_analysis_templates.py` 八组参考案例迁入现有
  `tst_pythonanalysis` 数据行后删除，复用固定模板 SHA、受控进程与原 CTest/fixture。
  覆盖标量参考、完整行、空/缺失、1e308、单位、坏帧与 8192×16 窗口。

## 验证记录

默认/no-ORT 在各自独立物理 build 配置完成；Office/WebEngine 依赖接线保持。
功能源码冻结于 `a6b92d6b8803e523827a2f19f1ae8aae286a310d`，SDK 为上列 `9160615d`；
之后仅补文档。此前 `7d1f3250` 的完整编译与 focused 回归保留为补代次修复前记录，
不能代替最终冻结版本验收。
完整编译、功能回归、Windows 原生组件和非 WSL Linux 原生矩阵结果在完成后补录。
既有 PY4 证据包保持不变，新批次不复用其成功计数。完整源码门禁在最终 fetch/rebase
之后重跑，基线只缩不涨；不将配置成功当作编译或执行成功。

本机可复现入口为整理工作树 `local/python-validation/` 的 `mount-matrix.sh`、
`matrix.sh`、`source-gates.py` 和 `check-anchor-scope.py`；原生验收另存独立目录。
构建总并行不超过 8，Windows 测试串行，强隔离资源用例独占运行。
