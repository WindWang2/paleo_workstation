# P3 数据操作重构——递延项与接缝登记（GAPS）

日期：2026-09-29。分支：`wave/data-page-operations`。
本文件登记**确属服务层/壳层缺口**、本波以视图层组合或 UI 就绪态交付的
事项。每条含：缺口、本波替代实现、正路（需要什么 API/接线）。

## 1. catalog 写 API 缺口（数据层）

### G-1.1 实体更新 API（updateEntity）
- **缺口**：`DataCatalog` 无实体字段更新接口（只有 `addEntity`）。
- **本波**：`EntityOverrideStore`（`.paleo/entity_overrides.json`）——
  改名/坐标/备注写视图层 sidecar，树/表/面板/拓扑全链显示生效，
  可撤销。**注意：catalog.json 里的原始 name 不变**——经 catalog 直接
  读名的其它页面（预测/编图等）看不到改名。
- **正路**：`DataCatalog::updateEntity(id, fields)`；届时
  `EntityEditCmd` 换单行调用，override 数据一次性迁移。

### G-1.2 实体删除 API（removeEntity）
- **本波**：删除 = sidecar 软删（`RecycleBin`，可恢复）+ 可选逐链接解挂。
  catalog 记录留存。
- **正路**：`removeEntity`（带链接处置语义）。

### G-1.3 资产删除 API（removeAsset）
- **本波**：同上软删（`batchRemoveSoft` → 回收清单）。
- **正路**：`removeAsset`（或带 purge 的两段式）。

### G-1.4 资产类型更新 API
- **本波**：`AssetOverrideStore`（`.paleo/asset_overrides.json`）——
  过滤/显示口径生效；catalog 原型不动。
- **正路**：`updateAssetType`（分类器词表校验随行）。

### G-1.5 链接更新/删除 API（updateLink/removeLink）
- **缺口**：角色变更 = `addLink(新角色)` 后旧链接**无法删除**——
  `RoleEditDialog` 明示「旧角色关联保留 + 不可撤销」（D5.5 正用例）。
- **本波**：转移挂接经 `setLinkUnresolved + attachLink` 同行复用实现
  （完全可逆）；角色变更是唯一不可撤销写操作。
- **正路**：`updateLinkRole` / `removeLink`。

### G-1.6 资产/链接标签 API
- **本波**：`TagStore`（`.paleo/asset_tags.json`）。查询页/其它面板
  不可见。
- **正路**：catalog extra 或独立标签表。

## 2. 壳层接线缺口（src/app/paleomainwindow，本波领地外）

### G-2.1 状态栏反馈接线
- `DataPage::statusMessage` / `DataListPanel::statusMessage` /
  `EntityPanel::statusMessage` 已发——壳需接 `statusBar()->showMessage`。

### G-2.2 外部导入接线
- `DataPage::externalImportRequested(paths)` 已发——壳需接到
  FolderImportWorkflow（复用 T22 确认表）。

### G-2.3 导入队列执行器注入
- `ImportQueuePanel::setRunner` 是注入钩子；生产应由壳把
  `FolderImportWorkflow` 的单行导入适配成 runner（逐文件 markProgress/
  markDone/markFailed）——队列 UI/状态机/重试已就绪并测过。
- 未注入时：队列条目停「排队」态（不误报）。

### G-2.4 预览区/标签栏拖放宿主（D3.3）
- 拖源 + mime（`application/x-paleo-asset-ids`）+ `assetDragStarted`
  信号已交付；datapreview（领地外）需加 drop 接收（→ openAsset）。
- 壳可用 `DataNavTree::isAssetMime` 判定。

### G-2.5 全局 Ctrl+K 归属
- DESIGN.md 规划全局搜索走 QgsLocator（壳）；本波命令面板在数据页内
  （页面级 QShortcut）。全局化时把 DataCommandPalette 源注册进壳。

### G-2.6 实体改名联动其它页
- 井名 override 生效于数据页全链 + 命令面板；预测/编图页经 catalog
  原名读——需 G-1.1 落地后自然收口。

## 3. 构建接线说明（非缺口，备查）

- 新增源文件全部为**头文件**：纯逻辑在 `src/ui/pages/dataops/*.h`；
  含 Q_OBJECT 部件平铺 `src/ui/pages/*.h`。CMake AUTOMOC 只对同名
  basename 头自动 moc——部件头经各 TU 尾部
  `#include "moc_<basename>.cpp"` 显式编入（datalist.cpp 持 6 个、
  entitypanel.cpp 持 panelextra、datapage.cpp 持 palette）。
  **新增部件头时须在且仅在一个 TU 尾部加 moc include**。
  这是「文件领地不含 CMakeLists.txt」约束下的零登记方案。

## 4. Qt 平台接缝（备查）

- 合成 QDropEvent/DragMove 被 `QApplication::notify` 吞（无 QDrag
  会话不路由）——测试经 `DataNavTree::handleDrop/handleDragMove/
  handleDragLeave` 直调；真实用户拖放不受影响。
- offscreen 平台 `propagateSizeHints` 警告为平台噪音（队列面板测试
  中出现过一次，无害）。
