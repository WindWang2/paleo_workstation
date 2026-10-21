# 新建工程与资料生成（2026-10-22）

新建入口以原生四步向导收集工程名称/位置、井点、其他资料、确认；资料全部可跳过，
支持直接创建空白工程。已有工程仍直接打开；文件菜单提供“工程资料设置”补充导入。

数据链：XLSX / CSV / TSV / TXT / 点矢量识别井名、XY、可选 KB/TD；字段与工作表可调整。
原件保持 RAW，规范井表登记 DERIVED 并保存 parentVersionIds；TVD 与 MD 分开保存。
缺失 KB/TD 用显式缺失标记适配既有 NOT NULL 数据库，不把缺失显示成零。
分层总表与拆分表按完整记录集合去重；旧工程、缓存、空文件默认跳过。

井点与地震覆盖范围沿用 catalog → 受管 GeoJSON → manifest 图层的生产链；面矢量
生成独立 study_area，保留各要素及多部件几何，避免伪造为地震采集测网。
原生投影记录为 georeference.kind=crs，精确绑定 QGIS 转换上下文；保留旧 similarity2d。
明确声明投影的层位文件按实际 XY/线道号建立网格，保留旋转及最大拟合误差；超半像元拒收。

真实资料核对：20 井、516 条分层、21 LAS、1 SEG-Y、8 层位；A18_log.las 内部井名
实际为 Zhuang181，确认页列明冲突并提供“关联 A18 / 保留内部井名”的显式选择。
归属选择保存进工程；执行时按唯一井名解析已建立实体，不改原始 LAS。

井位多工作表默认只选首张可识别表，预览与执行共用同一映射；可切换工作表。
同一分层记录同时提供 MD/TVD 时分别保留，缺失值按既有 wellnumeric 词表识别。
规范井表使用制表符，井名包含空格时仍能正确重开。

验证（2026-10-22，Windows / vendored QGIS）：

- 独立 build/setup/out 完整编译主程序与测试，-j8；未替换日常运行目录。
- tst_projectsetup：15 通过，含真实资料全量入库；350 个文件、导入失败 0；
  20 井、516 条分层、1 个地震测网；8 个层位与井点、范围共 10 个受管图层。
  保存后重读 catalog，核对分层数量；QGIS 实例化井点和范围图层，核对要素数与投影坐标。
- 回归：wellfileparsers 61、horizonbinner 9、import_ux 50、folderimport 27、
  projectdata 7、catalogstore 16；与新增套件合计 185 通过，0 失败、0 跳过。
- 原生四步向导截图复查通过，确认页明确列出 A18 的内部井名冲突与归属选择。
- layering strict / qgs-audit / symbol-audit / selftest / transitive strict
  （显式 QGIS 头目录）、qgis_spread strict、ui_invariants / ui_size /
  file_budget strict、i18n、ui_tokens、qgis_versions、docs_drift、known-reds 校验通过。

基线只收紧：file-budget 中 buildCenterArea 208→201；其余基线无增长。

PR #630 同步 master（#627–#629）后，保留“默认只建索引”和显示名覆盖参数，
井表映射另行透传；以默认外链 RAW 模式重新跑完真实资料入库。另补齐无总目录时
sourceArea.stats 的保存，并同步运行中的工程配置；自定义 ID/E/N 字段在保存、
重开及“工程资料设置”中均保留。主程序全量重编译通过。

现有古地理图补充（2026-10-22）：

- 自动发现“古地理图”目录，在其他资料页逐套勾选并确认层位；自动整理三级相可关闭。
  目录中的同名 GPKG 默认优先，SHP 与预览图可改选；单独指定 SHP 则尊重该选择。
- 复用 PaleoMapLibrary 的完整 RAW 文件组、可选 DERIVED、幂等重导和图层声明；
  常规导入提交后串行导入图件。所有选择保存到 sourceArea.stats。
- SHP/SHX/DBF 缺件不能导入；PRJ/CPG/QML 及 `<主件>.qml` 一并登记和核验。
  预览后增加、删除或修改配套文件会要求重新识别，不把 DBF 等作为独立资产。
- 真样例 D61/D62/D63 三份 GPKG 共 32 个图斑，生成 6 个原图／三级相图层；
  保存重开、重复导入不增资产。另选 D61 SHP 后核对 6 个受管成员及每个 SHA-256。
- 主程序构建通过；tst_projectsetup 17、tst_paleomaplibrary 27，共 44 通过，0 失败、
  0 跳过。其他资料页与确认页截图复查通过；源码闸门通过，基线无新增。
