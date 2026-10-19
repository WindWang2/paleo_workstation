# 基线质量分账（full-audit-20261020）

## 构建环境事件（ENVIRONMENT_FAILURE，非代码回归）

1. **Arrow 25/26 ABI 断链**：共享 vendor prefix 的 `libgdal.so.39`（10-01 构建）
   DT_NEEDED `libarrow.so.2500`，系统 Arrow 已升级 26（`.so.2600`），prefix 内无
   Arrow——任何链接 libgdal 的目标 `undefined reference to arrow::*`。
   处置：主仓 build/appimage 的 AppDir 内有完整 Arrow 25 闭包（只读复用），
   `CMAKE_EXE_LINKER_FLAGS` 注入 `-Wl,-rpath-link,<AppDir>/usr/lib`；测试运行
   `LD_LIBRARY_PATH=<AppDir>/usr/lib:<prefix>/lib` 前置。未触碰共享 prefix
   （BUILDING.md「不要在共享 prefix 中清理或重建」纪律）。
2. **-j4 链接期 ld 段错误**（信号 11，链接 tst_background_open 附近）——资源事件，
   降 -j2 恢复。
3. **libpaleo_qgis.a 对象损坏**：gold 诊断 `projectmapreference.cpp.o could not
   decompress section .debug_info` / `topologicalindex.cpp.o 不支持的重定位`——
   ccache 缓存或并发写损坏；GNU ld 表现为 `reloc against .debug_str: error 4` /
   `final link failed: bad value`。处置：删除 `CMakeFiles/paleo_qgis.dir` +
   `libpaleo_qgis.a` 干净重编后链接通过。若其他静态库出现同族错误同法处理。

## 基线口径说明

干净 master 基线窗口被上述环境事件消耗（三次构建中断）。处置：本工作树的
「基线」= master + Fix-1 源码（构建期间逐步落地）的全量 ctest；既有测试的红绿
对比以此为准，master 侧既有红按 known-reds.json + 抽样 stash 复跑（源文件回退
master、仅保留新测试）确认「修复前红」证据。上轮（#580）终局口径 437/440
（3 红在册：layering_transitive #533 挂起、tst_ui legendTitle 基线红、
tst_assetpaging 基线 SEGFAULT）作参照。

## 全量 ctest 结果（终局，Fix-1 + Fix-2 后）

- **426/440 通过（97%），14 红全部为 master 既有/在册/负载抖动，零本轮新增回归**。
- 环境要点：`LD_LIBRARY_PATH` 必须 **prefix 在前、AppDir 在后**（AppDir 自带全套
  QGIS/GDAL——前置会遮蔽共享 prefix 的已打补丁 QGIS，tst_labelzorder 曾因此假红）；
  QGIS_PREFIX_PATH 需指向 vendor prefix。
- 14 红分账：
  | 红 | 归属 |
  |---|---|
  | layering_qgs_audit / qgis_spread(_strict) / ui_size(_strict) / file_budget(_strict) / ui_tokens(_strict) / docs_drift（10 项） | master 既有 G1 簇（PR #580 分支已修未合并——G1-01/02/03/04/05 + 主窗 TU 26→27；本分支不重复修，属 EXCLUDED_ACTIVE_WORK） |
  | layering_transitive | #533 裁决挂起在册（ui_qtwidgets_notes 7>6） |
  | tst_ui（legendTitle） | 上轮 stash 法证实在册基线红 |
  | tst_assetpaging（SEGFAULT） | 在册基线红 |
  | tst_catalog（sqliteOpenCostAttribution） | 负载下墙钟抖动（roOpen 24ms == reopen 24ms 边界；串行/直跑复跑两连绿；#559 裸墙钟断言家族） |

## 修复前红/修复后绿抽样实证（5 簇）

| 簇 | master 版 | 修复版 |
|---|---|---|
| A-10 zip | 2 新例红（回绕穿闸读出垃圾「签名不符」） | 11/11 绿 |
| B-04 SATR | 谓词语义红（未取消→「已取消」整读失败） | 9/9 绿 |
| B-05 交会预算 | 无闸路径 33.3s 无界分配后断言红 | 7ms 拒绝绿 |
| D-13 换勾建议 | 旧因素 1/6 泄漏覆写用户值（红） | 绿 |
| C-10 回收防线 | 软删资产重导入静默放行（红） | 拒绝+declareMaps 过滤绿 |

其余簇（D-05/D-07/D-10/D-11/D-12/A-11/A-13/A-14/C-11/C-14/C-15/B-10/B-11/E-10..E-13）
以新增回归全绿 + R2 复审静态确认（E-10 六夹具对旧代码必红、A-11/A-13 夹具推演）
承载证据。
