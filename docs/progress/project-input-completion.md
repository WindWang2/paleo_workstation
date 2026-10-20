# 工区原始输入补齐（2026-10-20）

用户明确授权直接修改 `Downloads/paleo_comp_data` 原始文件，转换 XLS，
使用相近曲线补齐 GR，并按 GR 生成缺失岩性，使所有井、所有层位可读取。
完整原件备份位于工区同级 `paleo_comp_data_before_input_fill_20261019-234515`。

## 数据交付

- 4 份 XLS 原件和对应工程副本，共 8 个文件，经 Excel SaveAs 转为 XLSX；
  对照 10,106 个非空单元格，工区内不再保留 XLS。84 份 XLSX 无公式错误。
- 全部 LAS 和原始资料副本补齐 GR、深度覆盖与采样；同井伽马优先，
  A13 使用 GRDEN/VGRDEN，A8 使用最近邻 A17 的 GR 并按 D61 顶深平移。
  当前采样间距不大于 0.125 m，原始多余数据列保留并补上定义。
- 新增 A8/A13/A16/A17 岩屑 XLSX，其余井补齐岩性列与空白深度段。
  GR 中位数阈值 65/105 API 对应细砂岩、粉砂岩、泥岩。
- 20 井 × 34 地层 = 680 个组合；恢复 113 个原始已知分层记录，
  297 个缺失分层由邻井 D61 顶深平移补齐。完整分层作为各井主分层，
  最新 RAW 分层版本 ver-588，已保留原 ver-572 供完成任务追溯。
- 生成内容在 LAS 注释、XLSX 来源/批注/说明页及目录元数据中标记。
  当前数据库与 catalog.json 同步，382 个已登记版本的路径和 SHA256 全部一致。
- 8 编图层位经真实服务推理，各 20/20、0 跳过：D61 ver-573、C3 ver-575、
  C6 ver-577、D53 ver-579、D62 ver-581、D63 ver-583、D71 ver-585、D72 ver-587。
  预测版本标记含合成输入。QGZ 通过生产图层服务物化新结果，默认显示最新 D61。

工区内 `输入补齐说明.md`、`输入补齐记录.json` 与 `验收-*.json` 为交付记录。
原始预测和旧版结果仍保留为历史，未改写已冻结的旧预测载荷。

## 原生验收入口

新增 `tst_project_wellinputs`，通过 `add_paleo_test` 注册。
缺少显式环境变量时跳过数据集与远端操作，普通 CI 不读用户工区。

- `PALEO_WELL_INPUT_PROJECT`：显式数据集根目录。
- `PALEO_WELL_INPUT_REPORT`：逐井读取验收 JSON 输出路径。
- `actualProjectInputs`：原始岩屑与维护表两条读取路径各验证 680 个组合，
  复用真实 `planWellFaciesFormationRun`，最低 138 点满足当前 128 点窗口。
- `savedProjectReopens`：重开实际 QGZ，18 个保存图层全部有效。
- `PALEO_WELL_INPUT_RERUN`：显式逗号分隔层位，才执行真实推理并登记成果。
- `PALEO_WELL_INPUT_SAVE=1`：才允许物化已登记的新预测图层并保存工程。

真实推理先在工作目录内的完整工程副本执行，再核对结果并同步回原工区。
路径全部规范化，不依赖该验证副本。默认 ctest、`git diff --check`、
layering strict 通过；本次没有改变生产预测算法或放宽输入校验。
