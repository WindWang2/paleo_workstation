# Full Audit 2026-10-23 — 00 基线

## 基本身份

- BASE_SHA：`5abc061a073aa244e7719a52b0ea68deed5b632e`（origin/master，#647）
- 上一轮收口：`d699694c`（#629，full-audit-20261022）
- 工作分支：`codex/full-review-fix-review-fix-20261023`
- Worktree：`/home/kevin/projects/paleo_workstation-full-review-20261023`
- 主 checkout 未提交改动未触碰（`.zcode/`、`docs/progress/review-from-zero-20261019.md`）

## 增量窗

`d699694c..5abc061a`：11 个提交，212 文件，+8376/−1037。

- #630 新建工程引导、资料与现有古地理图导入
- #631 所有数据导入按钮打开来源对话框
- #632 便携工程离线底图后台导入
- #633 导入生成岩性道与完整分层，勾选图件绘制顺序
- #634 后台 worker 退出后保留 native CRS
- #635 便携包坐标库绑定、中文安装路径与启动自检
- #636 OSGeo4W 钉版对齐 freetype 2.13.3-5
- #637 统一工区坐标与井层单因素导入，补齐相分类，简化成图操作
- #643 角色双表补登 reference + 一致性闸门；测试临时目录清理（#642/#641）
- #644 简化历史源图，推迟详细要素证据
- #647 简化常用界面，修复草稿、定位和撤销

热点：`src/io` 35、`tests` 28、`src/ui/pages` 24、`src/workflow` 15、
`src/qgis` 11、`src/services` 10、`src/ui/dialogs` 8、`src/catalog` 5。

## 平台

- 共享 QGIS_PREFIX：`/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix`（symlink，只读复用）
- 构建：本 worktree 独立 `build/`，RelWithDebInfo，Ninja，`--parallel 4`，锁 `/tmp/paleo_workstation-build.lock`

## 已知 CI 红（#644 run 39869529876，sha f3998bd4；#647 run 39936388892 仍在跑）

lint：`mappingworkbenchpage.h` 与 `mappingworkbenchpage_evidence.cpp` 的
`buildEvidenceSection` / `buildEvidenceEditor` 参数名 `group` vs `parent`
（clang-tidy `readability-inconsistent-declaration-parameter-name`，warnings-as-errors）。
HEAD 上仍不一致。

linux 失败套件（qtest-first-run）：tst_assetpaging、tst_composeattribution、
tst_datapreview、tst_exportpipeline、tst_impactrecompute(6)、tst_import、
tst_mkprojectfixture、tst_onlinebasemap、tst_panels、tst_projectparsers、
tst_standardelements(2)、tst_wellattributes(3)、tst_wellcomposite。
已由 #645 登记。本轮要区分「测试期望过期」与「产品回归」，不把 #645 的症状再开一张单。
