// 层：视图
#pragma once
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include "../../services/errorhub.h"

class QComboBox;
class QLineEdit;
class QPushButton;
class QTableWidget;

// 方向64：错误历史面板（ErrorHub 环形历史的查看面）。挂「布局与面板」菜单
// 的「错误历史」dock。过滤 = 级别下拉 + 文本（title/text/source 子串）；
// 复制 = 选中行（无选中则全部可见行）TSV 进剪贴板；清空 = ErrorHub::clear()。
// 刷新合并：errorRaised 风暴下只置脏 + 单发 150ms 定时器，隐藏时不重建表。
class ErrorHistoryPanel : public QWidget
{
    Q_OBJECT
public:
    explicit ErrorHistoryPanel(ErrorHub *hub, QWidget *parent = nullptr);

    // 级别下拉索引：0 全部 / 1 错误 / 2 警告 / 3 信息。
    void setLevelFilter(int index);
    void setTextFilter(const QString &text);
    QString copyText() const;     // 当前「复制」会写入剪贴板的内容
    int rowCount() const;
    void refreshNow();

protected:
    void showEvent(QShowEvent *event) override;

private:
    void scheduleRefresh();
    void copyToClipboard();

    QPointer<ErrorHub> m_hub;
    QComboBox *m_level = nullptr;
    QLineEdit *m_filter = nullptr;
    QTableWidget *m_table = nullptr;
    QPushButton *m_copy = nullptr;
    QPushButton *m_clear = nullptr;
    QTimer m_refresh;
    bool m_dirty = true;
};
