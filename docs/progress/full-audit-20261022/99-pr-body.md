## Summary

全仓 review-fix-review-fix 质量整顿（第 6 轮，full-audit-20261022）：以
#607 收口后新合入的 20 个 PR（#603/#608–#626，+15.8k 行）为主战场的全仓深审，
三轮独立闭环（Deep Review → Fix 1 → Independent Review 2 → Fix 2 → 独立终审
→ 回归验收）。17 项 Finding 修复 + 4 项复审收口 + 1 项终审 P1 收口，零 P0/P1
遗留，零新增回归。

## Baseline

- BASE_SHA `b301f2e8`（origin/master 2026-10-22）；终 HEAD 见本 PR（11 commits）。
- CachyOS Linux 6.18.55 · gcc 16.2.1 · Qt 6.12.0 · vendored QGIS 4.2.2
  superbuild · Ninja RelWithDebInfo · `-j4` + flock 全局锁。
- 基线构建：首跑链接失败为**环境事故**（共享 prefix 的 libarrow/parquet 五个
  悬空符号链接指向已删除的 appimage 暂存；从 pacman 缓存 arrow-25.0.1-7
  提取真实库修复，非本仓代码问题，00/09 文档留档）。
- 纯净基线 ctest：**407/410（99%）**，3 红在册（tst_onlinebasemap[PR#627 面]/
  tst_datapreview/tst_assetpaging）。

## Review Coverage

- 5 份只读深审报告（03-review-{A..E}.md，docs/progress/full-audit-20261022/）：
  增量窗 247 文件全部纳入（逐行/调用链 vs 接口契约分层），全仓其余模块
  复覆盖 + 未核查区如实申报（02-coverage.md）。
- 主代理交叉复核（importrecovery/welllogread 接缝、#617 手势、#625 底图、
  基线红归属）+ 1 项代理发现证伪（B-06，neighbourhood 严格比较核实）。

## Findings

- 新立案 **22 项（去重后）全部 P3，零 P0/P1/P2**：上轮收口后 20 PR 的增量面
  在本轮深审下无崩溃/数据损坏/数值错误级问题。
- 排除矩阵：16 开放 Issues（#556–#597）+ PR #627/#628 文件面零重叠。

## Fix 1（fc7e2b08…80bc61ac，7 commits）

1. **深度单位诚实性**（C-7/B-02/B-03）：predictionLog 全格式米制归一（英尺
   LAS 验证页错轴）、mdAxisInMetres 未知单位拒收（DM 井 10 倍错位）、
   petrophys 解析失败真因透传。
2. **文本曲线生态**（A-02/A-03/A-06）：GB18030 中文井名/表头可读（工区一半
   真实文件此前被拒）、井深/分号词表三面对齐、超宽单行 256 列早退。
3. **批量让位交互**（B-01）：BatchQueue Low 优先级 + JobRunner 优先级参数 +
   3/4 参 start 落实默认 Normal——批量期间交互取数不再被整批压住分钟级。
4. **预览栅格失败通道**（B-05≡D-03）：request* 四路失败/取消也回调；TOC
   min/max 清值 + 「统计不可用」、统计页不留「计算中…」、直方图不套假拉伸。
5. **导入反馈**（A-01/A-08）：单模糊井名文案区分、迟到 Skipped 行序契约。
6. **道数据源身份**（D-01）：FormationTrack::FormationSource 持久化身份路由
   ——改名层序道不再被分层区间静默覆盖（图面数据混淆）；随批 E-01/02/03
   （vendor P14 补登、基线计数对齐、缩进复位）。
7. **双嗅探消重**（C-8）：parse 带 format 重载，六调用点 + 缓存路径收敛
   （每文件每轮省 2-3 次 open + 64KB 头读——IO 次数级，不做无实测墙钟宣称）。

## Review 2（06-independent-review-r2.md）

独立复审（非作者）：**MUST_FIX 1 / SHOULD_FIX 4 / DOCUMENT_ONLY 10 /
FALSE_POSITIVE 0**；B-06 维持证伪；无需回滚。MUST_FIX = C-8 重构曾丢
`!path.isEmpty()` 守卫（无测井井 mock 深度回退被破坏）。

## Fix 2（75f3a54e）

R2-M1 守卫回修 + 无测井井 mock 深度回归用例；R2-S2 剖面空回调不留图例项 +
失败测试扩四路；R2-S3 readCurveTvd 词表对齐（B-02 残留收口）；R2-S4 spread
基线头注；R2-S1 按裁定改判结转（登记册注明）。

## Final Review（08-final-independent-review.md）

独立终审（从 HEAD 重新出发，未采信前序结论）：**PASS-WITH-NOTES**——
17+4 项修复全部确认（含六条构造性挑战：DLIS FT 归一、0.1INCH 拒收上屏、
会话切换无饥饿、旧 sidecar 等价、#618 未触碰、无空解引用）；唯一合并前置
条件 N1（file_budget 回升 2 项，P1）已按其建议收口（485045a5：共享助手
代码收缩，buildMapProductContent 373→372 基线随缩下调，非补登）。
N2（P3 services 侧双嗅探存量）/N3/N4 处置见 08 附记。

## Testing

- 纯净基线（b301f2e8）全量 `ctest -j4 -LE 'core|perf'`：407/410，3 红在册。
- **终局全量（终 HEAD）同口径复跑：407/410，红集合与基线完全一致——零新增回归。**
- 新增/扩展回归 **15 个用例**，复审逐个红绿判定为真（06 §3、08 §2.8）。
- 源码闸门 13 项复跑全绿（layering/qgs-audit/qgis-spread 双 strict、
  file_budget 双 strict、ui_invariants、i18n、ui_tokens、qgis_versions、
  docs_drift）；`git diff --check` 干净。
- 运行口径注记：本机裸 ctest 需 `LD_LIBRARY_PATH=<superbuild prefix>/lib`
  （与 ./paleo-dev 的注入同理），首跑 201 项瞬时红为口径错误已纠偏留档。

## Performance

- C-8：IO 次数级收敛（每读取点嗅探 2→1；wellpredictionservice per-ref→
  per-path），按纪律不做无实测的墙钟宣称。
- B-01/A-06：设计语义恢复（让位闸可达）与预算早退，同上不宣称数字。

## Conflict Avoidance

- 16 开放 Issues（#556–#597）根因面零重叠（01-conflict-matrix）。
- PR #627 文件（onlinebasemap/previewmapcanvas/qgiscanvascontroller）未触碰。
- 交付前新增 PR #628 共享 3 文件，`git merge-tree` 零冲突标记、hunk 不相交。
- 结转项（A-04/A-05/B-04/D-02/N2/C-9 测试债）在 04/05/08 登记册注明归属。

## Known Limitations

- 3 项基线在册红未修（非本轮引入）：tst_datapreview 岩屑岩性道断言、
  tst_assetpaging 批量改类型断言（#621 后续面）、tst_onlinebasemap（#627 面）；
  known-reds.json 未含前两条，随基线红清账批次处理。
- C-9 的 propertymodelworkflow/sectionworkbench 拒收路径测试债、N2 services
  双嗅探存量、三个深度换算函数三体收敛——登记下一批次。
- 环境事故（悬空 arrow 链接）已修复共享 prefix；若其他 worktree 再现链接丢失，
  按 09 台账 Round 0 的修复记录处置。

审计档案全量：`docs/progress/full-audit-20261022/`（00-09 + 99）。
