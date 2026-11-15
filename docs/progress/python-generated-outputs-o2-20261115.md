# O2：三成果与 Pi/SDK 联合收口

日期：2026-11-15；对应 [计划](../planning/PYTHON_DELIVERY_AND_PI_AGENT_PLAN.md) O2。
实施候选 `ab9883ba`，基于已重排的 A2、I1、I2、O1；SDK 指向 PR #23 的 `1362a69e`。
本记录当前为实现与验收跟踪，**尚未编译或执行本批测试，不宣称整批完成**。

新增独立 `python.generated_outputs` 能力、明确的输入种类、冻结 profile 与三端口 frame v3。
数值表和地震道沿用各自真实选择/取数/验证契约；生成 worker 复用既有 TaskService 和环境
envelope。宿主独立验收 REPORT、类型化 TABLE 与 RGBA，编码 CSV/PNG，整组交给原有
outputset/Catalog/操作恢复链路。未增加工具安装环境、Shell、文件系统或网络权限。
接口与兼容边界见 [契约](../PYTHON_GENERATED_OUTPUTS.md)。

图片编码采用实际 Qt QImageWriter，精确尺寸/长度/完整 SHA 先于图像构造；受限 QBuffer
在写/seek 时限制 PNG 大小。额外宿主内存预算、输入/源码/运行时与编码器来源分别记录。
PNG 为非空间报告成果；原 v1 与 O1 记录继续由各自原 restorer 处理。

必要回归已登记但未执行：

- `tst_pythonimageoutput`：实际 PNG 编码与含透明像素的逐像素回读、类型化帧、边界、
  取消、内存/输出预算及不可压缩 PNG 的真实 4 MiB 上限。
- `tst_pythongeneratedoutputs`：两输入 schema、宿主字段拒绝、当前/历史 encoder profile
  契约、载荷前预算和显式版本分派；这是契约验证，不冒充强执行。
- opt-in `tst_pythongeneratedoutputs_native`：两种真实 SDK 输入→真实受控 Python→三端口
  认证分页→source 离线、新 registry/Host 与历史身份恢复发布恰一次；坏端口、截流、
  PNG 上限、第三私有写失败和同长同 mtime 暖页篡改均必须整组拒绝。
- opt-in `tst_pigeneratedoutputs_native`：受控 SSE 模型传输，实际 Pi/C++ LlmClient/Host、
  SDK SEG-Y、强隔离 Python、三成果发布与重启恢复；不同 provider ID、Pi→原生同轮
  重试均不得多次计算/提交。模型传输夹具不替代任何数值执行或提交。

两个 opt-in suite 缺少显式 Python/Pi 根、SHA 或强后端将失败，不以跳过伪装正例。
同 binary 的重启测试不伪造另一 Qt encoder，也不声称已实跑跨 Qt 升级。

生产源码独立只读复审未发现阻断问题。早期完整源码门禁 40/41，唯一失败为父 A2 任务
标题漏入 TS；已在父批补译并同步后续依赖，基线不变。正式编译、两配置、原生平台与
fetch/rebase 后全套门禁仍需逐项完成并记录，之后再提本批 PR。
