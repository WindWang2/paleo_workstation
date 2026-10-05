# Goal-Loop 方向 43：地震属性体化——时间切片/整体扫描 + 属性图层入层树

## 背景（实测事实，勿再勘察）

属性计算目前仅 IL/XL 剖面切片（TODOS P3 递延）。已有基础：

- `src/algorithms/seismicattr.{h,cpp}`：属性核函数库已落（瞬时族需整道谱、
  时窗族需垂向窗——时间切片单采样面不满足输入形状，需全测网分块扫描）。
- `SeismicTaskService`：异步任务面齐备（闸/取消/LRU/回落），剖面属性
  已走它——体扫描复用同通道，禁新起线程池。
- SATR 容器：头已带参数/几何，**只写不读**——读回器是新增件。
- `layermanifest`：层树条目需诚实栅格 URI——剖面属性图非地理参考，
  时间切片（IL×XL 地理栅格）才具备入树资格。
- 缺口三连：体/切片分块扫描调度、`.sattr` 体化格式、层树挂接；
  附带相干沿剖轴道距加权（当前等权）与结果同参数去重缓存。
- **本方向扩量约定（无逃逸口）**：体化格式与 3D 体视渲染消费面
  **均为必做**，不进 ledger 递延；相干加权与去重缓存同为必做。

## 环境接线

```bash
cd /home/kevin/projects/paleo_workstation && git fetch origin
git worktree add .worktrees/attr-volume -b goal/attr-volume-20261004 origin/master
cd .worktrees/attr-volume
ln -s /home/kevin/projects/paleo_workstation/vendor/superbuild/prefix vendor/superbuild/prefix
ln -s /home/kevin/projects/paleo_workstation/vendor/onnxruntime vendor/onnxruntime
[ -d /home/kevin/projects/paleo_workstation/vendor/prefix ] && \
  ln -s /home/kevin/projects/paleo_workstation/vendor/prefix vendor/prefix
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DQGIS_PREFIX=/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix
cmake --build build -j8
```

## 目标形态（建议按序）

1. **SATR 读回器**：容器头信息齐备——补只读面（参数/几何/切片寻址），
   round-trip 断言与写端一致。
2. **时间切片属性**：分块扫描调度（IL×XL 栅格输出、地理参考）——
   瞬时族整道谱/时窗族垂向窗按属性族各取所需窗口；调度走
   SeismicTaskService，取消/顶替语义对齐切片取数。
3. **体化格式（必做）**：`.sattr` 体容器设计落地——头（参数/几何/
   属性族/块表）+ 分块 payload；写/读双向；整体属性体（瞬时/时窗
   族全体扫描）产出落体容器而非散片。
4. **3D 体视消费面（必做）**：属性体入 3D 显示路径（勘察既有
   seismic-3d 呈现面接体渲染/切片联动）；体级进度/取消走任务面板。
5. **层树入树**：时间切片属性结果落 DERIVED 版本 + layermanifest
   层树条目（诚实栅格 URI）；剖面属性不冒充层树条目。
6. **附带（必做）**：相干沿剖轴道距加权参数化（加权/等权双档，
   结果差异有断言）；同参数结果去重缓存（hash 参数包→命中复用，
   缓存命中在 provenance 可见）。

## 通用纪律（方向内全程有效）

- **分层**：`src/` 新文件头三行 `// 层：<词表>`；属性核/扫描调度归
  algorithms/services，层树挂接归 metadata/workflow，视图只发信号。
- **诚实面**：输入形状不满足（切片算时窗族）如实拒绝报因；
  层树条目不挂非地理参考图。
- **资源**：构建/测试一律 `-j8`。
- **UI**：对照 `DESIGN.md`；i18n 两门；进度/取消走任务面板样式。
- **性能断言**：禁绝对毫秒墙钟；分块扫描用合成体 + 比率门，
  峰值 RSS 有界（滑窗不全体驻留）。
- **冲突**：seismictaskservice/layermanifest 为多点热区——与他方向
  （尤其 37 层位 3D）撞时按语义并集合。
- **ledger**：`.goal-loop-ledger-attr-volume.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审（分层/输入形状
  诚实/取消语义/层树 URI/i18n 五维）→ 修复 → 再 review，
  **至少两轮零 High/Medium**；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. SATR round-trip：写端产物读回参数/几何/数据逐字段一致。
2. 时间切片：合成体某时间切片属性 → IL×XL 地理栅格，数值与逐道
   重算一致（合成断言）。
3. 体化：整体属性体落 `.sattr` 容器——块表寻址 round-trip、
   任意 IL/XL/切片面抽取与直算一致；3D 体视路径显示证据
   （渲染帧或切片联动截图入 ledger）。
4. 入树：切片属性层树条目可开/可上图，URI 指真实栅格；剖面属性
   不出现在层树。
5. 调度：扫描可取消（无半成品 DERIVED）；同参数二次扫描**必须**
   命中缓存且 provenance 可见命中；相干加权/等权两档结果差异断言。
6. 形状拒绝：时窗族在单采样切片上如实拒绝，文案指明所需窗口。
