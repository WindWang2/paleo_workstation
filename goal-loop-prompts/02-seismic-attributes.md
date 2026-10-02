# Goal Loop — goal/seismic-attributes：地震属性计算引擎（新功能）

你接手一个自治迭代循环。方向：**给工作站补上「地震属性」这一解释级核心能力**——
从 SEG-Y 数据体实时计算解释属性并上图。这是新功能（非修 bug），纵深大、可拆分多轮。

## 背景事实

- 数据通路已通：`vendor/sbm`（SgyVolume/索引/分页缓存）+ `src/io/segyindexstore.*` +
  `src/services/seismicmapping.*`/`seismictaskservice.*` 异步任务编排；真机 966MB 体
  实测 open ~271ms、IL 切片 ~64ms、64³ 体窗 ~46ms、对角剖面 ~97ms——读路径可用。
- 现有算法层 `src/algorithms/`（层：数据，禁 QtWidgets）：paleoalgorithms、
  faciespolygonize 等先例可参考接口风格；`add_paleo_test` 注册测试。
- 地震属性是解释工作站面包黄油：当前产品无属性计算面（勘察确认范围后再钉清单）。
- 真机数据：`PALEO_REAL_PROJECT_AREA=/home/kevin/projects/paleo_project/data/project_area`
  门控实测；fixture：`synthetic_4x5.sgy`、`mini_seismic.sgy` 在测试资产内。

## 范围（建议子项，按需取舍/排序；全部做完或做到 Oracle 为止）

1. **属性核函数库**（`src/algorithms/seismicattr.*` 或新模块——新顶层模块先
   `scripts/new_module.sh` 登记词表）：
   - 瞬时属性：包络（Hilbert）、瞬时相位、瞬时频率、Q 估计原型；
   - 振幅类：RMS/最大振幅/能量 时窗属性；
   - 相干体：3×3×3 semblance 相干（沿 IL/XL 窗口），输出属性体；
   - 曲率/甜点（sweetness）视实现成本取舍。
   全部纯数据层：输入体窗/切片句柄 + 参数 → 输出缓冲，不碰 UI/QWidget。
2. **服务编排**：`SeismicTaskService`/`seismicmapping` 挂属性任务（复用既有异步
   worker+进度/取消模式），结果写临时属性体或栅格层，catalog 登记产物条目。
3. **上图/交互**：属性作为图层进入层树（走 layermanifest/角色体系），剖面/切片视图
   可选属性叠加（混合权重或并排），属性计算面板（参数表单→信号→服务）。
4. **正确性验收**：合成数据夹具——已知频率正弦道、已知断层模型的相干响应、
   RMS 窗手算值；测试断言数值而非截图。
5. **性能**：属性计算走分块+并行；给 966MB 体 IL 切片包络/相干延迟实测入档
   （docs/progress/seismic-attributes.md），比率门测试而非绝对墙钟。

## Oracle

1. 至少 3 类属性可用路径闭环：面板参数 → 服务任务 → 结果上图/切片叠加，
   offscreen 可驱动全链（fixture 体）。
2. 数值正确性测试：每类属性至少 2 个合成断言（已知输入→解析期望值比对，
   容差明确写因由）；相干体加方向性用例（沿层连续高相干、跨断层低相干）。
3. 取消/进度：长任务可取消（现有任务服务语义复用），进度信号单调到达 100%。
4. 性能档案：966MB 实测表（每属性×切片粒度）入 docs/progress；一个比率门测试。
5. 分层绿（`--strict`），新增文件头三行层标记；vendor 改动登记 PATCHES.md。
6. ctest 全绿（新增测试含其中）；ledger；push + `gh pr create`。

## 勘察指引

- 读 `src/services/seismictaskservice.*` 与 `src/io/segyindexstore.*` 拿任务编排/
  数据访问接口形状；`vendor/sbm` 内 SgyVolume/索引 API（前有 P5 补丁先例，
  字位约定：inline=field record@8、crossline=CDP@20，INLINE@188/CROSSLINE@192 恒 0）。
- `src/algorithms/paleoalgorithms.cpp` + `tests/tst_algorithms.cpp`：算法层接口与
  测试风格先例。catalog 产物登记看 `DataImportService`/`catalogindex` 角色机制。
- Hilbert：Qt 生态无现成——用 Eigen/自写 FFT 或 vendor 已有 FFT 依赖勘察后定
  （优先不引新依赖；sbm/Eigen 若有现成 FFT 用之）。
- UI 挂点：找属性面板落位（DataOps/新 dock 或剖面属性条），视图只发信号。

## 禁区

- 不动剖面运行时调度/取消骨架重写（属另一方向范围）——你只复用其任务接口。
- 不改 catalog 写路径实现细节（仅使用其公开登记 API）。
- 不引入新第三方库除非必要（Eigen/FFTW 之类先查 vendor/ 已有物）。
- 地质约定：属性定义用标准公式并在代码注释给引用名；不确定的语义在 ledger 记录
  所做选择，不臆造单位。

## 迭代协议

- 轮0-1：勘察（接口面/夹具体/FFT 可得性）→ 技术选型入 ledger → 核函数先落数据层。
- 中段：每属性一类一轮（核+单测→服务挂点→上图→实测表）。
- 完成定义 = Oracle 6 条全绿，ledger 轮次齐全。
