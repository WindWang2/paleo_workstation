## 问题

lint 车道红于 `src/workflow/mappingworkbench_impact.cpp:430`，跟进修复 PR 亦失败；master @ ddd1325e 该车道为红。

## 证据

run 38835970978（lint job）：

```
check-tidy：base 66e79e616d563452f43dae9f752123ba799d9b25 -> 66e79e616d56
check-tidy：17 个 TU
FAIL src/workflow/mappingworkbench_impact.cpp
  src/workflow/mappingworkbench_impact.cpp:430:20: error: local copy 'child' of the variable 'polygonChild' is never modified; consider avoiding the copy [performance-unnecessary-copy-initialization,-warnings-as-errors]
clang-tidy 门禁红：修掉或（证明误报时）在 tools/.clang-tidy 收窄选题。
```

跟进 PR「fix(mapping): 修复 clang-tidy performance-unnecessary-copy-initialization」run 38866361582 结论仍为 `failure`（需确认是否修错位置或引入新告警）。

## 影响

lint 是唯一在合入前跑静态检查的车道；它红了会掩盖同批其他 16 个 TU 的检查结果，并让本批 A 的红灯面再加一条。

## 建议

- 修 `mappingworkbench_impact.cpp:430`（`child` 改 const 引用或消除副本）；若判定为误报，在 `tools/.clang-tidy` 收窄该 check 并注明理由（项目既定口径）。
- 复核跟进 PR 38866361582 为何仍红。
- 建议 lint 在 PR 阶段即为 required，避免「合入后才发现红」。
- 顺带对账 #484 记录的「lint 跑了 --strict 没跑 --selftest」。

## 验证

lint job 绿（`python3 tools/check_tidy.py --base <base>` 通过）。

## 关联

父项：本批 A；相关：#484
