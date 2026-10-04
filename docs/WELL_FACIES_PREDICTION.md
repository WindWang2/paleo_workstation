# 测井面板的网络相预测

打开测区井的测井预览或辅助资料中的综合柱状图，工具栏提供模型选择、
「预测相」「停止等待」「显示预测相」「刷新模型」「预测服务」。
参考井不需要关联测区井号或井位坐标即可运行相预测。

首次使用在「预测服务」填入服务地址和 API 密钥。**没有缺省地址**（#133：
不再硬编码任何第三方地址），请向服务提供方索取正式地址。

传输安全（#133）：

- 只接受 `https://`。`http://` 仅对本机回环（`localhost` / `127.x` / `::1`）
  放行，便于本地调试服务。
- 内网只有 http 的部署，须在「预测服务」勾选「允许不加密的 HTTP」（或设
  `PALEO_WELL_FACIES_ALLOW_INSECURE_HTTP=1`）——此时 API 密钥与整井曲线明文
  过网，状态栏给出警告。不建议用于公网地址。
- 不跟随重定向；响应体超过 64 MiB 即中止。

密钥存储：

- 构建找到 Qt6Keychain（QGIS 的既有依赖；Linux 装 `qtkeychain-qt6-dev`）时，
  密钥存入系统钥匙串（Windows 凭据管理器 / Secret Service(libsecret、KWallet) /
  macOS 钥匙串），`~/.config/paleo/well-facies.json` 只存地址与开关；旧版 JSON
  里的明文密钥在首次启动时自动迁入钥匙串并从 JSON 删除。
- 构建未带 Qt6Keychain，或钥匙串写入失败时，回落为 JSON 明文存储：POSIX 下
  权限 0600（仅当前用户可读写）；**Windows 下文件权限不提供同等保护**，请依赖
  用户目录 ACL，或改用环境变量。
- 也可用 `PALEO_WELL_FACIES_URL`、`PALEO_WELL_FACIES_API_KEY` 环境变量覆盖
  （环境变量里的密钥不会写入钥匙串或文件）。`PALEO_WELL_FACIES_NO_KEYCHAIN=1`
  可关闭钥匙串（ctest 沙箱默认设置）。

模型要求从 `GET /models` 获取。按钮按选中模型检查曲线、段、岩性、地层组、
连续深度和窗口点数；不可用时面板和 tooltip 给出具体原因。
段来自实际段分层，不能以组替代；岩性来自岩性井道。
曲线按模型字段名匹配（忽略大小写），不将其他曲线自动当成 GR。
不同采样网格只在有效曲线端点之间作线性插值，不外推、不补缺失值。
仅提交模型指定地层组的连续井段，不把整井当作目标组。

`POST /predict` 使用当前模型版本 ID。200 完成后展示结果；202 按服务的
`pollAfterMs` 查询 `GET /predictions/{jobId}`，不会重新提交。
网络操作采用 Qt 异步请求，HTTP、非法 JSON 和 failed/canceled 状态均显示原因。
轮询固定在配置的服务地址下，不向响应内的其他主机发送密钥。
「停止等待」停止客户端请求；服务端没有取消 API，已受理任务会继续执行。
同一输入在当前面板中再次点击预测时，会继续查询已知任务 ID。

完成后新增独立「预测相」文本道和「预测置信度」曲线道（0–100%），不改写
原有相解释。显示开关同时控制这两道，井道的现有导出能力可输出结果。
完整响应以模型、服务、井名和实际输入内容的 SHA-256 为键保存到本机
`~/.local/share/paleo/well-facies/`；重新打开相同数据会恢复结果。
换井、切模型或修改输入时清理旧结果，避免把上一口井的预测留在当前井。

实现分层：`domain/welllogfacies` 校验输入/结果；`ai/wellfaciesservice`
负责 API 2.0 传输；`workflow/wellfaciesworkflow` 编排与缓存；
`ui/wellcomposite` 发出意图并渲染；`app/main` 注入功能层工厂。

测试：`ctest --test-dir build -R 'wellfacies|wellcomposite|layering' --output-on-failure`。
`tst_wellfacies` 默认使用本地 HTTP 服务器，不调用外网。
`PALEO_WELL_FACIES_TEST_XML` 可指定真实综合柱状图来检验输入；另设
`PALEO_WELL_FACIES_LIVE=1` 才执行该井的真实网络推理。
若同时指定 `PALEO_WELL_FACIES_SCREENSHOT`，可保存面板截图；未开启 LIVE 时
截图使用本地测试服务器的结果，日志会明确标注该来源。

批量验证本机的 XML 综合柱状图（不上传原始资料、不调用外网）：

```sh
QT_QPA_PLATFORM=offscreen \
PALEO_WELL_FACIES_TEST_DIR=/path/to/xml-directory \
PALEO_WELL_FACIES_TEST_REPORT=/path/to/report.json \
build/tst_wellfacies realReferenceDirectory
```

逐口输出井名、可用状态、有效点数和不可用原因；对两种井入口检查按钮状态。
符合条件的井通过本地 HTTP 测试服务验证预测道和置信度道的显示。
测试目录和报告路径均为可选配置，未提供目录时跳过批量测试。

2026-10-03 验证：新功能、测井回归及层检查 9/9 通过；无 ORT 的
`paleo_ai` 构建通过。真实参考井 HZ26-6-1 的恩平组 3089–3356 m
输入校验通过（2670 点），`GET /models` 成功，但 `POST /predict`
返回 HTTP 500、`INTERNAL: 服务内部错误`，真实推理结果尚未验证。
测试工区既有的 HZ28-6-1 没有当前模型要求的恩平组，不能提交给该模型。

同日补充验证 `/home/kevin/projects/Download_backup/excel/` 的 57 份综合柱状图：
17 口井可用、40 口井不可用，57 个数据案例全部通过。
不可用原因：23 口无恩平组、9 口无 GR、6 口目标井段有无效 GR、
1 口缺少段/岩性覆盖、1 口无段井道。
全部 17 口可用井均在辅助井和测区井入口通过本地测试服务验证结果显示；
真实服务对 HZ19-1-1A（5827 点）和 XJ24-6-2D（4429 点）的推理请求
均返回 HTTP 500 / INTERNAL，真实预测结果仍未验证。
