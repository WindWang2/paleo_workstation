# E2：宿主配置、独立授权与历史环境

日期：2026-11-17。当前代码 HEAD `989b48c5418aa34c31f7ab08efc8d76e9d636d65`，SDK `9160615d2e3a633bee9dd3ed8816f6a38d58f8ae`。源码版本、实跑版本及精确范围分列。

## 范围与用户链路

E1 安装的 generation 通过原生维护 CLI 或同一宿主服务执行 activate/status/disable/revoke/rollback；客户端不要求系统 Python，激活也不启动解释器。配置以观察修订 CAS、OS 互斥和原子替换持久化。运行时和模板身份、可信与生成授权分别核对，生成专用身份不要求模板，但仍要求真实强后端就绪。

runner、Host 和后台预热共享稳定的 `PythonEnvironmentRegistry`。先完整选择宿主记录；只有宿主记录缺失才整组回退兼容变量，不能拼接不同来源的 root/SHA，也不能用兼容变量扩大授权。普通切换保留已认可的在途身份；紧急撤销先在内存拒绝，再通过现有 TaskService 保存，迟到激活不能覆盖较新的拒绝。

用户经命令或 Agent 进入 prepare → execute → 分页读取 → publish 时，仍走原有 plan、InputBundle、worker、operation 和 outputset。历史操作只恢复宿主认可身份，拒绝操作文件自报路径；发布已计算成果不重跑代码，未提交发布仍复核当前撤权，已提交 SQL 事实按原恢复路径处理。

当前修订补齐三条 Python 命令元数据。Dispatcher 的三处行为修复为：票据创建前设置默认 `pinned-value` 绑定；存在 Host 时由 Host 归一化、仅无 Host 时做标量预检；将 Host 的有效参数回传 prepare 结果。声明参数名也与定义对齐。新增 managed Dispatcher 回归使用宿主持有的真实上下文，覆盖四条真实数据行，其中两条为 managed 计算，并检查带合法 code 的额外生成参数被拒时没有 host handle、任务或正式版本。其编译及实际强执行以本稿验收表为准，不能由普通六函数子集代替。

## 代码身份与分平台接续

本次 restack 仅增加 `Q_OS_WIN` 条件内的六行测试清理，产品代码、全部原断言及 Linux 适用业务代码未变；删除该精确插入块可还原前一 Git blob。当前代码 HEAD 不等于下列每份旧实跑收据的 HEAD；不宣称 Linux 全量重跑、Native 重跑或 ELF 调试行号元数据相同。

- 原完整 Linux 构建／ordinary：`b3fe075e50b83393807370275575904be60609d2`，保持原 b047／6096 入口及下表范围。
- 后续 alias 单目标增量及已执行 Native 的依据 HEAD：`3f421ccc501ec44a34d6a464c83c57f7ff1c5fe1`；其 source/raw/SDK 和失败历史不改标为当前 Windows-only HEAD。
- Linux ort alias 增量：`tst_pythonanalysiswiring` actual 0，raw `7e7c8368da31c61f271e8bed720cf499724a6bb409eee5c4e8af3f0ca3277448`；ordinary／GUI 未重跑，其余 ELF 按原收据的 stat unchanged 保留。
- Linux noort alias 增量：`tst_pythonanalysiswiring` actual 0，raw `7e7c8368da31c61f271e8bed720cf499724a6bb409eee5c4e8af3f0ca3277448`；ordinary／GUI 未重跑，其余 ELF 按原收据的 stat unchanged 保留。

本次测试 TU 的 Git LF SHA `2a4d02416cee602a553e0fce29b036d4ddd8cebdb670ab4ba5b759be7490d35c` 与 Windows live CRLF SHA `d3c19285c6427b9c65713d71dd3e55b6d50aa0f707a1074aa0d25d3cfe742ead` 分列；whole-tree raw 只取实际构建／运行收据，计划派生值不当作实跑身份。

原完整构建／ordinary（保持当时 identity）：

| 原平台与配置 | 原已证结果 | 原边界 |
|---|---|---|
| Linux／WSL 默认 | 完整 58 个目标退出 0；10 个普通套件，Qt **149/0/0**；六项供应源码检查退出 0；Office、WebEngine 闭包和首帧通过 | 强隔离真实执行不属于此 ordinary 结果 |
| Linux／WSL no-ORT | 完整 57 个目标退出 0；9 个普通套件，Qt **135/0/0**；同范围供应、组件及首帧检查通过 | 同上 |

## Windows 实际验收与分域范围

- Windows ort：**组合验收完成**。旧 `3f421ccc` 完整 57 目标构建，加新 Windows-only 单 TU 增量 exit 0、复用未改测试的原运行结果、新四条真实 wiring 行／GUI／final exit 0；不称新 HEAD 完整重跑。
  当前 final raw `4632e649374db4bc8b3e6e3860e1894eaae8b5a076d653ad730cb5c22efe00eb`；ordinary 12 次 Qt 164/1/0/0，实际 exits [0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0]，allGreen=False；强执行 4 次 Qt 15/0/0/0，首帧 exit 0。各阶段原 HEAD 保存在索引，不统一改标。
  原 ordinary Runtime 26/1/0/0 的 private-hive 身份检查失败保留；同名强 Native 函数 3/0/0/0 通过，二者不互相覆盖。
  原 ORT System32 错误布局、只读 fixture 的 5/1 和原 coordinator／正确布局早轮运行退出 1 均保留；当前正确 pinned app-local 布局的实际完成 0 另列，不把旧失败改成通过。
- Windows noort：当前 HEAD 完整 56 目标构建及最终运行身份核验完成。
  当前 final raw `4632e649374db4bc8b3e6e3860e1894eaae8b5a076d653ad730cb5c22efe00eb`；ordinary 11 次 Qt 144/1/0/0，实际 exits [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]，allGreen=False；强执行 4 次 Qt 15/0/0/0，首帧 exit 0。各阶段原 HEAD 保存在索引，不统一改标。
  原 ordinary Runtime 26/1/0/0 的 private-hive 身份检查失败保留；同名强 Native 函数 3/0/0/0 通过，二者不互相覆盖。

E2 两份既有计划及其原分域不改；每路 ordinary private-hive 原红与对应强执行事实按实际收据保留。

Windows Qt 四项依次为 passed／failed／skipped／blacklisted；其他原三项按原口径。次数含各次 init/cleanup；不把 direct QtTest 记成 CTest 全绿，也不合并原失败与复跑以伪造首轮 0。

## 已执行 Native：保留真实版本与范围

- Native ort：实际 `3f421ccc501ec44a34d6a464c83c57f7ff1c5fe1`，raw `7e7c8368da31c61f271e8bed720cf499724a6bb409eee5c4e8af3f0ca3277448`，13 项、Qt 102/0/0；run／collect／CTest、六 ABI、首帧和前后完整性按该原身份通过，不重标当前代码 HEAD。
- Native noort：实际 `3f421ccc501ec44a34d6a464c83c57f7ff1c5fe1`，raw `7e7c8368da31c61f271e8bed720cf499724a6bb409eee5c4e8af3f0ca3277448`，13 项、Qt 102/0/0；run／collect／CTest、六 ABI、首帧和前后完整性按该原身份通过，不重标当前代码 HEAD。

统一索引中的五路完整 Native 共 140 项、Qt 1539/0/0，另列上述默认显式 composite；所有版本、scope 与首次失败均保留。Windows-only 条件块不改变 Linux 业务有效代码，因此没有再次启动机器验证或产品运行。

## 边界与证据

外部分发入口仍关闭；Shell、任意 CLI／SQL／HTTP、在线安装包与持久解释器仍后置。prepare／compute 只产生私有成果，publish 独立；生成数值与像素未经宿主独立重算。Pi 是可信可选编排组件，不是 OS 隔离沙箱；缺强后端不 fallback。Office／WebEngine 与 offscreen 首帧不扩写为 Office WASM DOM 验收。

证据索引位于操作者本地 E3 worktree 的 ignored 路径 `local/final-execution-20261116/final-docs-20261117-r2/reconciled-progress/evidence-index.json`，未入仓库，也不是本进度文档所在目录的仓库文件；对外审阅需另附实际可访问的证据附件或链接。该本地索引分列原完整构建／ordinary、alias 增量、各 Native 原身份与 composite、新 Windows-only proof、有限 O2 Linux crosscheck、动态 revision4 completed/final、原失败及四份 reviewed 全局文档 manifest。原 writer、原输入及生产文档／源码／refs 不修改。

本批分支 `codex/python-delivery-e2-20261115`，直接父分支 `codex/python-delivery-e1-20261115`；共同 master 基底为 `e995a477`。复用 SDK `9160615d`，没有新增跨仓接口依赖。 实际审阅 HEAD、直接父 PR 及 fetch/rebase 后的 Source gates 结果以本批 PR 正文为准；运行证据继续保留本文列出的实际代码身份，文档提交不重标原实跑版本。


## Windows 队列终态收口范围

E2 至 O2 的十六个 profile 已有真实 build／final 与独立 audit 收据；E2 默认仍按旧全量加新单 TU 的组合等原范围记录，不把原失败或旧实跑版本改标。队列终态以新核对命令实际 exit 0 的 `terminal_reconciliation` 纯元数据收口为准；原工具 session 87628 的 outer exit 因 session 失效仍为未知，不能写成原 outer 0。18823／81832 的原 exit 1 永久保留；本次收口未重新运行 C++、测试或依赖大树 SRI。owner 原件、精确 scope、各旧退出事实与 SHA 均链入上述操作者本地 ignored 证据索引。
