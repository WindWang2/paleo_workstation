# 时深口径确认与 Petrel 兜底拒收（2026-11-01）

## 背景（HY 验收 2026-10-31 修复验证）

验收工程连井剖面报「HZ19-1-1A 缺少时深关系」，排查结论（三层）：

1. **数据在、解析对、口径未确认**：`D:\HY测试数据\时深转换\03_整理\Merge_TDR`
   （Petrel checkshots，107 井 / 103,912 点）已由工区数据服务
   `parseTimeDepthFile` 正确解析入库（`well_objects` 119 个 time_depth），
   但文件头未声明 `# Unit in time` / `# Time reference`，且时间按海拔带符号
   （基准面以上正、以下负——HZ19-1-1A 184 行全负）。按服务宪法「不猜」，
   该来源以 review 状态留库（timeUnit/timeReference 空、原符号保留）。
2. **主程序消费门正确拒绝但上报失真**：`tdTableFor` 的服务路径元数据门
   （单位/单双程/基准已确认才交付）拒绝了该表，但「尚未确认」诊断在
   装配层被丢弃，画布只报「缺少时深关系」——把「有表未确认」误报成
   「没有表」。
3. **兼容兜底错位吞行**：固定列序解析器（TIME/TVDSS/TVD/MD）对 Petrel
   格式零识别，unbound 路径会把 X 坐标当时间、TWT 当 MD 产出伪表
   （本次验收走了服务路径未踩到，同类风险在）。

## 分工裁决（dataservice vs 主程序）

- **解析**：已在 dataservice（`src/parsers/time_depth_parser.cpp`），不在
  主程序再写一份；主程序固定列序解析器保持「兼容兜底」定位。
- **口径确认**：宿主确认、服务存证（先例 `saveCurveUnit`、
  `ParseOptions.comprehensiveWellMetreDepth`）。确认按**来源粒度**生效
  （一份导出一份口径，107 井一次到位），原始点值不动，读取按确认口径交付。

## 变更

### 主程序（codex/td-petrel-confirm）

- `src/io/wellfileparsers.cpp`：Petrel checkshots 头嗅探 → 整表拒收 +
  单条诊断（不再逐行噪声）；`src/io/wellfileread.cpp`：`readTimeDepth`
  空表/拒收以 error 出口（不再「成功但零行」）。
- `src/domain/wellsection.h`：`Well::timeDepthStatus`（缺时深原因注记）；
  `src/workflow/wellsectionworkflow.cpp`：两条装配路径（workbench/主表）
  均填注记 + 警告，井间地震缝把「缺少时深关系」细化为
  「%1 时深不可用：%2」（真未挂链维持原句）。
- `src/services/projectdata_timedepth.cpp`：
  `ProjectDataFacade::confirmTimeDepthSemantics`（解析路径与 `tdTableFor`
  同源：资产→openSource→模型井→来源条目→服务确认）。

### Data_service（codex/td-semantics-confirm，submodule）

- `DataService::confirmTimeDepthSemantics(tdId, unit, type, reference,
  negateTimes)`：校验（ms/s、TWT/OWT、基准非空）→ 整来源 payload 更新
  （timeUnit/timeType/timeReference/timeSign）→ changes 台账 → 同源
  TIME_UNIT_UNKNOWN / TIME_REFERENCE_UNKNOWN / TIME_SIGN_REVIEW 复核问题
  联动消解。`readTimeDepth` 对 `timeSign=="negated"` 按标准域交付
  （基准面以下为正）；数据库原值永不改动。

## 验收视角的最短操作路径

数据不重导：对 Merge_TDR 来源执行一次确认（ms / TWT / MSL / 符号取反），
重开连井剖面即通；地震体侧 `timeReference` 需同串登记为 MSL
（`saveVolume`，未对齐时缝隙如实报「时间基准未确认一致」）。

## 递延（TODOS.md 已登记）

- 确认入口的 UI 接线（井震标定工作台/数据页复核队列）；
- 地震体基准与井时深基准的成对确认向导（含 DT_ALL 同族来源）。
