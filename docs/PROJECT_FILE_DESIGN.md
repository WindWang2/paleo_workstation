# 项目文件设计（PROJECT_FILE_DESIGN）

> 指令：根据真实工区文件夹（`paleo_project/data/project_area`：井位/井曲线/
> 地震体/参考相图/参考资料 + 派生 sidecar）重设项目文件，支撑项目管理并
> 支持导入这种文件夹。

## 问题

此前工程锚点是裸 `.qgz`：新建工程只产一个 `.qgz`，打开收 `.qgz`，
「导入工区文件夹」是开口工程后的独立动作。`.qgz` 只能描述 QGIS 图层集，
管不住 catalog/manifest/gpkg/areaRules/commit_journal 这套工程束
（bundle）——成员缺失时 .qgz 照常开，坏在半截没人知道；也没有「指着
一个工区文件夹直接成工程」的入口。

## 设计：`project.paleo` 工程清单

工程根目录下 `project.paleo` = 工程的唯一入口与束清单（JSON，QSaveFile
原子写）。`.qgz` 退居「成员之一」——它仍然是 QGIS 图层集权威，但不再是
工程入口。

```json
{
  "format": "paleo-project",
  "formatVersion": 1,
  "name": "project_area",
  "projectId": "proj-…",
  "createdUtc": "…",
  "members": {
    "qgz":       "project_area.qgz",
    "catalog":   "artifacts/metadata/catalog.json",
    "manifest":  "project_area.qgz.project.sqlite",
    "gpkg":      "project_area.gpkg",
    "areaRules": "project_area.json"
  },
  "sourceArea": {
    "root": "/path/to/project_area",
    "importedUtc": "…",
    "stats": { "files": 60, "rows": 116, "failed": 0 }
  }
}
```

- **members** 全部是工程根相对路径——束可整体搬迁，清单不失链。
- **catalog/manifest/gpkg/areaRules** 允许缺席（工程早期没建 catalog 属
  正常）：打开时缺失成员如实进 `lastErrors`，不拦打开。
- **qgz** 缺席 = 工程束损坏，打开拒绝——唯一能拦开的成员。
- **sourceArea** 记录「从工区文件夹新建」的来源与导入统计，供追溯；
  手工新建的工程此节为空。

## 打开/新建契约

| 入口 | 行为 |
|---|---|
| 打开 `.paleo` | 读清单 → qgz 成员缺失/坏 → 拒开；其他成员缺失 → 如实报进 lastErrors 仍开 |
| 打开 `.qgz`（旁有 `.paleo`） | 打开 + 校验成员清单，缺失如实报 |
| 打开 `.qgz`（旁无 `.paleo`） | 打开 + **自动收养**：写一份 `project.paleo`（QGZ-only 老工程升级路径），记日志 |
| 新建工程 | 写 `.qgz` + `project.paleo`（name=文件名去后缀，projectId 随机） |
| 从工区文件夹新建 | 选文件夹 → 已有 `.paleo` → 直接打开不重建；已有 `.qgz` → 打开收养 → 否则在夹内写 `<basename>.qgz` + `project.paleo` → 自动进工区导入流程（IngestPlan 确认框），完成后 stats 回填 `sourceArea` |

### 就地工程与扫描守卫

「从工区文件夹新建」产生**源目录==工程根**的就地工程。`buildIngestPlan`
的扫描守卫相应改为：

- 源根 **在工程目录之内**（如 `artifacts/` 子目录）→ 仍拒（issue 如实报）。
- 源根 **==工程根** → 允许扫描，但束成员（`project.paleo` 声明的
  qgz/catalog/manifest/gpkg/areaRules + `project.paleo` 自身）与
  `artifacts/` 受管子树不出行；其余文件照常分类成项。
- 源根 **包住工程目录**（上级目录）→ 工程产物子树整体跳过（旧行为不变）。

`.preview_cache` 等隐藏目录天然不进表（枚举器不带 `Hidden`）。

## 文件契约

`src/metadata/paleoprojectfile.{h,cpp}` —— 纯 Qt JSON，无 QGIS 依赖：
`PaleoProjectFile`（formatVersion/name/projectId/createdUtc/members/
sourceArea）+ `readProjectFile`/`writeProjectFile`/`missingMembers`。
清单损坏或 formatVersion 超本实现 → 打开如实报错。

## 与既有机制的衔接

- `PaleoProjectStore::setProjectPaths` 的 qgz/gpkg/sqlite 约定不变——清单
  只是「指认并校验」这些既有位置，不另起路径体系。
- catalog 仍在 `artifacts/metadata/catalog.json`；`commit_journal/`、
  `.running`、崩溃转储等不重复登记进清单（它们是派生态不是成员）。
- 最近工程列表照存入口路径（`.paleo` 或 `.qgz` 均可——打开端自适应）。

## 非目标

- 不把数据文件清单写进 `project.paleo`（那是 catalog.json 的职责，
  清单只管束成员）。
- 不做多工程并行管理器（一次一个工程，QGIS app 同语义）。
