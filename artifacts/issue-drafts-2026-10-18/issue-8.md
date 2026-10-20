# [med] check_file_budget.py --strict 红且全仓零接线，本批把 baseline 反向上调约 20 处

## 问题

文件规模闸 `check_file_budget.py --strict` 退出码 1（5 条可收紧 + 1 条可收缩），且该闸在全仓零引用——ctest、ci.yml、tools/check_ci_scripts.py 均无 file_budget；同时本批把 baseline 向上重写了约 20 处（与「只缩不涨」纪律相反），并新登记 3 个超 200 行阈值的函数条目。当前 HEAD 复跑仍红、仍零接线。

## 证据

- 当前 HEAD 复跑 `python tools/check_file_budget.py --strict` → 退出码 1，输出「NOTE src/ui/paleomainwindow_workbench.cpp [fn:attachWorkbench] 300 行 < 冻结 1091——可收紧」「NOTE baseline 可收缩：src/ui/paleomainwindow_workbench.cpp:file」与末行「file-budget：超标 64 项（file 11 / fn 53），baseline 65，新增 0，回升 0，可收紧 5，可收缩 1」。
- `grep -rn "check_file_budget\|file_budget" CMakeLists.txt .github/workflows/ tools/check_ci_scripts.py` → 只命中脚本自身与 baseline 首行注释，零接线。
- baseline diff（本批 366aef97..8db58138）：src/ui/pages/mappingworkbenchpage.cpp:file 1517→1668，并新增 fn:renderEvidenceSnapshot:267、paleomainwindow_workbench_commands.cpp:fn:runWorkbenchCommand:385、paleomainwindow_workbench_wells.cpp:fn:attachWorkbenchWells:285。

## 影响

#502 点名「无任何防增长闸」后本批做出了闸却没接线——体量可以无声增长；而 baseline 反向上调等于把「只缩不涨」纪律倒过来执行，掩盖真实涨幅。

## 建议

- 给 check_file_budget.py 接 ctest + CI lint 项。
- 拆分 renderEvidenceSnapshot / runWorkbenchCommand / attachWorkbenchWells 三个新超长函数。
- 把 baseline 向下收紧（删 paleomainwindow_workbench.cpp:file 与 fn:attachWorkbench 两条过期行）而非向上重写。

## 验证

`ctest --test-dir build -R file_budget` 存在且 Passed；`python tools/check_file_budget.py --strict` 退出码 0（无可收紧/可收缩 NOTE）；baseline 行数不增。

## 关联

- 来源：#502（视图层规模失衡，建议加单文件/单函数防增长闸）、#503（测试侧膨胀同族口径）。
