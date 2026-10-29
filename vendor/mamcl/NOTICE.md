# vendor/mamcl — 许可与归属说明

`vendor/mamcl/` 存放第三方交付的「地震多属性智能分析」（MAMCL）程序包，随仓库
分发以便运行时本地解包使用（打包 SHA-256 编进二进制，见
`cmake/extra-mamcl.cmake` 的 `PALEO_MAMCL_PACKAGE` / `PALEO_MAMCL_SHA256`）。

| 项 | 值 |
|---|---|
| 归档 | `MAMCL_software_W04_v0.2.1_20260924.zip` |
| 内部版本 | W04_v0.2.1，构建日期 2026-09-24 |
| 交付形态 | Windows GUI 程序（`MAMCL_GUI.exe`）+ Python 侧 `seismicfacies` 源码 |
| 依赖清单 | `MAMCL_software_W04_v0.2.1_20260924.requirements.lock`（`uv pip compile --universal --generate-hashes` 产物；pip 走 `--require-hashes`） |
| 许可 | **归档内没有任何许可/版权文本**（已逐条枚举 75 个条目核实：无 `LICENSE`/`COPYING`/`NOTICE`） |

## 现状与纪律（#738）

1. **许可状态待定**：上游交付物未附许可文本，本仓库不代为拟定，也不得在缺少
   授权依据的情况下随 paleo_workstation 的分发包外流。已登记在仓库根
   `NOTICE.md` 的「源码内 vendored 组件」表。
2. **只进不出**：当前用法是「内置程序包 → 运行时解包到 AppDataLocation/external/mamcl
   → 本机调用」，不镜像、不转发、不放到公开渠道。
3. **补齐动作**：向交付方索取书面许可（或确认其内部授权范围）后，把许可全文或
   授权说明落到本目录（建议 `LICENSE` 或 `NOTICE`），并同步更新根 `NOTICE.md`
   与 `cmake/extra-mamcl.cmake` 的头注。

## 已知的间接依赖许可

`requirements.lock` 里各 Python 包的许可由其各自的上游项目规定（numpy/scipy/
scikit-learn/matplotlib 为 BSD/MIT；PyTorch 为 BSD-3；kmeans-pytorch 为 MIT；
pyzgy/openzgy 为 Apache-2.0；segyio 为 LGPL——与 `vendor/segyio` 同源）。
运行时由用户本机 Python 环境安装，paleo_workstation 只负责启动编排。
