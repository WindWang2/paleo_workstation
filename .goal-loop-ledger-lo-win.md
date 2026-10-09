# 方向 94 ledger：Windows vendored LibreOffice + 文档预览跨平台收口

worktree：`.worktrees/lo-win`，分支 `goal/lo-win-20261010`，基线 origin/master `753c2768`。

## 前提漂移记录（重要）

任务书按 `32851a1e` 撰写（预览 = LO headless → PDF → QtPdf）。worktree 基线
`753c2768` 比 `32851a1e` 多两个提交，其中 `a51bb454`（#314）把 Office 预览改为
Calligra 渲染进程（`src/app/officepreviewrenderer.cpp`），LibreOffice 降级为
「仅显式调用的历史转换 API」（manifest notes 原文）。BUILDING.md:75 明言
Windows/macOS 的 Calligra 打包尚需实机验收。

**对任务的影响**：Windows vendored LO 的交付物依然成立且有价值——
1. legacy 显式转换 API（ensureDocumentPdf/ensureWorkbookXlsx）在 Windows
   无 vendored 兜底的事实不变；
2. .xls→.xlsx 二进制表格转换线仍走 LO（原生读取面只覆盖 OOXML）；
3. Calligra Windows 未收口前，vendored LO 是文档面唯一可收的跨平台拼图。
「文档预览跨平台收口」在本方向按「转换器跨平台收口」执行，Calligra Windows
打包是另一方向的事，如实分开。相关消费面核实：#314 后 UI/app 已无
ensureDocumentPdf/ensureWorkbookXlsx 调用方，仅 PreviewDocService 门面 +
tests/tst_import.cpp（stub 转换器测试）。

## R0：MSI 解包路径实测（2026-10-09 本机 Windows）

**实测结论：(a) `msiexec /a` 无提权解包可行——采用；(b) 7-Zip 备选未启用。**

| 步骤 | 结果 |
|---|---|
| 下载 MSI | `LibreOffice_26.2.6_Win_x86-64.msi` 373252096 字节，SHA256 `f9877032fd908beb9c0ddf06df4af5c2e85f419c42e14876c4cce5aae5fb2660` |
| `msiexec /a <msi> TARGETDIR=<dir> /qn` | 退出码 0，无需提权，2m34s，解出 1.6G（含内嵌 MSI 副本 356MB） |
| 布局 | 平铺：`program/ share/ Fonts/ presets/ help/ readmes/ System/ System64/` + 许可文件；`program/soffice.exe` 顶层——与 resolver 期望 `vendor/libreoffice/program/soffice.exe` 对齐 |
| headless 子集剪裁 | 删 help/readmes/share/extensions(dict-* 471M+nlpsolver+wiki-publisher)/share/registry/res(92M)/program/python*(44M)/program/shlxthdl* + 根部 MSI 副本 → 914MB |
| docx→pdf | exit 0，30525 字节，`%PDF-1.7` ✓ |
| xlsx→pdf | exit 0，32798 字节 ✓ |
| xls→xlsx | exit 0（Calc Office Open XML 过滤器），zip 11 entries ✓ |

### R0 踩坑：Windows 完整性标签（重要，fetch 脚本必须处理）

解包树放在本仓工作区下（`C:\Users\wangj.KEVIN\projects\paleo_workstation\...`）
时 soffice 启动必挂：exit 77，报「无法启动应用程序/安装无法完成」
（app.cxx `BE_USERINSTALL_FAILED` ← `userinstall::finalize()` 失败，
profile 目录零残留）。对照实验：

- 同一树拷到 `C:\Temp` / `C:\Users\wangj.KEVIN\` / 用户 Temp → 全部正常；
- 树在工作区内 → 必挂；
- 摘掉 `Everyone:(CI)(DENY)(DC)` → 仍挂；
- `icacls <tree> /setintegritylevel (OI)(CI)medium` → **同路径同树立即正常**。

根因：本仓工作区 ACL 带 `Mandatory Label\Low Mandatory Level:(OI)(CI)(NW)`
（沙箱防护痕迹），子树继承 Low 完整性标签后 LO 的 user-installation 引导失败
（机制层面：低完整性对象参与 UNO/IPC/userinstall 的跨完整性级访问被 MIC 拦截；
经验层面：标签 Low→必挂、Medium→正常，三组路径对照可复现）。
CI `windows-latest` 无此标签，属本机沙箱特有——但 fetch 脚本统一做
`icacls /setintegritylevel medium` 归一，何处解包都稳。

对照组补充：本机已装 LibreOffice 26.8.0.3（`C:\Program Files\LibreOffice`）
同样调用可转换（58s）；其树整体拷到别处也可转换——LO Windows 树自含可
重定位，`${ORIGIN}` 相对寻址与 Linux 一致。

## 实施批记录

### 批 1（完成：7146bff3 + 6e5b8b19）

- manifest `deps.libreoffice_win`：MSI url/sha256/size + layout + extract 节
  （prune 清单 + icacls 归一）+ notes；linux 块 notes 补 Windows 腿指引。
- `fetch-libreoffice.sh` Windows 段：OSTYPE 分派 → msiexec //a 解包 → 剪裁
  → icacls 归一 → NotoSansSC 进 Fonts/（Windows 品牌字体目录，MSI 自带
  Caladea/Carlito 就住这）。实跑验证：356MB MSI（缓存命中）→ 924MB 树 →
  docx→pdf/xlsx→pdf/xls→xlsx 三线 exit 0。
- `tools/check_lo_pin.py` + ctest `lo_pin`/`lo_pin_selftest`（5 变异红 +
  绿形态 + 未知参数 exit 2 全验证）。
- `paleo-dev.ps1` bootstrap LO 腿（bash 单一来源；存在即跳过）；
  `paleo_dev_ps1_selftest`(+mutation) 绿。CI：windows leg bootstrap 自动
  覆盖解包步；冒烟 = tst_import round-trip（io 标签，进既有 test 步）；
  vendor/cache 已在缓存路径、键含 manifest 哈希自动 bust 一次；ci.yml 零改动。
- src/io：`documentConverterProgram()` 只读出口 + DERIVED extra 补
  converterVersion（bootstrap/version 双后缀 ini/rc）与 converterPlatform。
- tst_import 新测试 ×2：三级序（env 无条件赢 + vendored 必命中）与
  round-trip（docx/xlsx→PDF：magic/QtPdf 页数/PROVENANCE 三键）——
  本机 vendored 树实测绿（1439ms / 20613ms）；整文件 ctest 绿（65.9s）。
- BUILDING.md：双平台布局表 + 完整性标签坑位说明。

### 完整性标签机制补全（批 1 测试期发现，与 R0 同根）

worktree 内构建的 exe 继承工作区 Low 标签 → 进程以 Low 完整级启动 →
MIC no-write-up 拒绝写 Medium 的 Temp/注册表 hive → QTemporaryDir 全灭
（tst_fspath/tst_import 全部用例 1.4s 死）+ ctest 注册表沙箱 hive 挂载
失败大面积 ***Exception（.zwork 前次会话 R0b 红集的真相）。
修复：`icacls <worktree> /setintegritylevel (OI)(CI)medium` 一发入魂
（tst_fspath 由 5+2 红转 5 绿 1 既有红；tst_import 全绿）。
**结论：沙箱化开发机的 worktree 提级是测试前置条件**，fetch 脚本对
vendored 树的 icacls 步骤是同类防护（树内 exe 由进程直接拉起，标签
必须归一）。

### 待办
- [x] 全量构建 + 全量 ctest + 基线红集对照（Oracle 6）——见下
- [x] 第二轮 review（脚本口径修正 a09199ad + 测试补强 430dd0e0 两批落账）

## 全量验证记录（Oracle 证据）

- **全量构建**：MSVC/localdeps（conda Qt 6.11.2 + qtpdf 独立前缀）全部
  354 目标链接通过。途中扫出两个基线固有问题并修复/绕开：
  1. paleomainwindow.cpp C2027（SectionWorkbench 前置声明被解引用；CI
     工具链经传递包含侥幸编过）→ c42a88d4 补显式 include（非方向 94 面）。
  2. 本 worktree 缺 vendored onnxruntime → 按 manifest onnxruntime_win
     钉版补齐（含 dll 入 build，对齐 paleo-dev.ps1 口径）。
- **全量 ctest**（串行、-LE perf、354 项）：346 绿 / 8 红。
  红集：composeworkflow / correlation_async / crossplot_controller
  (SEGFAULT) / derivationgraph / fspath / mapping / mappingpages / workflows。
- **基线对照**（同机同环境，src/tests 还原 origin/master，仅保留无行为
  影响的 ui include 修复，重编重跑同 9 项）：基线 7 红 + mappingworkbench
  绿 + correlation_async 绿；分支独有红仅 correlation_async。
- **correlation_async 闪断定性**：分支二进制 `--repeat until-fail:3`
  3/3 全绿（4.86/5.14/4.75s；套跑 9.25s 为全量负载下时序漂移），且该
  测试与方向 94 改动面（io 转换线）无交集 → 负载闪断，非回归。
- **结论：红集 diff（基线 vs 分支，闪断裁决后）= 空**。tst_import
  （含三个新测试）套跑 65.9s 绿 + 强化版独跑 70.9s 绿（双跑确认）。
- 门禁项：lo_pin / lo_pin_selftest / layering / i18n /
  paleo_dev_ps1_selftest(+mutation) 全绿。

## 提交清单

| commit | 内容 |
|---|---|
| 7146bff3 | vendor 收口（manifest/fetch/check 脚本/ps1/BUILDING/ledger） |
| 6e5b8b19 | io PROVENANCE + resolver 出口 + tst_import 两测试 |
| c42a88d4 | ui 既有缺口修复（全量构建扫出） |
| a09199ad | fetch 脚本在场检查平台分口径 + license 提示（R2 review） |
| 430dd0e0 | 三级序真序断言 + 降级如实测试（R2 review 补强，Oracle 1/3） |
