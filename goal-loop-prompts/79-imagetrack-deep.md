# Goal-Loop 方向 79：图片道深化——锚深后补编辑 + 缩略图 LOD + 管理面板

## 背景（实测事实，勿再勘察；行号为 2026-10-08 master `e3c8d31d` 口径）

图片道系统（#265/#268 交付：岩心/薄片照片按 depthMd 锚挂进
综合柱状图与连井剖面）已通，但有三个实质量缺口：

1. **锚深导入后不可编辑（数据管理硬伤）**：depthMd 只在导入
   时由文件名正则 `(\d+(\.\d+)?)\s*m(?![A-Za-z0-9])` 提取
  （`src/io/dataimport_file.cpp:300-311`，无单位深度不猜）；
   `:303` 注释明言「addVersion 之后无版本更新面，须在此并入」
   ——文件名不合规的照片无法补锚、录错的深度无法修正。
2. **无缩略图/降采样（下一个性能墙）**：装载在任务线程✓
  （`src/workflow/wellsectionworkflow.cpp:403-408`
   collectCoreImages），但每张照片**全分辨率 QImage 常驻**；
   剖面侧缓存键 `((wellIdx<<16)|idx)<<8 | imageVersion&0xFF`
  （`src/ui/wellsection/wellsectionscene.h:156-158`）——8 位
   截断 version，256 次数据变更后会假命中（低危已在仓内
   注记）。几百张高分辨率岩心照片是可预期的日常负载。
3. **管理面薄**：无专门井附件面板；管理=导航树「岩心照片
  (N)」分组（`src/ui/pages/datalist_tree.cpp:225-257`）+
   批量挂接。透明 PNG、EXIF 旋转、超大图 minification 质量
   未见处理面（保守标注：非确认缺陷，是未见处理证据）。

## 环境接线（Windows 本机实测口径；方向 72 后统一链）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\imagetrack-deep -b goal/imagetrack-deep-20261009 origin/master
cd .worktrees\imagetrack-deep
./paleo-dev.ps1 build
./paleo-dev.ps1 test
```

等价手工接线见 BUILDING.md:140-152。

## 目标形态（建议按序）

1. **锚深后补编辑**：版本更新通道——catalog 侧 addVersion 后
   的 extra 更新路径（勘察 catalog 写面契约：直接 update vs
   新版本落 DERIVED；锚深是导入元数据非派生结果，倾向就地
   update + 版本备注，契约注释钉死）；编辑入口：导航树
   附件节点「编辑锚深」+ 图片道双击照片（预览 + 深度输入）。
2. **缩略图 LOD**：装载即生成两级缩略图（缩略 ~256px 进
   内存缓存 + 原图驻磁盘按需全载）——LodPolicy 注释钉死
  （何时全载：放大到可见宽度 > 缩略宽度的系数）；缓存键
   修复 8 位 version 截断（换 64 位键或 hash 口径）。
3. **EXIF/透明/大图**：EXIF Orientation 读取应用（Qt
   QImageReader 原生）；透明 PNG 的棋盘底；minification
   用平滑变换（SmoothTransformation）——三件都是装载路径
   的统一处理。
4. **井附件管理面板**：`src/ui/wellcomposite/` 或既有面板族
   内加附件页（按井列照片清单：缩略图/文件名/锚深/角色/
   来源版本；行内编辑锚深；删除=版本级不物理删文件）；
   导航树「岩心照片 (N)」节点双击跳面板。
5. **测试**：锚深编辑 round-trip（catalog 契约测试）；LOD
   两级装载（内存峰值断言——比率口径）；EXIF 旋转样张
  （仓内夹具小图）；面板 CRUD offscreen 测试。
6. **文档**：井附件锚深契约（文件名惯例 + 后补编辑语义）
   进 docs/progress 或 PROJECT_FILE_DESIGN 同族文档。

## 通用纪律（方向内全程有效）

- **分层**：版本更新契约归 catalog/metadata（数据层），装载
  LOD 归 services/projectdata，编辑编排归 workflow，面板归
  ui；`check_layering.py --strict` 绿。
- **诚实面**：无法解析/无锚的照片在面板如实列「未锚定」
  （可后补——正是本方向价值）；删除是版本操作不物理删。
- **资源**：`./paleo-dev.ps1` 系；ctest 串行。
- **无人值守**：锚深更新契约（就地 vs 新版本）自行定案记
  ledger（判据：审计面与撤销语义）。
- **性能断言**：批量照片（夹具 200 张）装载内存峰值比率门
  （LOD 前后对比），禁绝对 MB 墙。
- **ledger**：`.goal-loop-ledger-imagetrack-deep.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审
  （分层/catalog 契约/LOD 正确性/显示质量/i18n 五维）→
  修复 → 再 review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 锚深编辑：导入→补锚→重新打开 round-trip 一致（catalog
   契约测试）；无单位/非法输入拒收列因。
2. LOD：200 张夹具照片装载内存峰值较全分辨率基线下降
  （比率断言，系数记 ledger）；放大到缩略尺寸以上触发
   全载（行为断言）；缓存键假命中场景（>256 次版本变更）
   回归绿。
3. EXIF：旋转样张在道内正立显示（像素采样断言）；透明
   PNG 棋盘底可见。
4. 面板：CRUD offscreen 全绿；未锚定照片清单如实；删除
   为版本操作（文件仍在磁盘——断言）。
5. 回归：tst_import 图片锚面 / tst_wellsection_workflow
   collectCoreImages 全绿零改动；全量 ctest 对照 R0 红集合
   diff 为空。
