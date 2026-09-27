# SARibbon (vendored)

Qt Ribbon 控件库，主窗口 ribbon 壳（`PaleoMainWindow : SARibbonMainWindow`）用。

| 项 | 值 |
|---|---|
| 上游 | https://github.com/czyt1988/SARibbon |
| 版本 | v2.9.5（tag commit `a21d30c2a8495da92c7db3adcdc124699b0f2a60`，2026-09-18 发布） |
| 许可 | MIT（见 `LICENSE`） |
| 取用方式 | 上游 `src/` 下的 amalgamated 单文件：`SARibbon.h` + `SARibbon.cpp`（qrc 资源已内联） |
| 源 tarball SHA-256 | `2271c1a0560aa8e1350498bf929c8a4d8ecb49ea8ca62704c8b9b17bf2b457af`（`archive/refs/tags/v2.9.5.tar.gz`） |
| `SARibbon.h` SHA-256 | `66054a09e545464f66196fd429254aa1e294e6323141aa0fb89b0b4fbd96419d` |
| `SARibbon.cpp` SHA-256 | `f3849cdab29ca19df0899dac2692bd3975b592065abd29541f321bfd2a79358a` |

## 构建约定

- 顶层 `CMakeLists.txt` 把两个文件编成静态库 `paleo_saribbon`，
  `SA_RIBBON_BAR_NO_EXPORT` + `SA_COLOR_WIDGETS_NO_DLL`（静态嵌入）作为 PUBLIC 宏。
- 不启用 QWindowKit：`SARIBBON_USE_3RDPARTY_FRAMELESSHELPER=0`。主窗口用
  `UseNativeFrame`（系统原生标题栏），Linux X11/Wayland 与 offscreen 测试都走同一路径。
- 静态库里的 qrc 需要显式初始化：`Q_INIT_RESOURCE(SARibbonResource)`，由
  `PaleoMainWindow` 构造时调用。
- 颜色不用内置主题色：`office2021` 模板 + DESIGN.md token 调色板
  （`src/ui/paleotheme.cpp` 的 `ribbonPaletteJson()`）。

## 升级

1. 下载新 tag 的 tarball，核对 SHA-256，发布时间至少一周前。
2. 只替换 `SARibbon.h` / `SARibbon.cpp` / `LICENSE`，更新本表。
3. 全量 `./paleo-dev test`，重点看 `tst_ui`（ribbon 结构用例）。
