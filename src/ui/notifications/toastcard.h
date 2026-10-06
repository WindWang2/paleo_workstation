// 层：视图
#pragma once
#include <QFrame>
#include <QString>
#include <QTimer>
#include <functional>

#include "../../services/errorhub.h"

class QLabel;
class QToolButton;

// 方向64：单张通知卡（NotificationCenter 预建池成员，不单独使用）。
// 视觉见 DESIGN.md「错误呈现」节：surface 底 + 1px border + radiusMd +
// 左侧 4px 语义色条 + 级别文字 + 标题 + 正文 + 计数胶囊 + ✕。
class ToastCard : public QFrame
{
public:
    explicit ToastCard(QWidget *parent);

    void present(const ErrorHub::Entry &entry, int ms, quint64 seq);
    void bump(int count, int ms);
    void dismiss();

    bool isActive() const { return m_active; }
    const QString &key() const { return m_key; }
    quint64 seq() const { return m_seq; }
    QString text() const;

    std::function<void()> onDismissed;

private:
    void applyLevel(ErrorHub::Level level);

    QLabel *m_level = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_text = nullptr;
    QLabel *m_count = nullptr;
    QToolButton *m_close = nullptr;
    QTimer m_timer;
    QString m_key;
    quint64 m_seq = 0;
    int m_levelShown = -1;
    bool m_active = false;
};
