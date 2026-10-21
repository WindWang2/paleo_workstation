# [med] 批量出图交付文件名无唯一性保护：safeSegment 撞名/大小写不敏感致 QFile::copy 全批失败

## 问题

交付目录用平面文件名 `<project>_<safeSegment(horizon)>_<date>.<ext>`，无唯一性保护：两个仅在被 safeSegment 替换字符（/ \ : * ? " < > | 空格 制表）上不同的层位会得到同一文件名，QFile::copy 目标已存在即失败；Windows 大小写不敏感文件系统上「c6」与「C6」同样同文件。失败发生在全部渲染完成后，且错误信息不提示撞名。

## 证据

- `src/workflow/horizonbatchexport.cpp:254`：`const QString baseName = QStringLiteral("%1_%2_%3").arg(project, safeSegment(horizon), date);`；:43-49 safeSegment 把 '/'、' '、'\t' 等一律替换为 '_'（故 "K1/q" 与 "K1 q"、"K1_q" 三名同形）。
- :428-431：`const auto name = QFileInfo(path).fileName(); if (!QFile::copy(path, job->external->filePath(name))) return job->error = tr("无法写入批量导出文件：%1").arg(name);`——Qt 6 文档：目标已存在时 QFile::copy 返回 false、不覆盖；仓内 constraintstore.cpp:758-760 亦因此先 remove 再 copy。
- date 对所有层位相同（:195-196）、project 亦同，碰撞只取决于层位名消毒后是否同形。

## 影响

一次批量导出在全部渲染完成后才整体报「无法写入批量导出文件」，用户看不出是层位名撞车；含重名层位的工程只能改层位名才能导出。

## 建议

交付文件名拼上层位序号或资产/版本 ID（`%1_%2_%3_%4`），撞名时改报「层位 %1 与 %2 导出文件名相同」；metadata.json 的 horizons[].files 同步记录实际落盘名。

## 验证

造两个消毒后同名的层位（如 "K1/q" 与 "K1 q"）跑批量导出，断言交付目录内文件名唯一且清单记录一致；单测断言 copy 失败时错误信息含撞名提示。

## 关联

- 同文件同族：worker 报错仍发布外部目录（unconfirmed，见跟踪 issue）；外部交付发布条件应与 geologicalexport / mapexport 对齐。
