# Python、Pi 与类型化输入输出合并后 review

基于合并后的 master `9bc792a9bc857a6723767593e0bb37f69f746c2c`，SDK gitlink
维持 `1362a69e61a663aba2b5024bfa83627af0759354`。本轮检查供货、配置、启用、
诊断、模型流、宿主绑定、取数、成果验收、发布及恢复，沿用现有操作和成果链路。
Shell、任意路径读取、动态包安装与网络权限继续后置。

## 修复

| 问题 | 原行为 | 处理与回归 |
|---|---|---|
| 已取消刷新污染共享缓存 | 旧刷新在新刷新复用已验证 snapshot 后才进入校验，清掉新请求依赖的缓存 | 获取 inspection mutex 后先检查取消；尚未开始校验则保留旧 snapshot。实际校验开始后的取消仍使缓存失效；覆盖两种 inspection、确定的并发交错与后续同尺寸篡改 |
| CSV 展开后超出策略预算 | 二进制数值帧满足预算，但浮点转十进制后的 report＋CSV 可能超限 | 在编码后复核最终总字节数；覆盖 raw frame 已通过、展开后越界及恰好等于预算的边界，拒绝时不返回部分成果 |
| Pi 小分片误触发输出上限 | 每个 delta 均计入完整 JSON 包装，实际合并后只需少数帧 | 统一按 JSON 转义后的文本 payload 计量，继续保留原字符限额和 16 KiB 帧余量；真实 Pi 覆盖 10000 个单字符、同一 owner callback 突发、转义字符与真实溢出 |
| Windows Pi 宽句柄继承 | Qt 默认允许子进程继承宿主的其他可继承句柄，独占文件可能被意外延长持有 | 使用私有持久 STARTUPINFOEX 与 HANDLE_LIST，只准入三条标准管道；失败保持禁止宽继承并拒绝继续协议。补充真实固定 Node 存活期间的独占文件删除及无效标准句柄启动失败回归 |

## 结构整理与保留项

- 将 A1 握手与 A2 会话重复的最小环境、Windows Job Object、Linux parent-death、
  no-new-privileges 和关闭继承 fd 收进私有 `piprocessplatform_p.h`。各调用方继续
  负责独立运行时认证、协议、启动顺序、超时、取消和回收；旧 Qt/Linux 继续拒绝运行。
- 将数值表和地震道输入重复的 owner/context、正式 Catalog 父子关系、源 revision
  与模型目录核对收进 app 私有 `pythoninputbinding_internal`。格式解析、错误码、
  地震文件完整 SHA 要求、大小约束及 reader 世代绑定留在各 adapter。
- 多成果 Native 套件的 7 个坏端口行和 1 个成果篡改行共用 suite 生命周期的
  真实就绪环境，减少重复激活和 NumPy admission probe；项目、SDK、Host、源 reader、
  操作、成果和计数仍逐行独立。两个历史恢复行继续独立冷启动及 59→60 策略切换。
  完整数据行、300 秒整进程时限、每次计算和启动的完整性校验均保留。
- 保留 v1 报告适配与历史恢复、人工 Console、运行时和模板独立 store。它们仍有
  调用方或不同的版本/授权契约，不能仅因名字相近而删除。
- 保留 capture 与 launch 两个信任边界的完整性校验、固定工具集、宿主权限判定、
  分页引用校验、单次发布和 `commit_uncertain` 恢复。Pi 仍为可信编排进程，
  生成 Python 仍走原强后端；本轮不扩大工具能力或提高预算。

早期多成果 Native 套件达到原 300 秒整进程时限；本轮减少重复准入准备成本，
保留全部业务行、冷启动历史恢复行及每次计算/启动的完整校验。

Pi 端到端初次验收出现过 `pi_control_timeout`。临时有界时序观测的完整链路通过，
未复现原失败，无法据此判断旧失败阶段；因此保留原 15 秒控制规则及认证顺序。
观测代码仅保存在 ignored 诊断副本，正式源码不携带该临时跟踪。句柄修复独立于
超时归因，不把诊断副本的通过计为正式版本验收。

不增加 registry、执行队列、通用 adapter 框架或 UI。所有基线保持原值。

## 验证

本轮功能源码冻结为 `fcf54e7d2e908d8c2e378a2cba759ecceceb28a1`，SDK gitlink
保持 `1362a69e61a663aba2b5024bfa83627af0759354`。以下仅计正式冻结源码的新运行；
临时诊断及旧批次结果不计为通过。

| 平台与配置 | 实际构建 | 实际验收 | 结果 |
|---|---|---|---|
| Windows ORT | 82 个目标 | 50 个全新测试进程，Qt 699 PASS | 0 FAIL；2 个可选截图 skip |
| Windows 无 ORT | 81 个目标 | 49 个全新测试进程，Qt 679 PASS | 0 FAIL；2 个可选截图 skip |
| Linux ORT | 43 个目标，编译 3343.388 秒 | 39 个 C++ 套件＋8 个供货检查，Qt 523 PASS | 47/47；0 FAIL、0 skip |
| Linux 无 ORT | 43 个目标，编译 1350.365 秒 | 39 个 C++ 套件＋8 个供货检查，Qt 523 PASS | 47/47；0 FAIL、0 skip |

Linux 两配置各有 13 项新增回归和四项真实 Python 接线业务行通过；实际 Pi 的
A1 七项与 bridge 七项业务行均齐全，无 skip。各配置完成六个 `ldd -r` ABI 检查、
一次真实 GUI 的六项必需 marker，以及 Python 2143 项、Pi 全集在执行前、
执行后和 GUI 后的完整 SHA 与只读状态核对。Python/Pi 的 REQUIRE 均为 1，
所有测试保持原时限。两配置编译和 Native 原始证据均经过独立复核。

Windows 每配置另外执行真实 Pi 的 bridge 7 项和 A1 9 项业务回归，共 32/32 PASS，
包含两项 Windows 句柄回归，无 skip。两个可选截图 skip 分别来自未设置
`PALEO_AI_RESULT_SHOT_DIR` 和 `PALEO_AI_SHOT_DIR`，不影响真实 Native 验收。
两配置的多成果 Python Native 整体耗时分别为 268.532 秒和 184.229 秒；
Pi 联合成果 Native 为 75.999 秒和 130.008 秒，均保持原 300 秒整进程时限。

Windows 每配置一次真实 GUI 启动均出现全部六项必需 marker，Office 套件均为
Qt 10/0/0/0，并核对五项 editor asset SHA。源码 6620 项、两配置物理 EXE、
依赖 prefix 38823 项、Python runtime 983 项、模板 5 项、Pi 10 项及六项源码/
已暂存模板文件完成运行前后完整 SHA 核对。无 ORT 的宏、链接与运行依赖已单独检查。

Linux 编译范围明确为应用、三个工具与 39 个受影响测试共 43 个目标，传递覆盖
各配置全部生产静态库和 SDK（ORT 30 个、无 ORT 29 个）；显式包含默认构建
不会选择的 snapshot worker。
完整 39 个 C++ 套件、8 个供货检查、原生 Python/Pi、GUI、ABI 和完整性要求保持。
最初默认全仓构建的受控中止回执保留，不计为默认目标全部编译成功。

功能冻结版本的完整 Source gates 为 41/41，所有基线保持原值。最终提 PR 前
再次 fetch/rebase `origin/master`，对文档提交后的 HEAD 重跑同一批 41 项门禁，
检查功能源码投影与冻结提交一致；执行收据绑定最终 HEAD、SDK 与 master，结果
写入 PR 描述。

原生验收前，VM2 固件依次尝试数据盘和网络启动项，首次 240 秒 SSH 等待失败。
同一进程随后自然进入 Linux，固定 host key 的 SSH probe 成功后，原代码、原时限
的严格环境核验通过；未重启或修改磁盘、固件。首次失败、状态变化和后续核验
的原始日志分别保留，正式测试在核验通过后开始。

两配置验收后 VM2 已正常 sync/poweroff，全部任务与所属进程退出；停机后重新
计算 root 和 firmware 完整 SHA，生成 v5 检查点。原数据盘保持只读，VM1 未变。
旧启动前 root SHA 不再作为下一次恢复锚。
