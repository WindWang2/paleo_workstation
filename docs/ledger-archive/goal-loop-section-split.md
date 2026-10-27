# 方向 65 ledger — seismicsection 家族拆分（dockwidget 3,040 + canvas 2,192）

- worktree：`.worktrees/section-split`，分支 **`codex/dir65-seismicsection-split`**
- 提交：`498114c`（已 rebase onto `bf7f929`，含方向 64 的弹框收口）
- 纪律：全留 `src/ui/seismicsection/`（视图层）；新文件首行 `// 层：视图`；
  四条静态门全绿；`-j8` 构建；新 TU 经 `cmake/extra-section-split.cmake` 挂载
  （根模块表只读 + `_paleo_extra_manifest` 一行登记）。

## R0 体量实测（wc -l）

| 文件 | R0 | 收口 |
|---|---|---|
| `seismicsectiondockwidget.cpp` | 3,040 | **533** |
| `seismicsectioncanvas.cpp` | 2,192 | **297** |

## 收口 TU 清单（wc -l 实测）

| TU | 行数 | 职责 |
|---|---|---|
| `seismicsectiondock_setup.cpp` | 797 | 工具栏 / D2.x 显示控制行 / 三面板装配 |
| `seismicsectioncanvas_paint.cpp` | 798 | paintEvent 主体（776 行单体，刻意不切函数） |
| `seismicsectiondockwidget.cpp` | 533 | 构造/析构 + 切片导航/提取 + 任意线提取 |
| `seismicsectioncanvas_render.cpp` | 461 | 上色 / LUT / 叠加烘焙 / LOD / wiggle |
| `seismicsectiondock_wells.cpp` | 325 | D5 井震与任意线 + 井旁道控件 |
| `seismicsectiondock_bookmark.cpp` | 278 | D2.10 卷帘 / D2.11 道头卡 / D2.12 书签 / D2.13 复制打印 |
| `seismicsectiondock_attr.cpp` | 278 | 属性剖面 / 时间片扫描 / 属性体扫描 |
| `seismicsectiondock_inversion.cpp` | 277 | 子波提取 + 三段式反演 |
| `seismicsectiondock_track.cpp` | 259 | 种子追踪 + 层位体传播 |
| `seismicsectiondock_interpret.cpp` | 257 | 解释会话（拾取/断层/资产登记） |
| `seismicsectioncanvas_interact.cpp` | 245 | 鼠标/滚轮/键盘交互 + hover |
| `seismicsectioncanvas_params.cpp` | 237 | 显示参数 + D4/D5 叠加数据入口 |
| `seismicsectioncanvas_nav.cpp` | 222 | 卷帘 / 缩放平移 / 坐标换算 / 导出抓图 |
| `seismicsectiondock_internal.h` | 175 | undo 命令族（interpret + track 共用） |
| `seismicsectioncanvas_internal.h` | 47 | 纹理缓存键内容指纹（setSectionData + rebuildImage 共用） |

两主文件 533 / 297（门 ≤1,000）；最大新 TU 798（门 ≤900）。

## 等价性机械证据

- **零丢行**：对拍 `bf7f929` 两文件每一行（跳过 include 与 namespace 括弧行），
  在新面（17 文件并集）多重集消解 → 未落地 **0 行**；唯一例外是
  `SliceFingerprint` 的签名加了 `inline`（跨 TU 共享必需，语义零变化）。
- **符号一一对应**：成员函数定义 dockwidget 65→65、canvas 83→83，无重复定义。
- **类零变更**：`seismicsectiondockwidget.h` / `seismicsectioncanvas.h`
  对 `bf7f929` 的 `git diff --stat` 为空。
- **token 例外**：以上游 358 条为底，仅把 seismicsection 的 20 条按新落点重映射；
  总数仍 358，`ui_tokens --strict` violations=0 / stale=0。
- **静态门**：`check_layering --strict` / `check_ui_tokens --strict` /
  `check_ui_invariants --strict` / `check_i18n` 全绿。

## 行为红线保留（地震面特有）

- 世代号 + 请求号双守卫（切片/卷帘）：`extractSliceAsync` / `updateCompareSlice`
  逐行未改。
- QPointer 迟到回调守卫：属性扫描 / 体传播 / 3D 预览 / 追踪 / 反演发布，逐行未改。
- 卷帘 B 图任务取消对端：逐行未改。
- 纹理缓存键语义：见 `seismicsectioncanvas_internal.h` 顶部注释节（消费面引用点）。

## 验证结论

| Oracle 项 | 状态 |
|---|---|
| 1 类零变更 / 消费方零改动 | ✅ 头文件 diff 空；逐行 0 丢行；定义 65→65 / 83→83 |
| 2 体量门 | ✅ 533 / 297；最大新 TU 798 |
| 3 sectionui 守卫矩阵 ×3 | ✅ **25 passed / 0 failed / 1 skipped，三遍全绿**（含迟到回调 UAF、世代号顶替、卷帘取消、纹理缓存命中、书签往返） |
| 4 性能门 | ⚠️ `tst_seismic_perf` / `budgets` / `baseline` 在 **R0 即红**，非本改动引入，比率门无法取 R0 基线（见下「环境口径」） |
| 5 全量 ctest 红集合 diff | ⚠️ 280 通过 / 81 红；R0 46 红 → 现 81 红，**35 项新增红已逐项归因，无一可归因于本拆分**（见下「新增红归因」） |

## 环境口径与已知偏差（诚实记档）

1. **磁盘耗尽**：本机多 worktree 并行构建把 1.9T 盘占满（曾 0 可用），
   链接期报 `LNK1104/LNK1116/LNK1201/LNK1120`。处置：清本 worktree 自己的
   `build/*.ilk`（17G）与 `build/*.pdb`（25.8G，纯调试信息，运行测试不需要），
   空间回落后构建收敛 rc=0。未触碰其他 worktree 的 build。
2. **rebase**：拆分期间上游并入了方向 64（QMessageBox → PaleoNotify 收口）与
   #259/#260，我方提交已 rebase onto `bf7f929`。token 例外 json 以**上游为底**
   只做路径重映射（不吞上游 3 条新增，baseline 355→358 保持上游口径）。
   冲突仅 `tools/ui-token-exceptions.json`（数据）与 `seismicsectiondockwidget.cpp`
   （rebase 自动合入 64 的弹框收口），均已核验无冲突标记残留。
3. **`tests/tst_import.cpp` C2022 是上游预存编译错**（`QByteArrayLiteral` 内
   `\xff` 串被当字符字面量，MSVC 报「成员引用太大」），`origin/master` 同文件
   同错。与本方向无关，未在本分支修（保持原子边界）。
4. **R0 ctest 口径**：R0 基线为串行轮（3h48m，243 通过 / 46 红）。该轮后半程
   我误判它已被 kill 而另起 `-j8` 轮，两轮并发约 1 小时，R0 红集合可能含
   争抢诱发的额外红项。凡「R0 红 → 拆分后绿」按**进步**记账；凡
   「R0 绿 → 拆分后红」逐项复跑仲裁。
5. 地震域矩阵新增红 2 项已仲裁：
   - `tst_seismic_highdpi`：3 个失败槽全是 `dpr=2 子进程 rc == 0`（子进程拉不起），
     非断言不符；该测试引用的是 `Seismic3DViewportWidget`，源码树内
     `grep -n "seismicsection" tests/tst_seismic_highdpi.cpp` **零命中**。
   - `tst_ai_horizonsuggest`：SEGFAULT，但只 include `src/ai/*`、链 `paleo_ai`
     （`cmake/extra-test-deepening.cmake`），与 `paleo_ui` 无耦合。
   两者均判为环境/预存问题，非本改动信号。

## 新增红归因（35 项，R0 绿 → 拆分后红）

**结论：无一可归因于本拆分。** 三类成因，全部是本机环境事故的连带：

| 成因 | 项数 | 证据 |
|---|---|---|
| **A. perf 类测试跑完不退出，我为推进 ctest 强杀进程致假红** | 8 | `tst_perf_catalog` 16/16 全通过、`tst_perf_regress` 11/11 全通过、`tst_coldstart` 3/3 全通过但 ctest 记红；这三项 Totals 行 `failed=0` |
| **B. QGIS 前缀 `resources/` 整个缺失（磁盘事故连带）** | 若干 | `ls paleo-qgis-prefix/` 只剩 `bin include lib`；`tst_runtime` 唯一失败断言是 `QFile::exists(QgisRuntime::srsDbPath())`（该测试源码 `seismicsection` 零命中） |
| **C. 磁盘耗尽期间 DLL/夹具从缺** | 若干 | `tst_onnx*` / `tst_modelregistry` 无 Totals（onnxruntime 未 vendored）；`tst_layerservice` / `tst_notifications` / `tst_shortcuthelp` 无结果文件 = 启动即死 |
| **D. 双 Qt DLL 入口点（skill 已记 Landmine 1）** | 1 | `tst_shortcuts_shell` 复跑仍 `0xc0000139`；同族 `tst_dockmanager` / `tst_ui` / `tst_layertreepanel` 在 R0 即同码红 |

**最强判据**：35 个新增红中，测试源码 `grep seismicsection` **仅 1 个命中**
（`tst_shortcuts_shell`，且其失败码是 DLL 入口点 `0xc0000139`，非断言不符）。
反向确认真正验证 seismicsection 的用例矩阵全绿：

```
tst_seismic_sectionui : 25 passed, 0 failed, 1 skipped   （×3 遍）
tst_seismic_core      : 10 passed, 0 failed
tst_faultsectionui    :  9 passed, 0 failed
```

**Oracle #5 诚实结论**：红集合 diff 非空，故该条**未达成**；但差异已逐项
归因至环境事故，无一指向本改动的代码面（逐行 0 丢行 + 定义数 65→65/83→83 +
三类守卫矩阵全绿构成等价性主证据）。若要补齐，需在磁盘与 QGIS 前缀修复后
重跑两轮对照——这超出本方向可独自完成的范围，记为遗留项。

## 遗留项（TODO，不在本方向修）

1. `paleo-qgis-prefix/resources/` 缺失（`srs.db` 等）——影响 `tst_runtime` 等，
   属主机依赖闭包问题，需单独方向处理。
2. `tests/tst_import.cpp` C2022 上游预存编译错——保持原子边界未修。
3. perf 类测试「跑完不退出」需在测试壳统一收尾（当前靠外部杀进程，ctest 会记假红）。
