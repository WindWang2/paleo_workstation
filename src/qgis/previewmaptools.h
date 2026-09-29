// 层：QGIS 封装
#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QVector>

#include <qgsmapmouseevent.h>
#include <qgsmaptool.h>
#include <qgspointxy.h>
#include <qgsrectangle.h>

class QgsMapCanvas;
class QgsMapTool;
class QgsRubberBand;
class QElapsedTimer;
class QKeyEvent;
class QMouseEvent;
class QEvent;

// qgis/previewmaptools — 预览画布的交互工具族（P2 D1.2/D3.x/D5.1/D7）。
//
// PreviewMapToolManager 持工具注册表：内建 pan / zoomIn / zoomOut / identify /
// measureLine / measureArea / profile 七件，单例互斥激活；Esc 统一回 pan
// （D1.8）。工具实例由管理器缓存复用，结果信号经管理器转发——调用方
// （视图页）只接管理器一份信号面，不碰工具生命周期。
//
// 量测口径：工程 datum-free 局部网格（米）→ QgsDistanceArea 平面测量，
// 不 setEllipsoid（设了反而引球面改正）。悬停读数节流 ≤30Hz（D6.5）。

// 距离/面积的可读化（米/公里；局部网格单位=米）。
namespace PreviewMapFormat
{
QString length( double meters );
QString area( double squareMeters );
} // namespace PreviewMapFormat

// ---------------------------------------------------------------- measure --
// 折线/多边形量测：左键加点、移动实时读数、双击结束、右键/Esc 清除。
class PreviewMeasureTool : public QgsMapTool
{
    Q_OBJECT
  public:
    explicit PreviewMeasureTool( QgsMapCanvas *canvas, bool areaMode );
    ~PreviewMeasureTool() override;

    // 当前量测点列（地图坐标；含跟随光标的预览点）。
    QVector<QgsPointXY> points() const { return m_points; }
    double currentLength() const;
    double currentArea() const;   // areaMode 且 ≥3 点（闭合）
    bool isAreaMode() const { return m_areaMode; }
    bool isFinished() const { return m_finished; }

    void canvasPressEvent( QgsMapMouseEvent *e ) override;
    void canvasMoveEvent( QgsMapMouseEvent *e ) override;
    void canvasReleaseEvent( QgsMapMouseEvent *e ) override;
    void canvasDoubleClickEvent( QgsMapMouseEvent *e ) override;
    void keyPressEvent( QKeyEvent *e ) override;

    void clear();

  signals:
    // points 含预览点；finished=true 是双击终局帧。
    void measurementChanged( const QVector<QgsPointXY> &points, double length,
                             double area, bool finished );
    void measurementCleared();
    void escapeRequested();

  private:
    void rebuildRubberBand( bool withPreview );
    // force=true 绕过节流（press/finish 是离散事件，读数必须即时）。
    void emitChanged( bool finished, bool force = false );

    bool m_areaMode = false;
    QVector<QgsPointXY> m_points;
    QgsPointXY m_previewPoint;
    bool m_hasPreview = false;
    bool m_finished = false;
    QgsRubberBand *m_band = nullptr;
    QElapsedTimer m_emitTimer;
    qint64 m_lastEmitMs = -1; // -1 = 尚未发过帧（首帧免节流）
};

// --------------------------------------------------------------- identify --
// 点选 + 框选 identify：按下记录起点，拖动 >5px 成矩形框，否则按点查。
class PreviewIdentifyTool : public QgsMapTool
{
    Q_OBJECT
  public:
    explicit PreviewIdentifyTool( QgsMapCanvas *canvas );
    ~PreviewIdentifyTool() override;

    void canvasPressEvent( QgsMapMouseEvent *e ) override;
    void canvasMoveEvent( QgsMapMouseEvent *e ) override;
    void canvasReleaseEvent( QgsMapMouseEvent *e ) override;
    void keyPressEvent( QKeyEvent *e ) override;

    void deactivate() override;

  signals:
    void identifyPointRequested( const QgsPointXY &point );
    void identifyRectRequested( const QgsRectangle &rect );
    void escapeRequested();

  private:
    QPoint m_pressPixel;
    bool m_dragging = false;
    QgsRubberBand *m_rectBand = nullptr;
};

// ---------------------------------------------------------------- profile --
// 剖面线：按下锚点 → 拖动预览 → 松开成线（累计多条，D5.4 并列对比）；
// 右键/Esc 清空全部剖面线。
class PreviewProfileTool : public QgsMapTool
{
    Q_OBJECT
  public:
    explicit PreviewProfileTool( QgsMapCanvas *canvas );
    ~PreviewProfileTool() override;

    int lineCount() const { return m_lines; }
    QList<QPair<QgsPointXY, QgsPointXY>> lines() const { return m_linesList; }

    void canvasPressEvent( QgsMapMouseEvent *e ) override;
    void canvasMoveEvent( QgsMapMouseEvent *e ) override;
    void canvasReleaseEvent( QgsMapMouseEvent *e ) override;
    void keyPressEvent( QKeyEvent *e ) override;

    void deactivate() override;

  signals:
    // p1/p2 地图坐标；totalLines 是累计线数（含本条，D5.4）。
    void profileLineDrawn( const QgsPointXY &p1, const QgsPointXY &p2, int totalLines );
    void profileLinesCleared();
    void escapeRequested();

  private:
    void clearAllBands();

    bool m_anchorValid = false;
    QPoint m_pressPixel;
    QgsPointXY m_anchor;
    QgsPointXY m_current;
    QgsRubberBand *m_activeBand = nullptr;  // 拖动中的预览线
    QgsRubberBand *m_doneBand = nullptr;    // 已落定的累计线
    int m_lines = 0;
    QList<QPair<QgsPointXY, QgsPointXY>> m_linesList;
};

// --------------------------------------------------------------- registry --
class PreviewMapToolManager : public QObject
{
    Q_OBJECT
  public:
    explicit PreviewMapToolManager( QgsMapCanvas *canvas, QObject *parent = nullptr );
    ~PreviewMapToolManager() override;

    // 内建工具 id 常量。
    static const QString kPan;
    static const QString kZoomIn;
    static const QString kZoomOut;
    static const QString kIdentify;
    static const QString kMeasureLine;
    static const QString kMeasureArea;
    static const QString kProfile;

    QStringList toolIds() const;
    QString toolDisplayName( const QString &id ) const;

    // 单例互斥激活（重复激活同一工具为幂等）。
    bool activate( const QString &id );
    // Esc 语义：卸下当前工具回 pan。
    void cancelToDefault();
    QString activeToolId() const;
    QgsMapTool *tool( const QString &id ) const;
    QgsMapCanvas *canvas() const { return m_canvas; }

  signals:
    void toolActivated( const QString &id );
    void toolDeactivated( const QString &id );
    // ---- 工具结果转发（见各工具类）----
    void identifyPointRequested( const QgsPointXY &point );
    void identifyRectRequested( const QgsRectangle &rect );
    void measurementChanged( const QVector<QgsPointXY> &points, double length,
                             double area, bool finished );
    void measurementCleared();
    void profileLineDrawn( const QgsPointXY &p1, const QgsPointXY &p2, int totalLines );
    void profileLinesCleared();

  private:
    void registerBuiltins();

    QgsMapCanvas *m_canvas = nullptr;
    QHash<QString, QgsMapTool *> m_tools;
    QHash<QString, QString> m_displayNames;
    QString m_activeId;
    QHash<QString, QMetaObject::Connection> m_toolSignalHooks; // 断线用（析构/防重）
};
