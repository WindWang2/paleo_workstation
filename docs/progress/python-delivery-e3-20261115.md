# E3：设置入口与后台环境诊断

日期：2026-11-17。当前代码 HEAD `33ca7ac52caedbfa9072815d49b725cf721c1385`，SDK `9160615d2e3a633bee9dd3ed8816f6a38d58f8ae`。源码版本、实跑版本及精确范围分列。

## 范围与用户链路

现有模型配置入口打开 Python 环境设置。**UI 只编辑草稿、显示快照并发出意图**；安装、完整性核验、CAS 激活、执行探针和诊断写入由功能控制器经同一 TaskService 在后台编排。工厂与聊天 runner 共享稳定 registry，不另建环境权威或第二条任务队列。

安装候选不会顺带启用计算；可信和生成权限独立选择。运行时、模板、来源和两类执行就绪状态分别显示。generated-only 没有模板时明确显示“无需安装（生成专用）”，不把缺失模板写成完整性校验通过；未实测的后端不显示可执行，也不回退系统解释器。

后台结果绑定任务、表单修订和环境代次，刷新不覆盖用户草稿或偷换 CAS。关闭窗口取消未受理工作；已受理的停用／撤销以 Application scope 完成保存，窗口删除或工程切换不会丢掉持久撤权。任务服务销毁、信号重入和迟到回调都检查 owner 与代次，写盘失败如实保留本会话拒绝，不让旧激活恢复新近撤权。

诊断导出只保留白名单状态、实际后端资源能力和稳定原因码，去掉密钥、请求、生成代码、数值输入、原始错误载荷与完整用户路径。申请预算和实际可执行能力分列，不以配置值冒充强隔离后端已接受的限额。

当前翻译修订在英文、中文两份 TS 中**各完成本批新增的 100 条界面文案**，覆盖模型入口、后台任务、控制器和环境窗口；没有改检查器或降低门槛，也不声称历史英文骨架已全量翻译。翻译资源随当前构建重新生成／链接。原浅色、深色、小窗、滚动及忙状态截图保留历史身份，不把它们当作当前 Windows 或强执行验收。

## 代码身份与分平台接续

本次 restack 仅增加 `Q_OS_WIN` 条件内的六行测试清理，产品代码、全部原断言及 Linux 适用业务代码未变；删除该精确插入块可还原前一 Git blob。当前代码 HEAD 不等于下列每份旧实跑收据的 HEAD；不宣称 Linux 全量重跑、Native 重跑或 ELF 调试行号元数据相同。

- 原完整 Linux 构建／ordinary：`08660d05d235a11eebaf80c1db4e40335cfe4e7d`，保持原 b047／6096 入口及下表范围。
- 后续 alias 单目标增量及已执行 Native 的依据 HEAD：`f4d1f3fdb5be7c6b858ed3dabfc560d76d31512d`；其 source/raw/SDK 和失败历史不改标为当前 Windows-only HEAD。
- Linux ort alias 增量：`tst_pythonanalysiswiring` actual 0，raw `fa6b65012a82eef55d3d9f82a0704b1c33eb2a8b57bef8e3a4e4d4df580e592e`；ordinary／GUI 未重跑，其余 ELF 按原收据的 stat unchanged 保留。
- Linux noort alias 增量：`tst_pythonanalysiswiring` actual 0，raw `fa6b65012a82eef55d3d9f82a0704b1c33eb2a8b57bef8e3a4e4d4df580e592e`；ordinary／GUI 未重跑，其余 ELF 按原收据的 stat unchanged 保留。

本次测试 TU 的 Git LF SHA `2a4d02416cee602a553e0fce29b036d4ddd8cebdb670ab4ba5b759be7490d35c` 与 Windows live CRLF SHA `d3c19285c6427b9c65713d71dd3e55b6d50aa0f707a1074aa0d25d3cfe742ead` 分列；whole-tree raw 只取实际构建／运行收据，计划派生值不当作实跑身份。

原完整构建／ordinary（保持当时 identity）：

| 原平台与配置 | 原已证结果 | 原边界 |
|---|---|---|
| Linux／WSL 默认 | 完整 61 个目标退出 0；13 个普通套件，Qt **186/0/0**；六项供应源码检查退出 0；Office、WebEngine 闭包和首帧通过 | 不外推为强隔离真实执行通过 |
| Linux／WSL no-ORT | 完整 60 个目标退出 0；12 个普通套件，Qt **172/0/0**；同范围供应、组件及首帧检查通过 | 同上 |

## Windows 实际验收与分域范围

本 profile 的冻结范围为五个 strong groups；wiring 只执行 managed 两条数据行，legacy 两条没有在本 profile 实跑，只引用 E2 相同测试字节与 SDK 的实际收据。五组及两行的实际状态只取对应 completed/final，不由计划推定。

- Windows ort：当前 HEAD 完整 60 目标构建及最终运行身份核验完成。
  当前 final raw `819a9f0f196a356c8c692177d8bbc47b0e59e9ca98a3cff9c30fea3b5287ae6e`；ordinary 14 次 Qt 180/0/1/0，实际 exits [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]，allGreen=True；强执行 5 次 Qt 35/0/0/0，首帧 exit 0。各阶段原 HEAD 保存在索引，不统一改标。
- Windows noort：当前 HEAD 完整 59 目标构建及最终运行身份核验完成。
  当前 final raw `819a9f0f196a356c8c692177d8bbc47b0e59e9ca98a3cff9c30fea3b5287ae6e`；ordinary 13 次 Qt 160/0/1/0，实际 exits [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]，allGreen=True；强执行 5 次 Qt 35/0/0/0，首帧 exit 0。各阶段原 HEAD 保存在索引，不统一改标。

本轮 E3 至 O2 Windows Runtime 分域只把 `changedFixtureIsReauthenticatedBeforeStrongExecution` 留在既有 strong Native 域；ordinary 保持其余 17 个方法原顺序，包括 `invalidManifests` 的八条数据行。两域为原 18 方法的精确不交并集，原 strong 同函、断言、fixture、REQUIRE、预算均不减。分区本身不计运行结果，实跑状态仅取本轮对应收据。

Windows Qt 四项依次为 passed／failed／skipped／blacklisted；其他原三项按原口径。次数含各次 init/cleanup；不把 direct QtTest 记成 CTest 全绿，也不合并原失败与复跑以伪造首轮 0。

## 已执行 Native：保留真实版本与范围

- Native ort：实际 `f4d1f3fdb5be7c6b858ed3dabfc560d76d31512d`，raw `fa6b65012a82eef55d3d9f82a0704b1c33eb2a8b57bef8e3a4e4d4df580e592e`，14 项、Qt 124/0/0；run／collect／CTest、六 ABI、首帧和前后完整性按该原身份通过，不重标当前代码 HEAD。
- Native noort：实际 `f4d1f3fdb5be7c6b858ed3dabfc560d76d31512d`，raw `fa6b65012a82eef55d3d9f82a0704b1c33eb2a8b57bef8e3a4e4d4df580e592e`，14 项、Qt 124/0/0；run／collect／CTest、六 ABI、首帧和前后完整性按该原身份通过，不重标当前代码 HEAD。

统一索引中的五路完整 Native 共 140 项、Qt 1539/0/0，另列上述默认显式 composite；所有版本、scope 与首次失败均保留。Windows-only 条件块不改变 Linux 业务有效代码，因此没有再次启动机器验证或产品运行。

## 边界与证据

外部分发入口仍关闭；Shell、任意 CLI／SQL／HTTP、在线安装包与持久解释器仍后置。prepare／compute 只产生私有成果，publish 独立；生成数值与像素未经宿主独立重算。Pi 是可信可选编排组件，不是 OS 隔离沙箱；缺强后端不 fallback。Office／WebEngine 与 offscreen 首帧不扩写为 Office WASM DOM 验收。

证据索引位于操作者本地 E3 worktree 的 ignored 路径 `local/final-execution-20261116/final-docs-20261117-r2/reconciled-progress/evidence-index.json`，未入仓库，也不是本进度文档所在目录的仓库文件；对外审阅需另附实际可访问的证据附件或链接。该本地索引分列原完整构建／ordinary、alias 增量、各 Native 原身份与 composite、新 Windows-only proof、有限 O2 Linux crosscheck、动态 revision4 completed/final、原失败及四份 reviewed 全局文档 manifest。原 writer、原输入及生产文档／源码／refs 不修改。

本批分支 `codex/python-delivery-e3-20261115`，直接父分支 `codex/python-delivery-e2-20261115`；共同 master 基底为 `e995a477`。复用 SDK `9160615d`，没有新增跨仓接口依赖。 实际审阅 HEAD、直接父 PR 及 fetch/rebase 后的 Source gates 结果以本批 PR 正文为准；运行证据继续保留本文列出的实际代码身份，文档提交不重标原实跑版本。


## Windows 队列终态收口范围

E2 至 O2 的十六个 profile 已有真实 build／final 与独立 audit 收据；E2 默认仍按旧全量加新单 TU 的组合等原范围记录，不把原失败或旧实跑版本改标。队列终态以新核对命令实际 exit 0 的 `terminal_reconciliation` 纯元数据收口为准；原工具 session 87628 的 outer exit 因 session 失效仍为未知，不能写成原 outer 0。18823／81832 的原 exit 1 永久保留；本次收口未重新运行 C++、测试或依赖大树 SRI。owner 原件、精确 scope、各旧退出事实与 SHA 均链入上述操作者本地 ignored 证据索引。
