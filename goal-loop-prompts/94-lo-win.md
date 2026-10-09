# Goal-Loop 方向 94：Windows vendored LibreOffice + 文档预览跨平台收口

## 背景（实测事实，勿再勘察；行号为 2026-10-09 master `32851a1e` 口径）

#296 交付了 LibreOffice 26.2.6 headless 子集 vendored（Linux
侧三级探测 `PALEO_SOFFICE env > vendored > PATH`，`src/io/
dataimport_document.cpp:58-95`；manifest.json:161-174 钉
SHA256+size，12 个 headless deb 子集 core/ure/writer/impress/
draw/calc/images/en-us/ooofonts/graphicfilter + brand，26 文件
`${ORIGIN}` 相对可整体移动）。**Windows leg 缺口**：

- `manifest.json:174` notes 原文「Windows leg: MSI admin-extract
  deferred; resolver falls back to PATH soffice there」——
  Windows 侧文档预览转换依赖用户自装 soffice，无 vendored
  兜底（转换不可用则预览页降级「用系统程序打开」）。
- 消费面：document 资产（doc/docx/ppt/pptx/xls/xlsx）首次
  预览 `ensureDocumentPdf` headless 转 PDF → 受管 DERIVED
  版本（parent=RAW）→ QtPdf 渲染；外链 RAW 指纹复验
  （`:158-163`）。
- 官方 Windows 包是 MSI——非管理员解包需要 msiexec
  administrative install 或第三方提取；`vendor/fetch-libreoffice.sh`
  （122 行）目前只处理 deb（ar+tar）。

## 环境接线（Windows 本机实测口径；方向 72 后统一链）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\lo-win -b goal/lo-win-20261010 origin/master
cd .worktrees\lo-win
./paleo-dev.ps1 build
./paleo-dev.ps1 test
```

等价手工接线见 BUILDING.md:140-152。**本方向主战场就是
Windows**（Linux 侧已收口）——R0 先实测 MSI 解包可行路径。

## 目标形态（建议按序）

1. **R0 MSI 路径勘察**：官方 MSI 的无管理员解包实测——
  （a）`msiexec /a <msi> TARGETDIR=<dir> /qn` administrative
   extract（多数环境允许，无需提权）；（b）若 (a) 被组策略
   挡，第三方提取器合规性评估（7-Zip 提取 MSI 是常见路径，
   许可证 LGPL——依赖政策按「编译器工具链与构建依赖」例外
   评估）；两案实测结果记 ledger。
2. **fetch 脚本 Windows 段**：`fetch-libreoffice.sh` 增 Windows
   分支（PowerShell 或 bash-in-CI 均可——与仓内脚本风格
   对齐）——下载官方 MSI（SHA256 入 manifest.json）→ 解包
   → 只保留 headless 子集对应文件（program/soffice.exe 等
   ——Linux 12 deb 的 Windows 等价面）→ 目录布局与
   Linux vendored 对齐（resolver 探测序统一）。
3. **resolver 统一**：`resolveDocumentConverter` 三级探测在
   Windows 生效（env > vendored > PATH）——vendored 布局
   命中测试；`vendor/manifest.json` 的 libreoffice 节补
   windows 子表（msi url/sha256/解包布局）。
4. **版本对齐**：Windows MSI 版本与 Linux 26.2.6 同版本
  （跨平台转换结果可能有细微差异——PROVENANCE 记 soffice
   版本与平台，预览 DERIVED 版本可追溯）。
5. **CI**：windows leg bootstrap 增 LO 解包步（或独立步）；
   预览转换冒烟测试（docx→PDF 断言页数>0）进 Windows leg
  （可选非阻断起步）。
6. **测试**：Windows 下 vendored 探测命中（转换器路径断言）；
   docx/xlsx 夹具转换 round-trip；失败降级如实（无
   soffice→UI 降级路径——既有）；PROVENANCE 记平台/版本。
7. **文档**：BUILDING.md vendor 段补 LibreOffice Windows
   布局；manifest notes 的「deferred」句改写。

## 通用纪律（方向内全程有效）

- **分层**：resolver 在 io（既有），脚本在 vendor/；零 src/
  业务逻辑变更（除非 resolver 需 Windows 路径适配——最小）。
- **诚实面**：MSI 解包若不可行（组策略挡且无合规路径），
  如实保持 PATH 兜底 + 记因——不硬造。
- **依赖政策**：LibreOffice 官方包延续 #296 的 vendored 口径
  （SHA256 钉死）；MSI 解包产物只保留 headless 面需要的
  子集（不装全家桶）。
- **资源**：`./paleo-dev.ps1` 系；ctest 串行；MSI 下载与解包
  耗时记 ledger。
- **无人值守**：解包路径两案取舍自行定案记 ledger。
- **ledger**：`.goal-loop-ledger-lo-win.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审
  （脚本正确性/resolver 统一/manifest 完整/PROVENANCE/i18n
  五维）→ 修复 → 再 review，至少两轮零 High/Medium；Low 记
  PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. Windows vendored：本机解包产物布局与 resolver 探测命中
  （路径断言测试）；三级序生效（env > vendored > PATH）。
2. 转换：docx/xlsx 夹具 → PDF round-trip（页数/字节头断言）；
   PROVENANCE 记 soffice 版本+平台。
3. 失败降级：故意清 PATH+vendored → UI 降级路径如实（测试）。
4. manifest：windows 子表（msi url/sha256）入库；check 脚本
  （check_qgis_versions 同族或独立）可校验。
5. CI：windows leg 解包步 + 冒烟（若 CI 不等跑则本地完整
   证据 + yaml 结构核对）。
6. 全量 ctest 对照 R0 红集合 diff 为空；既有文档预览测试
   零改动通过。
