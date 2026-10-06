# 方向64 调用点迁移行为对照表

口径：文本（title/text 实参）逐字透传不改写；时机 = 原调用点同一位置同步入账（toast/状态栏立即出现；原模态阻塞→非阻塞，后续语句提前执行，下表逐点已核对后续语句只是 return/状态更新，不依赖用户点掉框）；失败语义（返回值/提前 return/状态回滚）不变。无 hub 或无呈现层时回落原 QMessageBox 调用。

| 位置（迁移前行号） | 旧 | 新 | 呈现通道 | 标题 |
|---|---|---|---|---|
| `src/ui/welltops/welltopseditordialog.cpp:447` | `QMessageBox::information` | `PaleoNotify::information` | 状态栏 info（5s） | tr("先落定当前改动") |
| `src/ui/welltops/welltopseditordialog.cpp:510` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("井名不合法") |
| `src/ui/welltops/welltopseditordialog.cpp:706` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("无法保存") |
| `src/ui/welltops/welltopseditordialog.cpp:719` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("无法保存") |
| `src/ui/welltops/welltopseditordialog.cpp:730` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("保存失败") |
| `src/ui/welltops/welltopseditordialog.cpp:756` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("无法合并") |
| `src/ui/welltops/welltopseditordialog.cpp:787` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("批量修正被校验阻断") |
| `src/ui/welltops/welltopseditordialog.cpp:799` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("批量修正失败") |
| `src/ui/welltops/welltopseditordialog.cpp:960` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("回滚失败") |
| `src/ui/paleomainwindow.cpp:570` | `QMessageBox::critical` | `PaleoNotify::critical` | severe 模态（同键60s单弹） | tr("打开工程失败") |
| `src/ui/paleomainwindow.cpp:582` | `QMessageBox::critical` | `PaleoNotify::critical` | severe 模态（同键60s单弹） | tr("新建工程失败") |
| `src/ui/paleomainwindow.cpp:605` | `QMessageBox::critical` | `PaleoNotify::critical` | severe 模态（同键60s单弹） | tr("打开工程失败") |
| `src/ui/paleomainwindow.cpp:1294` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("导入工区文件夹") |
| `src/ui/paleomainwindow.cpp:1425` | `QMessageBox::critical` | `PaleoNotify::critical` | severe 模态（同键60s单弹） | title |
| `src/ui/paleomainwindow.cpp:1427` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | title |
| `src/ui/paleomainwindow.cpp:1434` | `QMessageBox::information` | `PaleoNotify::information` | 状态栏 info（5s） | tr("从工区文件夹新建") |
| `src/ui/paleomainwindow.cpp:1592` | `QMessageBox::critical` | `PaleoNotify::critical` | severe 模态（同键60s单弹） | tr("保存编辑失败") |
| `src/ui/paleomainwindow_attach_mapping.cpp:289` | `QMessageBox::information` | `PaleoNotify::report` | 模态 information（保留清单③） | tr("导出成功") |
| `src/ui/welltops/welltopseditordialog.cpp:459` | `QMessageBox::question`(Yes\|No, 默认 No) | `PaleoNotify::ask(YesNo, Reject)` | 模态（保留清单①），同按钮/默认键，返回 Yes 判定不变 | 未保存的改动 |
| `src/ui/welltops/welltopseditordialog.cpp:794` | `QMessageBox::question`(Ok\|Cancel, 默认 Cancel) | `PaleoNotify::ask(OkCancel, Reject)` | 模态① | 批量修正确认 |
| `src/ui/welltops/welltopseditordialog.cpp:952` | `QMessageBox::question`(Ok\|Cancel, 默认 Cancel) | `PaleoNotify::ask(OkCancel, Reject)` | 模态① | 回滚确认 |
| `src/ui/edittools/editingtoolbar.cpp:474` | 自建 `QMessageBox`(Question; 保存/放弃/取消; 默认保存) | `PaleoNotify::askSaveDiscard(Question)` | 模态①，同图标/按钮文案/角色/默认键 | 退出编辑 |
| `src/ui/edittools/editingtoolbar.cpp:606` | `QMessageBox::question`(Ok\|Cancel, 默认 Cancel) | `PaleoNotify::ask(OkCancel, Reject)` | 模态① | 放弃编辑 |
| `src/ui/paleomainwindow.cpp:1570` | 自建 `QMessageBox`(Warning; 保存/放弃/取消; 默认保存) | `PaleoNotify::askSaveDiscard(Warning)` | 模态①；Esc/✕ 仍 = 取消（event->ignore） | 未保存的编辑 |
| `src/ui/paleomainwindow_attach_mapping.cpp:129` | `QMessageBox::question`(Ok\|Cancel, 默认 Cancel) | `PaleoNotify::ask(OkCancel, Reject)` | 模态① | 发布版本 |
| `src/ui/paleomainwindow_attach_mapping.cpp:258` | `QMessageBox::warning`(Retry\|Cancel, 默认 Retry) | `PaleoNotify::ask(RetryCancel, Accept, Warning)` | 模态①，同 warning 图标 | 导出失败（重试） |
| `src/ui/paleomainwindow_attach_compose.cpp:267` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("删除失败") |
| `src/ui/paleomainwindow_attach_compose.cpp:293` | `QMessageBox::information` | `PaleoNotify::information` | 状态栏 info（5s） | tr("批量出图") |
| `src/ui/paleomainwindow_attach_compose.cpp:307` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("批量出图") |
| `src/ui/paleomainwindow_attach_compose.cpp:335` | `QMessageBox::information` | `PaleoNotify::report` | 模态 information（保留清单③） | tr("批量出图") |
| `src/ui/pages/wellsitingpanel.cpp:364` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr( "覆盖诊断" ) |
| `src/ui/pages/wellsitingpanel.cpp:429` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr( "候选生成" ) |
| `src/ui/pages/wellsitingpanel.cpp:483` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr( "添加计划井" ) |
| `src/ui/pages/wellsitingpanel.cpp:490` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr( "添加计划井" ) |
| `src/ui/pages/wellsitingpanel.cpp:501` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr( "地图布点" ) |
| `src/ui/pages/wellsitingpanel.cpp:547` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr( "删除计划井" ) |
| `src/ui/pages/wellsitingpanel.cpp:567` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr( "改名" ) |
| `src/ui/pages/wellsitingpanel.cpp:623` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr( "保存方案" ) |
| `src/ui/pages/wellsitingpanel.cpp:671` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr( "删除方案" ) |
| `src/ui/datapreview/datapreviewtabseismic.cpp:501` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | QObject::tr("转码未完成") |
| `src/ui/datapreview/datapreviewtabseismic.cpp:510` | `QMessageBox::information` | `PaleoNotify::information` | 状态栏 info（5s） | QObject::tr("转码完成") |
| `src/ui/datapreview/datapreviewtabseismic.cpp:512` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | QObject::tr("转码包含坏道") |
| `src/ui/datapreview/datapreviewtabseismic.cpp:564` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | QObject::tr("分页转码未完成") |
| `src/ui/datapreview/datapreviewtabseismic.cpp:572` | `QMessageBox::information` | `PaleoNotify::information` | 状态栏 info（5s） | QObject::tr("分页转码完成") |
| `src/ui/paleomainwindow_attach.cpp:84` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | QObject::tr("网格化") |
| `src/ui/paleomainwindow_attach.cpp:89` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | QObject::tr("网格化") |
| `src/ui/paleomainwindow_attach.cpp:98` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | QObject::tr("网格化") |
| `src/ui/paleomainwindow_attach.cpp:149` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | QObject::tr("网格化") |
| `src/ui/paleomainwindow_attach.cpp:232` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | QObject::tr("网格化失败") |
| `src/ui/paleomainwindow_attach.cpp:244` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | QObject::tr("面运算") |
| `src/ui/paleomainwindow_attach.cpp:273` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | QObject::tr("面运算失败") |
| `src/ui/layers/layerpropertiesdialog.cpp:306` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("保存样式预设") |
| `src/ui/layers/layerpropertiesdialog.cpp:324` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("从预设恢复") |
| `src/ui/layers/layerpropertiesdialog.cpp:346` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("导出样式 .qml") |
| `src/ui/layers/layerpropertiesdialog.cpp:351` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("导出样式 .qml") |
| `src/ui/layers/layerpropertiesdialog.cpp:366` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("导入样式 .qml") |
| `src/ui/layers/layerpropertiesdialog.cpp:373` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("导入样式 .qml") |
| `src/ui/layers/layerpropertiesdialog.cpp:378` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("导入样式 .qml") |
| `src/ui/wellcomposite/wellcompositepanel.cpp:1031` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("导出失败") |
| `src/ui/wellcomposite/wellcompositepanel.cpp:1436` | `QMessageBox::information` | `PaleoNotify::information` | 状态栏 info（5s） | tr("不可编辑") |
| `src/ui/wellcomposite/wellcompositepanel.cpp:1581` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("导出失败") |
| `src/ui/wellcomposite/wellcompositepanel.cpp:1614` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("打印失败") |
| `src/ui/wellcomposite/wellcompositepanel.cpp:1627` | `QMessageBox::information` | `PaleoNotify::information` | 状态栏 info（5s） | tr("导出预设") |
| `src/ui/wellcomposite/wellcompositepanel.cpp:1635` | `QMessageBox::information` | `PaleoNotify::report` | 模态 information（保留清单③） | tr("导出预设") |
| `src/ui/pages/entitypanel.cpp:605` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("重名") |
| `src/ui/pages/entitypanel.cpp:665` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("重名") |
| `src/ui/seismic3d/seismic3dviewpanel.cpp:1044` | `QMessageBox::information` | `PaleoNotify::information` | 状态栏 info（5s） | tr("三维截图") |
| `src/ui/seismic3d/seismic3dviewpanel.cpp:1054` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("截图失败") |
| `src/ui/seismic3d/seismic3dviewpanel.cpp:1058` | `QMessageBox::information` | `PaleoNotify::information` | 状态栏 info（5s） | tr("已导出") |
| `src/ui/seismic3d/seismic3dviewpanel.cpp:1060` | `QMessageBox::critical` | `PaleoNotify::critical` | severe 模态（同键60s单弹） | tr("导出失败") |
| `src/ui/dialogs/griddingdialog.cpp:178` | `QMessageBox::information` | `PaleoNotify::information` | 状态栏 info（5s） | QObject::tr("面运算") |
| `src/ui/dialogs/griddingdialog.cpp:222` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | QObject::tr("面运算") |
| `src/ui/dialogs/griddingdialog.cpp:265` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | QObject::tr("导出失败") |
| `src/ui/dialogs/griddingdialog.cpp:270` | `QMessageBox::information` | `PaleoNotify::information` | 状态栏 info（5s） | QObject::tr("导出完成") |
| `src/ui/pages/datalist_undo.cpp:439` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("物理删除（部分未完成）") |
| `src/ui/pages/datalist_undo.cpp:481` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("归位失败") |
| `src/ui/pages/datalist_undo.cpp:563` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("回滚失败") |
| `src/ui/wellcomposite/curveconfigdialog.cpp:625` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("提示") |
| `src/ui/wellcomposite/curveconfigdialog.cpp:675` | `QMessageBox::information` | `PaleoNotify::information` | 状态栏 info（5s） | tr("提示") |
| `src/ui/paleomainwindow_attach_validate.cpp:182` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("导出方案点位表") |
| `src/ui/paleomainwindow_attach_validate.cpp:194` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | tr("导出覆盖对比图") |
| `src/ui/datapreview/previewprofilepanel.cpp:417` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | QObject::tr( "导出失败" ) |
| `src/ui/datapreview/previewprofilepanel.cpp:431` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | QObject::tr( "导出失败" ) |
| `src/ui/paleomainwindow_attach_shell.cpp:203` | `QMessageBox::critical` | `PaleoNotify::critical` | severe 模态（同键60s单弹） | tr("保存工程失败") |
| `src/ui/dialogs/folderconfirm.cpp:482` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | QObject::tr("导入工区文件夹") |
| `src/ui/datapreview/previewidentifypanel.cpp:184` | `QMessageBox::warning` | `PaleoNotify::warning` | 非模态通知卡（8s） | QObject::tr( "导出失败" ) |
| `src/ui/datapreview/datapreviewtabimage.cpp:167` | `QMessageBox::information` | `PaleoNotify::report` | 模态 information（保留清单③） | tr("去配准") |
| `src/ui/paleomainwindow_attach_compose.cpp:261` | `QMessageBox::question`(Ok\|Cancel, 默认 Cancel) | `PaleoNotify::ask(OkCancel, Reject)` | 模态① | 删除版面 |
| `src/ui/pages/datalist_batch.cpp:130` | `QMessageBox::question`(缺省 Yes\|No, NoButton) | `PaleoNotify::ask()`（缺省 YesNo/Platform） | 模态①；tst_panels 点「Yes/No」路径不变 | 移除资产 |
| `src/ui/pages/datalist_batch.cpp:292` | `QMessageBox::warning`(Yes\|No, NoButton) | `PaleoNotify::ask(YesNo, Platform, Warning)` | 模态①；tst_panels 点 Yes 路径不变 | 角色变更（不可撤销） |
| `src/ui/pages/entitypanel.cpp:461` | `QMessageBox::warning`(Yes\|No, NoButton) | `PaleoNotify::ask(YesNo, Platform, Warning)` | 模态① | 角色变更（不可撤销） |
| `src/ui/layout/layouttemplates.cpp:394` | `QMessageBox::question`(Ok\|Cancel, 默认 Cancel) | `PaleoNotify::ask(OkCancel, Reject)` | 模态① | 应用内置模板 |
| `src/ui/attributetablepanel.cpp:184` | `QMessageBox::question`(Discard\|Cancel, 默认 Cancel) | `PaleoNotify::ask(DiscardCancel, Reject)` | 模态①（offscreen 跳过分支不变） | 放弃编辑 |
| `src/ui/datapreview/datapreviewtabseismic.cpp:479` | `QMessageBox::question`(缺省) | `PaleoNotify::ask()` | 模态① | 转码地震工作区 |
| `src/ui/datapreview/datapreviewtabseismic.cpp:538` | `QMessageBox::question`(缺省) | `PaleoNotify::ask()` | 模态① | 转码分页工作区 |
