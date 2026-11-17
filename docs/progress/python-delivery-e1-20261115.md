# E1：Python 内部供货与原生离线安装

日期：2026-11-17。当前代码 HEAD `2723228ae63466469d808dd3af7c552ab31df4e7`，SDK `9160615d2e3a633bee9dd3ed8816f6a38d58f8ae`。源码版本、实跑版本及精确范围分列。

## 范围与用户链路

内部供货端复用已准备的 runtime/scripts generation，以顺序流封装交付，原 manifest 身份不变。客户端由 C++/Qt Core 的 `paleo_python_environment` 和同一安装服务承担，不依赖系统 Python、pip、解包程序、网络或解释器执行；可选组件不进入默认安装，也不注册成 Agent 工具。

用户选择供货包与专用 store 后，客户端核外层包 SHA、逐文件 SHA、完整路径与目录集合，拒绝越界、别名、重复、截断、超限和额外条目。安装与原 preparer 共用 OS 锁；完整核验后同文件系统发布不可变 generation，再原子更新安装选择，不编辑或删除旧 generation。

取消和提交前失败保留旧选择；提交后的目录同步不确定单列 `commit_uncertain`，不伪称回滚。普通 Windows 本地长路径经原文与规范化双重检查，device/UNC/ADS、junction、硬链接及混合 store 保持拒绝。generation 重命名与超过 260 字符的本地路径租约修正保留物理锁存在、并发拒绝、释放与再次获取的真实回归，不以放宽 stale 或超时删除掩盖残留锁。

E1 交付安装，不顺带启用计算；持久配置、独立可信／生成授权由 E2 承接，设置入口由 E3 承接。内部包与模板仍为既定许可状态，存在 LICENSE 文本不被解释为分发授权。

## 代码身份与分平台接续

E1 仍为 2723228a，没有本次 alias 或 Windows-only 测试清理。原 Linux 两路完整构建／定向回归保持 C++ overlay、历史快照与未同步非构建文档的原边界，不称完整 Git 字节树。

原完整构建／ordinary（保持当时 identity）：

| 原平台与配置 | 原已证结果 | 原边界 |
|---|---|---|
| Linux 默认 | 当前 `cppSourceHead/buildInputSourceHead=2723228a…`；完整 54 目标、50 个 C++ 测试目标构建退出 0；定向 Runtime **27/0/0**、Delivery **23/0/0**；Office 入口、WebEngine、首帧和审计通过 | 本轮运行的是定向回归，不把 50 个已编译测试当作 50 套全部执行 |
| Linux no-ORT | 独立 build，同范围完整构建和定向结果通过 | 同上；未新增当前 Native 执行结论 |

## Windows 实际验收与分域范围

- Windows ort：完整 53 目标和最终身份核验完成；raw `28180e7691ffb335daffbca32ba5585bc592c2b7e4e9865cfd81e4f72e1c6633`。原 ordinary exit 8、Qt 719/18/6/0 保留。定向环境／强执行／GUI 均按原 required-stage 收据 exit 0；平台 skip 与原红不删。
- Windows noort：完整 53 目标和最终身份核验完成；raw `28180e7691ffb335daffbca32ba5585bc592c2b7e4e9865cfd81e4f72e1c6633`。原 ordinary exit 8、Qt 714/17/6/0 保留。定向环境／强执行／GUI 均按原 required-stage 收据 exit 0；平台 skip 与原红不删。

Windows Qt 四项依次为 passed／failed／skipped／blacklisted；其他原三项按原口径。次数含各次 init/cleanup；不把 direct QtTest 记成 CTest 全绿，也不合并原失败与复跑以伪造首轮 0。

## 已执行 Native：保留真实版本与范围


统一索引中的五路完整 Native 共 140 项、Qt 1539/0/0，另列上述默认显式 composite；所有版本、scope 与首次失败均保留。Windows-only 条件块不改变 Linux 业务有效代码，因此没有再次启动机器验证或产品运行。

## 边界与证据

外部分发入口仍关闭；Shell、任意 CLI／SQL／HTTP、在线安装包与持久解释器仍后置。prepare／compute 只产生私有成果，publish 独立；生成数值与像素未经宿主独立重算。Pi 是可信可选编排组件，不是 OS 隔离沙箱；缺强后端不 fallback。Office／WebEngine 与 offscreen 首帧不扩写为 Office WASM DOM 验收。

证据索引位于操作者本地 E3 worktree 的 ignored 路径 `local/final-execution-20261116/final-docs-20261117-r2/reconciled-progress/evidence-index.json`，未入仓库，也不是本进度文档所在目录的仓库文件；对外审阅需另附实际可访问的证据附件或链接。该本地索引分列原完整构建／ordinary、alias 增量、各 Native 原身份与 composite、新 Windows-only proof、有限 O2 Linux crosscheck、动态 revision4 completed/final、原失败及四份 reviewed 全局文档 manifest。原 writer、原输入及生产文档／源码／refs 不修改。

本批分支 `codex/python-delivery-e1-20261115`，直接父分支 `master`；共同 master 基底为 `e995a477`。复用 SDK `9160615d`，没有新增跨仓接口依赖。 实际审阅 HEAD、直接父 PR 及 fetch/rebase 后的 Source gates 结果以本批 PR 正文为准；运行证据继续保留本文列出的实际代码身份，文档提交不重标原实跑版本。 本批已有 [PR #954](https://github.com/WindWang2/paleo_workstation/pull/954)。原 Windows StorageGovernance 两项 NTFS 大小写夹具断言仍失败，相关生产与测试 Git blob 已对照 master 完全相同；定向环境修正和真实受控执行的通过分别记录，不把原 48 套合成为全绿。


## Windows 队列终态收口范围

E2 至 O2 的十六个 profile 已有真实 build／final 与独立 audit 收据；E2 默认仍按旧全量加新单 TU 的组合等原范围记录，不把原失败或旧实跑版本改标。队列终态以新核对命令实际 exit 0 的 `terminal_reconciliation` 纯元数据收口为准；原工具 session 87628 的 outer exit 因 session 失效仍为未知，不能写成原 outer 0。18823／81832 的原 exit 1 永久保留；本次收口未重新运行 C++、测试或依赖大树 SRI。owner 原件、精确 scope、各旧退出事实与 SHA 均链入上述操作者本地 ignored 证据索引。
