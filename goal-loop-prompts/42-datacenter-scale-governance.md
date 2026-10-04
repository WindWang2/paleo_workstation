# Goal-Loop 方向 42：数据中心规模治理——资产表虚滚动 + 存储治理台

## 背景（实测事实，勿再勘察）

catalog 后端规模面已打平：查询 O(1)（邻接索引）、mutator 写路径
379×（10k 灌库 1.14s）。**未平的缺口在 UI 侧与治理面**：

- `src/ui/pages/datalist.cpp` `refreshAssetTable()`：每次刷新
  `setRowCount(0)` + `for (const CatalogAsset &a : cat->assets())`
  全量重建 + 逐行 cellWidget——10k 级目录即卡顿，100k 不可用。
  （测试夹具现成：`makeSyntheticCatalogDir` + `PALEO_CATALOG_SCALE`
  门控，tst_catalog_scale 先例）。
- 存储治理全缺：工程目录内未被 catalog 登记的文件（孤儿文件）无检测；
  `HealthIssue::sizeBytes` 字段已备（cataloghealth.h）但无按实体/类型的
  体积汇总面；stale 衍生版本只能人工感知（`markDownstreamStale` 已写
  `extra["stale"]`），无批量清理预览。
- 体检服务先例：`paleo::health::buildCatalogHealth`（纯查询、GUI 线程
  毫秒级）+ `verifyExternalShas`（worker 线程两段式）——新检测项沿用
  「GUI 只做 stat/内存查，重活离线程」纪律，不重造检测。
- 挂载点：dataops 面板族（`dataopspanelextra.h` TopologyGraph 同构；
  `dataopspanelops.h` 回收站 purge 先例）+ datalist 表格。

## 环境接线

```bash
cd /home/kevin/projects/paleo_workstation && git fetch origin
git worktree add .worktrees/datacenter-gov -b goal/datacenter-gov-20261004 origin/master
cd .worktrees/datacenter-gov
ln -s /home/kevin/projects/paleo_workstation/vendor/superbuild/prefix vendor/superbuild/prefix
ln -s /home/kevin/projects/paleo_workstation/vendor/onnxruntime vendor/onnxruntime
[ -d /home/kevin/projects/paleo_workstation/vendor/prefix ] && \
  ln -s /home/kevin/projects/paleo_workstation/vendor/prefix vendor/prefix
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DQGIS_PREFIX=/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix
cmake --build build -j8
```

## 目标形态（建议按序）

1. **资产表虚滚动**：刷新改增量（diff 应用或 model/reset 分段），
   大目录按可视区惰性填行/分页——不动 catalog API，纯视图层；
   `refreshAssetTable` 语义（过滤/排序/选中保持）逐条回归。
   cellWidget 行先量化：渲染行数受可视区限，不随资产总数线性。
2. **存储治理台**（cataloghealth 同族新面板或体检对话框扩页）：
   - 按实体/资产类型聚合体积（`sizeBytes`/`resolvedVersionPath` stat）；
   - 孤儿文件检测：工程受管目录内未被任何版本引用的文件——
     走 pathcanon 归一化比对，列出清单带大小，禁猜用途；
   - stale 衍生版本清单：`extra["stale"]==true` 聚合列出 + 原因列。
3. **批量治理动作（预览制）**：孤儿文件/stale 版本的批量回收——
   先出预览清单（数量+体积+影响版本数），确认后走既有 purge/
   mutator 路径；禁绕 catalog 直接 unlink 文件。
4. **规模回归**：10k 夹具下表格刷新/过滤/选中定位有比率门断言
   （相对基线不劣化）；UI 线程无整目录 stat 风暴（stat 入 worker
   或惰性分页）。

## 通用纪律（方向内全程有效）

- **分层**：`src/` 新文件头三行 `// 层：<词表>`；检测/聚合归
  services/catalog，面板归视图层，编排归 workflow；视图只发信号。
- **诚实面**：孤儿文件只列「未被引用」事实不猜语义；SHA 未扫完/
  检测未覆盖如实标；批量动作全部预览制，数量体积先行。
- **资源**：构建/测试一律 `-j8`。
- **UI**：对照 `DESIGN.md`；沿用 dataops 面板 token；i18n 两门；
  `objectName`+accessibleName 齐。
- **性能断言**：禁绝对毫秒墙钟；用 10k 夹具比率门（刷新耗时/
  渲染行数 vs 资产总数）。
- **冲突**：datalist.cpp/cataloghealth.* 为多点热区——与他方向撞时
  按语义并集合。
- **ledger**：`.goal-loop-ledger-datacenter-gov.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审（分层/线程纪律/
  预览制/选中保持/i18n 五维）→ 修复 → 再 review，
  **至少两轮零 High/Medium**；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 规模回归：10k 资产目录表格刷新渲染行数与可视区同阶（不随总数
   线性），过滤/排序/选中保持逐条过。
2. 体积汇总：按实体与按类型两种聚合与逐项 stat 总和一致（合成
   夹具断言）。
3. 孤儿检测：工程目录内塞入未登记文件 → 清单命中且大小正确；
   已登记文件零误报（路径归一化证据）。
4. stale 治理：造 stale 衍生链 → 清单列出原因；批量回收预览数
   量/体积与实际执行一致。
5. 线程纪律：SHA 复验/目录扫描期间 UI 可交互（进度可见、可取消，
   取消后无半成品动作）。
