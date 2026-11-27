// 层：视图
#pragma once

#include <QPointer>
#include <QWidget>

class QSplitter;
class QTabBar;
class QLabel;
class QToolButton;
class PaleoDecorationManager;

// 同一主画布和参考面板在宽屏并排，在窄屏共享同一位置。
class WorkspaceComparison final : public QWidget
{
    Q_OBJECT
public:
    explicit WorkspaceComparison(QWidget *mainMap, QWidget *parent = nullptr);
    ~WorkspaceComparison() override;
    void present(QWidget *reference);
    void release();
    bool comparing() const { return !m_reference.isNull(); }
    QByteArray splitState() const;
    void restoreSplit(const QByteArray &state);
    void setPrimaryLegend(PaleoDecorationManager *legend);
    void refreshLegends();

signals:
    void closeRequested();
    void dockRequested();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void arrange();
    QWidget *m_main;
    QWidget *m_controls;
    QLabel *m_caption;
    QSplitter *m_split;
    QTabBar *m_tabs;
    QToolButton *m_settings;
    QToolButton *m_legends;
    QPointer<PaleoDecorationManager> m_primaryLegend;
    QPointer<QWidget> m_reference;
    QByteArray m_savedSplit;
    bool m_wide = false;
};
