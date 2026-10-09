// 层：视图
#pragma once
#include <QDialog>
#include <QString>
#include <QStringList>
#include <QVector>

#include "../../workflow/wellattachmentops.h"

class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;
class DataCatalog;
namespace paleo::dataops
{
class RecycleBin;
}

// ui/pages/wellattachmentpanel — 井附件管理面板（方向 79）：按井列
// core/lab_analysis 照片清单（缩略图/文件名/锚深/来源/角色/版本/状态），
// 未锚定照片如实列「未锚定」——正是后补编辑的入口面。锚深行内编辑 +
// 对话框编辑共用 workflow/wellattachmentops 的校验（非法输入拒收列因，
// 状态行出 reasonText 文案）；清除/移除是显式按钮（清锚有确认）。移除 =
// 软删意图信号回属主（DataListPanel 的命令栈 + 可回收清单——版本级
// catalog 操作，磁盘文件不动）。
class WellAttachmentPanel : public QDialog
{
  Q_OBJECT
  public:
    // cat 只读 + mutator（锚深就地更新，与 DataListPanel 直改 catalog 同
    // 口径）；recycle 只用于过滤已软删行（移除执行归属主）。
    WellAttachmentPanel(DataCatalog *cat, const QString &projectDir,
                        const paleo::dataops::RecycleBin *recycle,
                        QWidget *parent = nullptr);
    ~WellAttachmentPanel() override;

    // 属主指定井（树「岩心照片 (N)」双击跳转）；不重载已选井。
    void setWell(const QString &wellId);
    void refresh();

  signals:
    // 软删意图（属主走 BatchRemoveCmd——可撤销、共享可回收清单）。
    void removeRequested(const QStringList &assetIds);

  private:
    void buildUi();
    void reloadWells();
    void applyCellEdit(int row, QTableWidgetItem *item, const QString &text);
    void openEditorForRow(int row);          // 预览 + 深度输入对话框
    void clearAnchorForRow(int row);
    void removeSelected();
    void setStatus(const QString &text);     // 空串 = 清

    DataCatalog *m_cat = nullptr;
    QString m_projectDir;
    const paleo::dataops::RecycleBin *m_recycle = nullptr;
    QVector<paleo::WellAttachmentRow> m_rows;
    QComboBox *m_wellBox = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_editBtn = nullptr;
    QPushButton *m_clearBtn = nullptr;
    QPushButton *m_removeBtn = nullptr;
    bool m_updating = false; // 程序化刷新期间屏蔽 itemChanged 回声
};
