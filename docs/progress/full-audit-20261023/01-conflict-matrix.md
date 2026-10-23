# Full Audit 2026-10-23 — 01 外部工作排除矩阵

## Open PR

无。

## Open Issues（外部所有，同根因排除）

| Issue | 指纹 |
|-------|------|
| #646 | windows Test 串行 3.5h/39 项后被 6h 预算取消 |
| #645 | #637 后 linux 回归：相分类阈值/图例、井表文案、导入版本计数、岩性道标题、assetpaging 类型、onlinebasemap 像素 |
| #639 | tst_panels_dataops 5 失败（软删/标签/改名） |
| #638 | Windows 退出 0xC0000409：tst_import / tst_folderimport / tst_sectionlifecycle |
| #596 | UI 交互细节 37 项 |
| #595 | IO 残留：int 减法溢出、失败不回滚、析构不断信号 |
| #594 | 算法死代码 / 8 位回绕 / KD 树注释 |
| #593 | 空棒 0.0、哨兵文档漂移、时深单点零时间 |
| #592 | 数据层中文不走 tr，i18n 闸门只扫控件 |
| #591 | 闸门红单与实测不符、基线未收敛 |
| #590 | UI 阻塞 / 悬垂 this / 裸 QMessageBox / 3D GL 状态 |
| #589 | 空值哨兵三套、协克里金基台、取消谓词反相、ONNX 锁外读名 |
| #588 | 大文件整读、非原子写、孤儿登记、SHA 大小写 |
| #587 | 开库先写后验、索引缺项放行、幻影链接、不 fsync 目录 |
| #585 | 导航树测试索引断言过期导致段错误 |
| #561 | fencewidget 线性扫、ShaCache mtime-only、superbuild 无 CI |
| #559 | 测试薄弱区 |
| #558 | paleomainwindow.h / workflows.h 传递扇出 |
| #556 | mappingworkflow 失败只 qWarning |

## 可修范围

- 增量窗 #630–#647 的新缺陷（不含上表同根因）。
- #645 的根因修复可以做，但交付时引用 #645，不新开重复 issue。
- lint 参数名不一致（HEAD 仍在）属于本轮可修，不单独立 issue 除非修不进本 PR。
