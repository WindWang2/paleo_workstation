# 外部工作冲突矩阵（full-audit-20261019）

## 开放 PR

| PR | 分支 | 范围 | 处置 |
|---|---|---|---|
| #568 | codex/paleomap-facies-integration | data 域：古地图微相导入三级相补齐 + 系统纹理（catalog/io/导入链） | EXCLUDED_ACTIVE_WORK：不修改其触及面（以 PR diff 文件清单为准）；审查可覆盖 |

## 开放 Issues（全部 EXCLUDED_EXISTING_ISSUE，不重复修复根因、不关闭、不改写）

| Issue | 主题（特征库摘要） | 涉及面（用于审查排除） |
|---|---|---|
| #553 | GDAL 句柄无 RAII：68 处 throw 路径手工 GDALClose，新增路径漏写即泄漏 | src/io（尤其 segy/gdal 读写族）、src/qgis 栅格写 |
| #554 | 巨型函数三例：766 行 geostat 单体 / SegyReader::open ~490 行 / buildDisplayBar 421 行 | src/algorithms/geostat、src/io/segy、src/ui（displaybar） |
| #555 | LibreOffice 转换无超时：soffice 挂死永久堵死转换队列 | 转换服务（soffice 封装）、BatchJobQueue |
| #556 | 吞错路径：mappingworkflow 产物四级失败仅 qWarning 静默；ErrorHub 采用率 10/135 | src/workflow/mappingworkbench 族、ErrorHub 接线 |
| #557 | 渲染热点：曲线道每帧全量扫描（缺二分定窗）+ 交会图框选全量重绘 | src/ui 测井曲线道、交会图 |
| #558 | 传递性分层债：paleomainwindow.h/workflows.h 扇出根，ui 传递消费 algorithms 符号 | src/ui/paleomainwindow.h、src/workflow/workflows.h、分层闸门 |
| #559 | 测试薄弱区：主窗 attach 域伞式覆盖、ui/pages 32 页仅 1 定向套件、dsp 无测试、73 处裸墙钟断言 | tests/ 布线 |
| #560 | folderimport 复制粘贴三胞胎：sameProject 判定 / 导入作业脚手架 / preview 双胞胎 | src/io/folderimport 族、preview |
| #561 | 低危批：fencewidget 线性扫 / 井表 readAll 峰值 / ShaCache mtime-only / superbuild 无 CI / ASAN 观察档 / io 错误文案语言混用 | src/ui/fencewidget、井表 io、ShaCache、superbuild、src/io 文案 |

## 判定规则

- 与上述 Issue/PR 根因重合 → `EXCLUDED_EXISTING_ISSUE` / `EXCLUDED_ACTIVE_WORK`。
- 审查可覆盖相关代码（记录层面），但不得实施与排除项同根因的修改。
- 修复对排除项有不可避免依赖 → 缩小范围或标记 `BLOCKED_BY_EXTERNAL_WORK`。
- 新发现（无既有 Issue/PR 覆盖）→ 进入本轮修复池。
