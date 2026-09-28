# GOAL Loop 账本 — wave/devex-infra（未入库工作账本；交付记录见 docs/progress/devex.md）

目标：devex 基础设施治理，验收条件为完成判据。
Oracle：干净 build 全绿；ctest -j4×5 全绿；check_layering 与 --strict 皆绿
且注入违例红；5 个旧式 add_paleo_test 兼容；relink 表入 devex.md；
actionlint 绿；自 review 干净；推送+PR。

| 轮 | 改动 | 验证 | 判定 | 下一步 |
|---|---|---|---|---|
| 1 | worktree+Ninja 配置（默认 generator 是 make，需 -G Ninja） | ninja paleo 379/379、ctest -N=82 | 通过 | 读文件+量基线 |
| 2 | checker v2（argv/strict/词表外置/selftest 25 夹具） | selftest 绿、strict 绿、未知参 exit2、注入违例红 | 通过 | CMake 契约 |
| 3 | add_paleo_test(LIBS)+XDG 沙箱+LABELS+onnx APPEND 修复+最小链接集 60 测试 | ninja 679 全绿、ctest -j4 83/83 | 通过 | 5×复跑 |
| 4 | 5×ctest：run4/5 出现 tst_correlation_full 偶发红 | 定位墙钟断言(tst_correlation_full.cpp:2520) | 未通过 | RUN_SERIAL |
| 5 | RUN_SERIAL + tidy 工具链验证（db 过滤 GCC flag、-p 目录名契约） | 5×ctest 全绿；tidy 注入改动真实执行 | 通过 | relink 量测 |
| 6 | relink 面改前/改后实测（78→37/45/52/60/72/74）；e2e 脚手架（extra-*.cmake→旧式注册→构建→ctest 绿→清场） | 数据入 devex.md | 通过 | ASAN/unity |
| 7 | ASAN+UBSAN 轮（534 目标，80/83）：零内存错误；3 败均为预算类断言在 sanitizer 开销下预期红；UBSAN 仅 Qt 内部 QPointer downcast×3 | LastTest.log 归因 | 通过（如实记录） | unity 结论 |
| 8 | unity 轮：ON 编译不过（全仓同名匿名 helper 合批撞），保留开关+如实注释+递延 | build-unity 实测 | 通过（实验结论） | 从零终验 |
| 9 | 从零 rm -rf build + cmake + ninja + ctest -j4 ×5 | 534 目标全绿；83/83 ×5 连续 | 通过 | 提交推送 |
| 10 | 5 原子提交（build×3/test×1/ci×1）+ push + PR #53；提交态复验（ctest 绿/strict 绿/actionlint 绿） | 全 Oracle 满足 | **完成** | — |

**Oracle 终验证据（2026-09-29）**：
- 干净 build：`rm -rf build && cmake -G Ninja && cmake --build` 534/534 → BUILD_SETTLED
- `ctest -j4` ×5：`100% tests passed out of 83` ×5（另在 settled build 上 ×5）
- `check_layering.py`/`--strict` 绿；注入违例 rc=1、清理后绿
- 5 旧式调用构建通过 + extra-*.cmake 路径 e2e（scratchmod）
- relink 表：docs/progress/devex.md（78→37/45/52/60/72/74）
- actionlint 1.7.7：零告警
- PR：https://github.com/WindWang2/paleo_workstation/pull/53
