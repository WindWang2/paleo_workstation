# Goal-Loop 方向 44：井曲线多格式读口——DLIS/LIS/BE + MD/TVD 对齐 + 挂接语义收口

## 背景（实测事实，勿再勘察）

并集读面已接到岩石物理/属性建模/剖面井轨/交会清单/测井综合图
（goal/well-logset 已落）。递延项（TODOS P3）：

- `src/io/` 只有 `lasparser/lasdoc/lascache/lasalias/laswriter`——
  **DLIS/LIS/BE 不读**（master 实测零文件）；`wellfileparsers` 勘察
  现有入口可挂点（测斜表 `parseDeviationText` 已落，勿重复造）。
- **本方向扩量约定（无逃逸口）**：三种格式的解析全集钉死——
  井名/全部曲线目录/全帧数据/单位与深度基准元数据**必须读全**，
  不是「头部可读」即可交差；不支持的子结构允许列白名单，
  但白名单内项需在 Oracle 逐条有夹具证据。
- 非驱动文件只做线性重采样，**不做 MD/TVD 对齐**——多 LAS 并集在
  深度基准不一致时语义静默错位。
- `attachLink` 仍把新挂链接升主——挂接语义与「主文件稳定性」的取舍
  未收口（勘察消费面谁依赖主文件）。
- 格式识别走 manifest 词表登记先例；解析失败逐条列因不静默
  （importledger 先例）。
- 依赖政策：vendored 优先——DLIS/LIS 解析若无成熟 vendored 方案，
  自写受限子集（井名/曲线头/帧数据）并在 ledger 记覆盖边界；
  禁引入 GPL 传染性组件。

## 环境接线

```bash
cd /home/kevin/projects/paleo_workstation && git fetch origin
git worktree add .worktrees/welllog-fmt -b goal/welllog-fmt-20261004 origin/master
cd .worktrees/welllog-fmt
ln -s /home/kevin/projects/paleo_workstation/vendor/superbuild/prefix vendor/superbuild/prefix
ln -s /home/kevin/projects/paleo_workstation/vendor/onnxruntime vendor/onnxruntime
[ -d /home/kevin/projects/paleo_workstation/vendor/prefix ] && \
  ln -s /home/kevin/projects/paleo_workstation/vendor/prefix vendor/prefix
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DQGIS_PREFIX=/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix
cmake --build build -j8
```

## 目标形态（建议按序）

1. **DLIS/LIS/BE 读口（全集钉死）**：井名/全曲线目录/全帧数据/
   单位与深度基准元数据读全——进 `lasdoc`/`welllogset` 同一消费面
   （消费面无感格式差异）；manifest 词表登记新 kind；不支持的
   子结构走显式白名单（每项列因），白名单外默认必须解析；
   RP66/LIS79 规范条目逐条对账进 ledger。
2. **MD/TVD 对齐**：非驱动文件挂接时读时深表（若有）做深度基准
   对齐；无时深表如实标「线性重采样」口径标签，**不冒充已对齐**；
   对齐结果进并集读面，消费面自动获益。
3. **挂接语义收口**：`attachLink` 升主行为勘察消费面依赖——
   定「主文件只由显式操作变更」或维持现状，选哪个都要写进契约
   注释 + 测试钉死；界面如实呈现当前主文件。
4. **失败诚实面**：坏帧/截断文件/基准缺失逐条列因进 importledger，
   零静默丢弃。

## 通用纪律（方向内全程有效）

- **分层**：`src/` 新文件头三行 `// 层：<词表>`；解析归 io，
  对齐/并集归 domain/io，语义收口归 workflow。
- **诚实面**：受限子集边界、对齐口径、主文件语义全部显式标注；
  不读的格式在导入清单里如实标「未支持」而非静默跳过。
- **资源**：构建/测试一律 `-j8`。
- **性能断言**：禁绝对毫秒墙钟；大 DLIS 流式读不整载内存。
- **兼容性**：新增格式不回归 LAS 既有用例（tst_welllogset 等全绿）。
- **ledger**：`.goal-loop-ledger-welllog-fmt.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审（分层/格式边界/
  对齐诚实/挂接契约/i18n 五维）→ 修复 → 再 review，
  **至少两轮零 High/Medium**；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. DLIS/LIS/BE 夹具各一：井名/**全部曲线**/全帧数据/单位/基准
   读进并集消费面，逐曲线数值与夹具生成源一致；白名单内子结构
   逐项有「不支持+原因」证据。
2. MD/TVD：两文件基准错位的并集，带时深表者对齐后曲线匹配
   参考道；无时深表文件标「线性重采样」口径可见。
3. 挂接语义：主文件变更路径唯一（显式操作），attachLink 后的
   主文件归属与契约注释/测试断言一致。
4. 失败面：截断 DLIS/缺段文件逐条进 ledger，不 crash 不静默；
   畸形帧边界（半帧/重叠帧）有专项断言。
5. 回归：LAS 全部既有用例绿，格式间消费面无差异（同一曲线
   接口）；三格式曲线混入同一并集时深度基准按各自元数据归一。
