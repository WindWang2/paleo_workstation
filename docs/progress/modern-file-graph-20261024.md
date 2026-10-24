# 2026-10-24 文件图谱 modern 视觉更正

用户要求图谱样式也接近给定知识图谱截图。此变更覆盖上一版的常驻三栏、椭圆集合和矩形文件节点。

- 画布铺满，左侧节点详情、右侧图例/关联、左下真实计数、右下缩放控件以悬浮卡片呈现。无选择不占用详情区，禁用操作不堆成按钮墙。
- 分类使用 coral / purple / mint / amber / cyan / peach 圆点；实体内的数字为真实去重文件数。数据颜色、局部尺寸与字体写入 DESIGN.md，颜色集中在主题定义。
- 数字/图标固定深墨 #1F2937，对六种节点填色的对比度最低约 4.71:1；浅深主题共用类别色。
- 圆点、名称、箭头保持屏幕尺寸；缩放只改变网络间距，避免全图变成无法阅读的微小节点。曲线的端点与标签在缩放后重新计算，箭头停在真实圆边缘。
- 选中邻域突出，其余网络淡化；属性以转义后的键值表展示，主题切换同步更新内联属性颜色。原有真实归属、血缘、预览与返回导航沿用。
- 工具栏使用随主题变化的单色线条图标，完整筛选集中于筛选菜单。窄窗默认收起右侧浮层，关系按钮可切换查看且不会同时覆盖左侧详情。

## 验证

Windows MSVC + 本机既有依赖环境，构建并行不超过 8。构建 paleo、tst_datagraph、tst_datagraphpage。
图谱数据层回归 30 个 Qt 用例通过；页面回归包含原有返回、版本预览与重新定位，以及新增 40 节点样本、圆形/分类色、全画布悬浮结构、窄窗切换、缩放后屏幕直径不变。
测试使用仓库 Windows 入口规定的 build/paleo-tmp，避免该机器对仓库进程的树外临时目录限制。

实际 Qt 截图位于 build/modern-graph-visual/：modern-file-graph-overview-{light,dark}.png、modern-file-graph-focus-{light,dark}.png。
截图取自测试 catalog（10 口井、30 份资料、30 条真实登记关联），没有伪造生产数据关系。

最终验证：paleo 与两个图谱测试目标编译成功；tst_datagraph 为 30 passed，tst_datagraphpage 为 12 passed、0 failed、0 skipped。页面测试包含变换后节点屏幕直径保持不变的断言。
源码门禁：layering strict、Qgs audit/spread strict、UI invariants/size/tokens strict、file budget strict、i18n、docs drift 均通过。构造函数规模冻结值 225→219，其余基线不增长。
浅深主题的概览与选中截图已目视检查，包括属性行、节点及文件名、箭头、分类色、浮层边距、滚动区域、工具栏和缩放工具。
未更改导入或项目创建逻辑，未把演示样本写入用户工程。
