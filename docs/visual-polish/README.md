# 方向 29 视觉实证档案

完整逐文件、逐画面索引与验收结论见 [ledger](../../.goal-loop-ledger-ui-visual-polish.md)。

- `before/1x`、`before/2x`：dda2faa 生产 UI 重新构建后的原始截图。
- `after/1x`、`after/2x`：当前生产 UI 重新构建后的原始截图。
- 每目录四份 capture JSON 记录源 diff、二进制哈希与画面清单；日志记录真实测试入口。
- `screenshots.json`：298 对尺寸和原始图像哈希；尺寸一致不能代替逐图视觉复核。
- `token-baseline-final.json` / `token-after.json`：精确候选与例外、668→0 非白名单违规。
- `contrast.json`、`semantic-diff.json`：对比度和文案/连接/布局构造守恒检查。
- `golden-paper-before.png` / `golden-paper-after.png`、`golden-review.json`：纸面 golden 原图复核，原测试容差保持。
- `tests-adjustment-failures.*`：修复阶段的失败现场，保留用于解释合法视觉预期更新及性能复验。
- `tests-oracle-final.*`：指定验收集与新 token 闸门，一次运行 19/19。
- `tests-final.*` / `tests-last-two-retest.*`：全量一次运行 252/254，两项计时失败原阈值独立复验通过；254 项最新结果全部通过，不宣称单次全量零退出。
- `tests-environment-failures.*` / `tests-isolated-retest.*`：前轮四项性能/写入失败现场与原阈值复验通过记录，保留失败证据。
- `native-gl-final.log`：独立原生 GL 检查 3 passed / 0 failed / 0 skipped。
- `validation-final.json` / `reviews.json`：最终证据汇总与两轮六维 High=0 / Medium=0 自审。

所有 PNG 都来自真实 Qt Widgets / 原生 OpenGL framebuffer，未修改像素、合成或缩放原图。数据为仓库 fixture 与测试合成资料。Linux offscreen Widgets 与 X11 / Mesa 软件 OpenGL 的范围、默认列宽与单行工具条遗留限制均在 ledger 说明。

三份全量 JUnit XML 中各有一行 QINFO 输出尾空格；可读 `.xml` 仅去除该空白以满足 diff 检查，完全原字节另存对应 `.xml.gz`，SHA256 登记在 `validation-final.json`。测试状态、断言和 stdout 内容保持。
