# Goal-Loop 方向 41：单因素原生算法深化——变差函数克里金 + SFPKG/外委格式批量读取

## 背景（实测事实，勿再勘察）

单因素原生已落 C++ 局部方向 IDW/软边界/井群权重/硬屏障栅格连通/
真实数值等值线（`src/algorithms/singlefactor/`）——**上游井数 >80
等条件下各向异性路径会退成 IDW，UI 标签不得冒充克里金**（TODOS P2）。

- 克里金是独立立项：变差函数拟合（球/指数/高斯模型 + 块金/基台/变程）
  + 克里金系统方程求解；勘察 `singlefactor/` 现有权重求解骨架
  （lascache/curvekernel/faultpath 的可复用件）——**复用插值面接口，
  新增变差函数拟合模块**。
- 外委格式：SFPKG 完整导入、XML/XLSX 批量读取——勘察 `src/io/` 现有
  读取面与 singlefactor 的输入契约（cartographicworkfile.h 先例），
  格式识别走 manifest 词表登记，不旁路。
- 时深域转换、监督分类、Python GUI 嵌入、完整历史制图策略——
  **明确不在本方向**（各需独立契约，ledger 记递延）。
- FaultPathMetric（有限断层路径距离）：勘察 `faultpath.{h,cpp}` 与
  grid_connectivity_v1 硬屏障模型的差距，能立则立，不能则 ledger
  记边界。
- 参考工程策略：勘察制图工作场参数面，把可枚举的历史制图策略
  模板化（参数包词表），不做 GUI 全复刻。

## 环境接线

```bash
cd /home/kevin/projects/paleo_workstation && git fetch origin
git worktree add .worktrees/sf-kriging -b goal/sf-kriging-20261004 origin/master
cd .worktrees/sf-kriging
ln -s /home/kevin/projects/paleo_workstation/vendor/superbuild/prefix vendor/superbuild/prefix
ln -s /home/kevin/projects/paleo_workstation/vendor/onnxruntime vendor/onnxruntime
[ -d /home/kevin/projects/paleo_workstation/vendor/prefix ] && \
  ln -s /home/kevin/projects/paleo_workstation/vendor/prefix vendor/prefix
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DQGIS_PREFIX=/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix
cmake --build build -j8
```

## 目标形态（建议按序）

1. **变差函数**：实验变差计算（滞距分桶/容差）+ 模型拟合（球/指数/
   高斯 + 块金/基台/变程最小二乘）；拟合失败/退化数据如实报因，
   不给假参数。
2. **克里金插值**：普通克里金系统求解（对称正定；奇异/病态如实
   回落并说明）；与 IDW 同插值面接口——`method=kriging` 出真克里金，
   井数阈值/病态回落行为与 UI 标签一致（不冒充）。
3. **SFPKG/外委格式**：SFPKG 完整字段读取 + XML/XLSX 批量——
   新格式进 manifest 词表登记；行级坏数据如实列因不静默。
4. **制图策略模板**：可枚举的参考工程制图参数包词表化（勘察工作场
   参数面）；UI 标签与真实算法名严格一致。

## 通用纪律（方向内全程有效）

- **分层**：`src/` 新文件头三行 `// 层：<词表>`；变差/克里金/路径距离
  归 algorithms，格式读取归 io，参数模板归 domain/workflow。
- **诚实面**：拟合失败/矩阵病态/格式不识如实报因；UI 标签与真实
  算法严格一致（IDW 不标克里金）。
- **资源**：构建/测试一律 `-j8`。
- **数值正确性**：克里金在采样点精确通过（exactitude）、无数据处
  趋均值面、与已知解析解的合成断言。
- **性能断言**：禁绝对毫秒墙钟；网格插值用合成格 + 比率门。
- **ledger**：`.goal-loop-ledger-sf-kriging.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审（分层/数值正确/
  诚实标签/格式诚实/i18n 五维）→ 修复 → 再 review，
  **至少两轮零 High/Medium**；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 克里金精确性：合成变差模型生成的场，克里金在采样点无误差通过，
   远场趋近理论均值（合成断言，非肉眼）。
2. 诚实标签：病态/退化输入 → 明确回落路径与说明，UI 不显示
   未发生的克里金。
3. 变差拟合：已知参数模型生成的实验变差 → 拟合参数在容差内还原
   （球/指数各一例）。
4. SFPKG/XML/XLSX：三类夹具读取字段全齐，坏行逐条列因零静默。
5. FaultPathMetric 边界：能立则断层绕行路径距离与解析解一致；
   不能立 → ledger 记明不可立原因与递延面（本条按勘察结论二选一
   验收）。
