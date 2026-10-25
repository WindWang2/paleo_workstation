# archive/legacy-python — 已退役 Python 历史原件归档

本目录集中保管**已确认退役**的 Python 历史文件。归档不等于删除：
所有文件经 `git mv` 迁入，原内容、文件名、相对路径结构与完整 Git
历史保持不变，可随时追溯或还原。

**本目录内容是历史实现/历史产物，不参与当前系统的构建、测试、
运行与交付。** 不要在本目录新增仍在演进的工具。

## 归档边界（审计基线）

- 审计基线 commit：`39515ad9`（origin/master，2026-10-25）
- 全量审计结论（66 个被跟踪 Python 文件的 A–E 分类、逐文件证据）
  见 `docs/progress/python-legacy-audit-20261025.md`
- 本仓库自首个 commit 起即为 C++20/Qt6/QGIS 工程，**从未存在过
  Python 版业务应用实现**；活跃 Python 文件均为工程工具、测试
  夹具、用户脚本功能（D 类脚本面）与供应链组件，**一律不入本目录**。

## 归档原因摘要

| 原路径 | 原因 |
|--------|------|
| `artifacts/gen_batch_status.py` | 2026-10-16 一次性评审交付 docx 生成器；输出路径硬编码在特定 Windows 机器（`C:\Users\wangj.KEVIN\...`）；产物 docx 已随 #598 同 commit 入库（自包含交付物在档）；全仓零引用；随 #598「收尾提交——工作区评审遗留」入库保存 |
| `artifacts/gen_review_doc.py` | 同上（2026-10-16 整体评价 docx 一次性生成器） |

逐文件详情（哈希、大小、历史、C++/替代实现状态）见 `MANIFEST.md`。

## 目录结构

```
archive/legacy-python/
├── README.md        本说明
├── MANIFEST.md      逐文件归档清单（含 SHA256/历史/理由）
├── SHA256SUMS       归档时内容哈希（sha256sum -c 可校验）
└── original/        原件，保留原相对路径结构
    └── artifacts/
        ├── gen_batch_status.py
        └── gen_review_doc.py
```

## 校验与还原

```sh
# 校验完整性
cd archive/legacy-python && sha256sum -c SHA256SUMS

# 还原到活跃目录（如需）
git mv archive/legacy-python/original/artifacts/<f>.py artifacts/<f>.py
# 或直接查看历史：
git log --follow -- archive/legacy-python/original/artifacts/<f>.py
```
