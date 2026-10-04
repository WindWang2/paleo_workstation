# Goal-Loop 方向 37：层位自动追踪 3D 化——体传播服务暴露 + 显式倾角引导

## 背景（实测事实，勿再勘察）

2D 剖面追踪闭环已落地；3D 核已存在但未暴露：

- `src/algorithms/horizontrack.{h,cpp}`：`propagateVolume` 数值核 + 合成断言
  已落（死列不复生/限步长/取消语义，tst_horizontrack 3 例）——**本方向
  做的是服务化与画布渲染，不重写数值核**。
- 服务通道：`src/services/seismictaskservice.h`（并发闸/取消/LRU/回落
  语义齐备，剖面取数已全量迁入）——体传播走同一通道，禁新起线程池。
- 画布叠加：剖面/平面画布管线经 `QgisCanvasController`，层位面上图有
  GeoTIFF 上图管线先例（horizon-autotrack 2D 轮已落）。
- 追踪核缺陷（TODOS P3）：无事件唯一性校验，近距平行同相轴可滑落——
  显式斜率扫描倾角引导是现行阈值/相干门之外的第三道防线（当前隐式：
  搜索窗中心跟随前一道）。
- 内存边界：体窗 IL 邻域滑窗调度是新增工作量；取消语义核内已有，
  服务层照 startSliceExtraction 模式接 requestCancel + 请求号守卫。

## 环境接线

```bash
cd /home/kevin/projects/paleo_workstation && git fetch origin
git worktree add .worktrees/horizon-3d -b goal/horizon-3d-20261004 origin/master
cd .worktrees/horizon-3d
ln -s /home/kevin/projects/paleo_workstation/vendor/superbuild/prefix vendor/superbuild/prefix
ln -s /home/kevin/projects/paleo_workstation/vendor/onnxruntime vendor/onnxruntime
[ -d /home/kevin/projects/paleo_workstation/vendor/prefix ] && \
  ln -s /home/kevin/projects/paleo_workstation/vendor/prefix vendor/prefix
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DQGIS_PREFIX=/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix
cmake --build build -j8
```

## 目标形态（建议按序）

1. **服务暴露**：`SeismicTaskService` 新增体传播任务面——种子点/参数包入、
   进度/取消/结果（层位面 grid）出；并发闸与剖面取数同池，顶替语义
   对齐 startSliceExtraction（cancelled≠failed 静默丢弃）。
2. **倾角引导**：追踪核补显式倾角先验——斜率扫描窗估计局部倾角并约束
   下一道搜索窗中心（不替换现有阈值/相干门，是第三判据）；无引导/估计
   失败如实回落现行行为。
3. **画布上图**：传播结果层位面入画布叠加（GeoTIFF 管线先例）；
   传播中进度可见、可取消；结果落 catalog DERIVED 版本（parentVersionIds
   锚源体版本）。
4. **内存调度**：IL 邻域滑窗按行推进——不全体驻留；体量超阈值如实拒绝
   并给原因，不 OOM 硬扛。

## 通用纪律（方向内全程有效）

- **分层**：`src/` 新文件头三行 `// 层：<词表>`；传播/调度归 services/
  algorithms，画布部件归视图层。
- **诚实面**：种子无效/无可用体/内存超限如实报因；不落半成品层位面。
- **资源**：构建/测试一律 `-j8`。
- **UI**：对照 `DESIGN.md`；进度/取消控件走既有任务面板样式；i18n 两门。
- **性能断言**：禁绝对毫秒墙钟；体传播用合成小体 + 比率门。
- **冲突**：paleomainwindow_attach.cpp 为多点挂接热区——与他方向撞时
  按语义并集合，不丢弃他人改动。
- **ledger**：`.goal-loop-ledger-horizon-3d.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审（分层/取消语义/
  内存边界/版本锚定/i18n 五维）→ 修复 → 再 review，
  **至少两轮零 High/Medium**；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 合成体单种子 → 整层位面：传播收敛、层位面上图且落 DERIVED 版本
   （parentVersionIds 锚源体）。
2. 倾角引导差异证据：同一近距平行同相轴合成体，无引导滑落/有引导不滑
   落（或如实标边界），对比数据入 ledger。
3. 取消语义：传播中 cancel → 无半成品版本、无悬挂任务、UI 复位可用。
4. 顶替语义：连续两枚种子 → 前者静默丢弃、后者生效（同切片顶替口径）。
5. 内存调度：大于滑窗的合成体传播，峰值 RSS 有界（比率门，不测绝对值）。
6. 空态：无种子/无体 → 入口禁用带原因，不 crash 不空白。
