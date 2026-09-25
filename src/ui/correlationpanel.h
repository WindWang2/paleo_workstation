#pragma once
#include <QList>
#include <QPair>
#include <QString>
#include <QWidget>

// ui/ — WellCorrelationPanel: 连井剖面 (well correlation section) scaffold.
//
// The constraint page's linked sub-panel: an ordered strip of well-log
// column placeholders tied to map selection through SelectionContext
// (§41.3). P1 scope = data model + scene scaffold only — real log-track
// rendering is deferred. Pure Qt Widgets: no Qgs* types appear here (§25),
// the panel emits intents (wellClicked/wellDoubleClicked) and follows
// selectionChanged broadcasts whose origin isn't "correlation" (its own
// echo guard — future producers broadcast under that origin).

class QGraphicsScene;
class QGraphicsView;
class QLabel;
class SelectionContext;

class WellCorrelationPanel : public QWidget
{
  Q_OBJECT
  public:
    explicit WellCorrelationPanel(SelectionContext *ctx, QWidget *parent = nullptr);

    // (id, name) pairs in section order; re-layouts the scene.
    void setWells(const QList<QPair<QString, QString>> &wells);
    int wellCount() const { return m_wells.size(); }
    QString wellAt(int index) const;          // well id at section position

    void reorder(int from, int to);           // QList::move semantics (drag-reorder equivalent)
    bool isWellHighlighted(const QString &wellId) const;

  signals:
    void wellClicked(const QString &wellId);
    void wellDoubleClicked(const QString &wellId);

  private:
    void rebuildScene();
    void applySelection(const QStringList &ids);

    SelectionContext *m_ctx = nullptr;
    QList<QPair<QString, QString>> m_wells;   // (id, name), section order
    QGraphicsView *m_view = nullptr;
    QGraphicsScene *m_scene = nullptr;        // CorrelationScene, defined in .cpp
    QLabel *m_emptyLabel = nullptr;
};
