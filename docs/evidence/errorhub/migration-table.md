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
