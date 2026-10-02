# Goal Loop — goal/time-depth-velocity：时深转换与速度建模（新功能）

你接手一个自治迭代循环。方向：**给工作站补上「时深转换」链路**——从井分层/校验炮
建速度模型，把时间域层位/剖面转换到深度域。这是解释工作站的核心地质能力之一，
新功能、纵深大、可拆分多轮。

## 背景事实

- 井数据已通：LAS 解析（`src/io/lasparser.*`、`lascache.*`）、wellcomposite 连井柱、
  `tst_seismic_welltie` 已有部分井震对标测试基建——勘察先摸清已有对标面到哪一步。
- 层位/网格：`src/io/horizonbinner.*` 把散点收成栅格；`src/metadata/layermanifest.*`
  管理图层清单；地震体读路径（vendor/sbm + segyindexstore）实测 966MB IL 切片 ~64ms。
- 算法层 `src/algorithms/`（层：数据，禁 QtWidgets）：paleoalgorithms、seismicattr、
  distancetransform 先例可参考接口风格；`add_paleo_test` 注册测试。
- 真机数据：`PALEO_REAL_PROJECT_AREA=/home/kevin/projects/paleo_project/data/project_area`
  门控实测（20 井 LAS + 966MB SEG-Y）。

## 范围（建议子项，按需取舍/排序；全部做完或做到 Oracle 为止）

1. **速度数据源接入**（`src/io/` 数据层）：井分层表（tops）/校验炮（checkshot）
   两种输入的最小解析器——井名、层位名、TWT/TVD 对、速度值；进 catalog 登记。
2. **速度模型核**（`src/algorithms/` 纯数据层）：
   - 层间平均速度 / V0-k（线性渐增）模型，由分层对拟合；
   - 指定 (x,y,twt) 查询速度的采样器；模型可序列化存档（catalog 产物）。
3. **时深转换执行器**：时间域层位栅格/散点 → 深度域输出（逐点查模型换算）；
   剖面视图深度标尺模式（时间轴↔深度轴切换信号面，转换计算在数据层）。
4. **正确性验收**：合成夹具——恒速模型（斜率解析解）、两层 V0-k 手算值比对、
   井位处转换结果必须精确命中分层深度（钉住模型锚点语义）。
5. **实测档案**：966MB 剖面时深转换延迟 + 层位面转换吞吐入
   `docs/progress/time-depth.md`，比率门测试。

## Oracle

1. 井分层/校验炮 → 速度模型 → 层位时深转换 全链闭环，offscreen 可驱动。
2. 数值断言：恒速/两层模型解析比对 ≥4 用例；井锚点处零误差断言。
3. 模型可存可取（catalog 条目），重复转换幂等。
4. 深度标尺/结果上图走层树与角色体系，视图只发信号。
5. 分层绿（`--strict`），文件头层标记；ctest 全绿；ledger；push + `gh pr create`。
6. 井震对标若已有基建则复用并对齐口径，不重写。

## 勘察指引

- `src/io/horizonbinner.*`：散点→栅格基建；`lascache`/`lasparser`：井曲线形状。
- `tst_seismic_welltie` 与 `src/ui/seismic3d/`：井震对标与剖面轴的既有挂点。
- catalog 产物登记：`DataImportService`/`catalogindex` 角色机制先例。
- UI 挂点：层位右键/属性面板加「转换为深度域」入口，视图层只发信号。

## 禁区

- 不动地震体读路径/索引格式；不重写井震对标既有实现（只做时深方向增量）。
- 不引入新第三方库（插值用 Eigen/自写，先查 vendor/）。
- 地质语义：速度模型公式标注出处名；TWT/TVD、深度基准（KB/MD/TVDSS）单位
  不确定时在 ledger 记录所做选择，不臆造。

## 迭代协议

- 轮0-1：勘察（tops/checkshot 数据形状、welltie 基建现状）→ 选型入 ledger。
- 中段：输入解析→模型核→转换执行器→上图，每件一轮（核+单测→服务挂点→实测）。
- 完成定义 = Oracle 6 条全绿，ledger 轮次齐全。

## 交付协议（必守）

1. **新建 worktree 分支开发**：`git worktree add .worktrees/time-depth -b goal/time-depth-<日期>`
   从最新 `origin/master` 起，全程不在主 checkout 写代码。
2. **完成后先仔细 review 再开 PR**——逐条执行，全部通过才准推送：
   - `git diff origin/master...HEAD` 自审全部改动：删掉调试残留、printf、注释掉的死代码；
     每个文件头三行层标记在；分层 `tools/check_layering.py --strict` 绿；
   - 全量构建无新警告（本分支引入的 warning 清零）；ctest 全绿（含新增测试）；
   - Oracle 每条在 ledger 里有对应验证证据（命令+输出摘要），不是「应该没问题」；
   - review 发现的问题先修再验，直到干净为止。
3. 推送 + `gh pr create`（标题 conventional，body 写 `## Summary`/`#### Test plan`/
   自审结论清单）。
