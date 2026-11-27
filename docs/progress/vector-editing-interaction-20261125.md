# 矢量编辑交互实施记录

基线为 `master@462592ca`。普通矢量层和三级相面工作副本共用主画布，各自保留适用的编辑规则。实施依据见[矢量编辑交互方案（#1114）](https://github.com/WindWang2/paleo_workstation/pull/1114)。

## 从主窗口复验

| 操作 | 应看到的结果 | 本地回归 |
| --- | --- | --- |
| 在交叉线、重合点或共边普通面上使用删除工具 | 悬停高亮候选。重合时按上下键预览，Enter 只删当前项，Esc 保留原状。框选后可以明确批删，撤销一次恢复整批 | `tst_edittools` |
| 修改几个要素后按 Ctrl+S，再从状态菜单保存图层 | 工程保存和图层修改分别反馈。图层提交、成果登记、井属性同步各有结果。未完成的阶段可重试，已登记版本可在重开工程后继续同步 | `tst_shell_projectlifecycle`、`tst_attrpanel`、`tst_p6_mapsave` |
| 在同层位建两份相图，在任务向导中继续编辑；新建前取消切换 | 从副本名选回目标。取消保留已有工作副本、工具和视野。通过同一工程的目录链接重新声明图层也能找到原版本 | `tst_mappingworkbench`、`tst_ui` |
| 新建相图，选已有工区和初始微相，或选择手绘范围 | 可预览实际边界与坐标系。已有边界的多部件及内洞随底面保留。首笔可撤回，保存后重新打开可核对覆盖范围 | `tst_faciespartition`、`tst_mappingworkbench` |
| 画一条 20 顶点分割线，让最后一点停在面内 | 右键后保留笔画，在提示下用 Shift＋左键移末点、退格或继续加点。完全越界保留待修，部分越界预览裁剪。Esc 取消本笔 | `tst_faciespartition`、`tst_edittools` |
| 开启跨层拓扑，在同坐标系的两个普通可写图层移动共享节点 | 画布获得焦点时一次撤销或重做作用于两层。任一栈不同步时保留现场并指出图层。说明输入框内的撤销仍归文本框 | `tst_edittools` |
| 第一次进入编图页，在底部“添加参考”选择图件 | 空状态只占一行。选图后展开两个横排槽位，按需创建画布。可增减槽位、拖动宽度、单独浏览或联动。目标图层与吸附、撤销始终可达 | `tst_ui`、`tst_mappingworkbench` |
| 给当前图斑添加备注，或为同一副本的后续编辑沿用说明 | 手写说明与后台草稿显示其归属。无模型时生成事实摘要。保存后返回的草稿留给原副本，采用后可进入下一版本 | `tst_mapping_async_explanation`、`tst_mappingpages` |

## Windows 原生验证

使用 Windows 11、Visual Studio 2022 的 Release 构建，以及仓库锁定的 QGIS 4.2.3、Qt 6.11.3、GDAL 3.13.3。构建目录中的 Office 入口可读。源码检查使用同一个 Windows 依赖前缀，包括显式指定 QGIS 头文件的传递依赖检查，共 41 项通过。

- 目录联接回归在普通用户权限下实际创建联接，从两条路径声明同一正在编辑的 GeoPackage。未提交的图斑仍可读，版本能从两条路径查到，切换到另一子表和原地覆写快照均被拦截。
- 附件导入回归通过 7 项。扩展名为 `.bin` 的有效测井文件能按内容导入，信息不足的表格进入确认，重复导入返回同一资产。另覆盖取消预览、预览后文件变化及切换工程。
- 桌面交互直接使用 Windows 窗口。主窗口 5 项、工作台 6 项通过。工具栏在浅色和深色、125% 和 150% 缩放的四种组合下各通过 7 项。这些计数含测试初始化与收尾。

在功能提交 `c1dff39d` 上编译了主程序和 16 个测试程序，使用 CTest 串行运行。下列 PowerShell 命令中的 `$build` 指向 Windows 构建目录。

```powershell
$cases = '^tst_(edittools|attrpanel|ui|mappingworkbench|faciespartition|mappingpages|mapping_async_explanation|constraintediting|draftautosave|p6_mapsave|c_workbench_commands|workbench_impactoutcome|shell_projectlifecycle|layerservice|staleness|agentattachmentimport)$'
ctest --test-dir $build --parallel 1 --output-on-failure --no-tests=error -R $cases
```

首轮 15 组通过。`tst_workbench_impactoutcome` 的 Qt 断言全部通过，但进程首轮耗时 41.95 秒，超过 CTest 的 30 秒上限。保留原配置单独复跑耗时 0.13 秒并通过。16 组汇总为 508 项通过，15 项按本地数据、截图或性能采样条件跳过。桌面截图与性能测量单独运行。后续仅修订了可选基准的首帧计时，提交 `c090f8bf` 上重新编译并通过完整 `tst_faciespartition`，耗时 2.30 秒。

## 大图响应

5 万面热态下，右键到下一次画布输入的中位数从 7.39 秒降至 3.15 秒，P95 从 7.91 秒降至 3.51 秒。首帧中位数从 7.41 秒降至 3.18 秒。结果来自 Windows 原生窗口、内存图层、固定视口下的 240 笔测量。完整分位数与逐笔样本见[性能报告](../perf/FACIES_CAPTURE_20261125.md)。

## 参考区的可绘面积

在 Windows 原生窗口固定左右侧栏为 165 和 230 个逻辑像素，主图中心为 (250, 50)，比例尺为 1∶10000。下表记录画布视口的逻辑宽高。Windows 上 100%、125% 和 150% 三种缩放得到相同尺寸。最后一列是在保存自定义布局、改变窗口尺寸并恢复布局后测得。

| 窗口 | 空参考 | 两份参考 | 恢复自定义布局 |
| --- | --- | --- | --- |
| 1440 × 960 | 1027 × 610 | 1027 × 411 | 1027 × 372 |
| 960 × 720 | 547 × 336 | 547 × 137 | 547 × 178 |

两种窗口中，收起空参考区都能为主图腾出 199 个逻辑像素的高度。窄窗首次展开两份参考时，主图高度从调整前的 68 增至 137 个逻辑像素。Windows 125% 缩放截图见[空参考](vector-editing-20261125/reference-compact.png)、[两份参考](vector-editing-20261125/reference-two.png)和[窄窗恢复布局](vector-editing-20261125/reference-restored-narrow.png)。

## 复现材料

初始 512 × 512 画布夹具中，重合点中心一次删除减少两个要素，普通十字线交点亦如此。回归夹具通过真实选择、删除工具与鼠标事件验证剩余几何、撤销栈和取消操作。运行 `tst_edittools ambiguousPointPickingAndDeletion` 时设置 `PALEO_EDIT_SHOT_DIR` 可保存画布与候选菜单，`PALEO_EDIT_SHOT_THEME=dark` 可复现深色主题。

交互走查保留了[基线主窗口](vector-editing-20261125/baseline-window.png)、修改后的[十字线画布](vector-editing-20261125/LineString-light-canvas.png)、[浅色候选菜单](vector-editing-20261125/LineString-light-candidates.png)和[深色候选菜单](vector-editing-20261125/LineString-dark-candidates.png)。后三张取自 Windows 原生 Qt 测试桌面，设备像素比为 1.5。125% 和 150% 下分别测得设备像素比 1.25、1.50，浅深两种主题的点、线、面候选均通过键盘选择回归。另在 125% 下通过鼠标事件记录了[20 点分割线失败后保留](vector-editing-20261125/split-20-vertices-retained.png)、[只修正末点](vector-editing-20261125/split-20-last-vertex-corrected.png)及[分割结果](vector-editing-20261125/split-completed.png)。

Windows 原生窗口回归 `resizedCanvasRestoresAutomaticFaciesAndLegend` 将窗口从 1600 × 1000 缩到 1040 × 720，主画布从 942 × 650 缩至 502 × 336。固定微相时，比例尺从 1∶225000 变为约 1∶422211，图例保持在微相层级。切回自动后比例尺不变，功能区与图例同时切至相层级。见[缩窗后恢复自动层级的 Windows 截图](vector-editing-20261125/automatic-after-resize.png)。

在 `tst_ui mappingWorkbenchCanvasRibbonAndReferences` 设置 `PALEO_EDIT_LAYOUT_QA` 可导出 1440 × 960 和 960 × 720 下的主窗口截图，日志同时报告窗口和画布的实际大小及设备像素比。相面 1 千、1 万、5 万图斑的确定性夹具及采样说明见[性能手册](../perf/BENCHMARKS.md)。
