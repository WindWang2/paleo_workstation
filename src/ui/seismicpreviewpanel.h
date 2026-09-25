#pragma once
#include <QHash>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>
#include <memory>

// ui/ — SeismicPreviewPanel: 地震剖面预览 (seismic section preview) scaffold.
//
// The data page's linked sub-panel (§42.x): a seismic-line list on the left,
// a QGraphicsView stub on the right rendering a baseline profile + CDP tick
// marks + the line label for the selected asset. Real trace rendering is a
// later milestone — until then the scene is a visible placeholder, never
// blank (§42.4: the empty state is a guidance label over the preview).
//
// Selection contract (§41.3): SeismicMapLink owns no public selection API —
// its entry point for foreign picks is the private onContextSelection()
// slot fed by the SelectionContext it was constructed with (direction B).
// The panel therefore behaves like every other producer: it broadcasts line
// picks on that context under origin "seismicpanel", and skips broadcasts
// carrying its own origin when they come back around (the WellCorrelationPanel
// echo-guard pattern). The context is resolved from the link by, in order:
//   1. dynamic property "paleo.seismic.ctx" (QObject*) on the link — the
//      documented binding, same idiom as pagepanels' kLayersProp;
//   2. a SelectionContext that is the link's parent;
//   3. a SelectionContext child of the link, or found under the link's parent.
// When no context is reachable the panel still emits seismicSelected and
// invokes the link's onContextSelection slot directly, so the layer pick
// lands even in degraded wiring.
// Pure Qt Widgets: no Qgs* types appear in the public API (§25).

class SeismicMapLink;
class SelectionContext;
class QGraphicsScene;
class QGraphicsView;
class QLabel;
class QListWidget;
class QListWidgetItem;
struct SeismicLineData;

class SeismicPreviewPanel : public QWidget
{
  Q_OBJECT
  public:
    explicit SeismicPreviewPanel(SeismicMapLink *link, QWidget *parent = nullptr);

    // (id, label) pairs in list order; replaces the list wholesale.
    void setSeismicAssets(const QVector<QPair<QString, QString>> &assets);
    void addSeismicAsset(const QString &id, const QString &label);

    // Loads SEG-Y traces from file for the given asset id.
    // Parses traces, registers the asset if new, and updates preview if current or idle.
    void loadLineFromFile(const QString &assetId, const QString &segyPath);

    int assetCount() const { return static_cast<int>(m_assets.size()); }
    QString currentAsset() const { return m_currentId; } // previewed line id

    // --- 三视图联动（wave/mapping-pipeline 阶段C 本链路新增）--------------------
    // 滚到某测线某时间：记录目标（经 lastGoto* 暴露）；当前预览有剖面时
    // 视图同步滚动到该时间在剖面上的纵向位置（无剖面/无测线数据时只记
    // 目标，不造假画面）。阶段B 的一条 inline 解码落地后，测线号会用于
    // 选择对应剖面资产。
    void gotoLine(int inlineNo, double timeMs);
    int lastGotoInline() const { return m_gotoInline; }
    double lastGotoTimeMs() const { return m_gotoTimeMs; }

  signals:
    void seismicSelected(const QString &assetId);

  protected slots:
    // Re-renders the preview scene for a line id (empty → idle scaffold).
    void updatePreview(const QString &assetId);

  private:
    SelectionContext *context();                  // lazy resolve + connect
    void rebuildList();
    void onListSelection(QListWidgetItem *current);
    void applyExternalSelection(const QStringList &ids);
    bool hasAsset(const QString &id) const;
    QString assetLabel(const QString &id) const;  // "" when id unknown

    SeismicMapLink *m_link = nullptr;
    SelectionContext *m_ctx = nullptr;
    QVector<QPair<QString, QString>> m_assets;    // (id, label), list order
    QHash<QString, std::shared_ptr<SeismicLineData>> m_lineTraces;
    QString m_currentId;
    QListWidget *m_list = nullptr;
    QGraphicsView *m_view = nullptr;
    QGraphicsScene *m_scene = nullptr;
    QLabel *m_emptyLabel = nullptr;
    int m_gotoInline = -1;                       // 三视图联动最近一次 goto 目标
    double m_gotoTimeMs = qQNaN();
};
