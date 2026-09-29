// 层：视图
#pragma once

#include <QList>
#include <QObject>
#include <QVariantList>
#include <memory>

#include <qgsmapdecoration.h>

class QgsMapCanvas;
class QPainter;

// ui/decorations/ — canvas decorations per ET9 audit.
//
// Upstream QgsDecorationItem + concrete decorations (scale bar, north arrow,
// grid, copyright, title, image) live in src/app/decorations and are
// APP_EXPORT — none of their headers are installed, and QGIS 4.x removed
// QgsMapCanvas::addDecorationItem. The only decoration surface available to
// embedders is the core QgsMapDecoration interface
// (src/core/qgsmapdecoration.h) plus QgsMapCanvas::renderComplete(QPainter*),
// which is exactly the hook libqgis_app's QgsDecorationItem uses.
//
// So: PaleoDecorationManager owns small bespoke QgsMapDecoration subclasses
// and paints the enabled ones from renderComplete — the same contract, no
// libqgis_app link required.

// Minimal scale-bar overlay: a rounded-to-1/2/5 bar anchored bottom-left with
// a "<distance> <unit>" label, sized off mapUnitsPerPixel.
class PaleoScaleBarDecoration : public QgsMapDecoration
{
  public:
    PaleoScaleBarDecoration() { setDisplayName( QStringLiteral( "Scale Bar" ) ); }
    void render( const QgsMapSettings &mapSettings, QgsRenderContext &context ) override;
};

// Minimal north arrow: small two-tone arrow + "N" anchored top-left
// （DESIGN.md：指北针左上）, rotated by -mapSettings.rotation().
class PaleoNorthArrowDecoration : public QgsMapDecoration
{
  public:
    PaleoNorthArrowDecoration() { setDisplayName( QStringLiteral( "North Arrow" ) ); }
    void render( const QgsMapSettings &mapSettings, QgsRenderContext &context ) override;
};

// Minimal graticule: light grid lines at rounded 1/2/5 map-unit intervals
// with edge tick labels. Anchored to the map, not the viewport.
class PaleoGridDecoration : public QgsMapDecoration
{
  public:
    PaleoGridDecoration() { setDisplayName( QStringLiteral( "Grid" ) ); }
    bool hasFixedMapPosition() const override { return true; }
    void render( const QgsMapSettings &mapSettings, QgsRenderContext &context ) override;
};

// D11 临时配准水印：手工仿射的 GeoJSON 上图期间，画布顶中压一条半透明
// 「临时配准 · 手工仿射」胶囊（DESIGN warning 底 + 深色文字——白字在橙底上
// 不足 AA）——与正式图层做视觉隔离，不把临时配准读成权威数据。
class PaleoWatermarkDecoration : public QgsMapDecoration
{
  public:
    PaleoWatermarkDecoration() { setDisplayName( QStringLiteral( "Watermark" ) ); }
    void setText( const QString &text ) { mText = text; }
    QString text() const { return mText; }
    void render( const QgsMapSettings &mapSettings, QgsRenderContext &context ) override;
  private:
    QString mText; // 默认文案由 PaleoDecorationManager 构造时以 translate 落（见 .cpp）
};

class PaleoFaciesLegendDecoration : public QgsMapDecoration
{
public:
  QString title;
  QVariantList facies;
  void render(const QgsMapSettings &, QgsRenderContext &context) override;
};

class QWidget;

// D1.10 装饰件主题 token（DESIGN.md 双主题）：画布本体按纸面白底不动
//（数据符号不随暗色——wellcomposite 同口径），装饰件的卡片/描线/文字色
// 跟随 UI 主题。浅色 = 既有硬编码色；暗色取 DESIGN dark 块 token。
namespace PaleoDecorationTheme
{
  void setDark( bool dark );
  bool isDark();
  QColor card();            // 半透明承载卡片底
  QColor ink();             // 主描线/文字
  QColor inkSoft();         // 次级网格线
  QColor border();          // 卡片描边
  QColor arrowDarkHalf();   // 指北针深色半翼
  QColor arrowLightHalf();  // 指北针浅色半翼
}

// Toggles decoration overlays on a QgsMapCanvas. Parented to the canvas by
// default; paintDecorations() is the slot wired to renderComplete and is also
// directly callable for offscreen paint checks.
class PaleoDecorationManager : public QObject
{
    Q_OBJECT
  public:
    explicit PaleoDecorationManager( QgsMapCanvas *canvas, QObject *parent = nullptr );

    void setFaciesLegend(const QString &title, const QVariantList &facies);
    QString legendTitle() const { return mLegend->title; }
    QVariantList legendFacies() const { return mLegend->facies; }
    void setScaleBarEnabled( bool enabled );
    void setNorthArrowEnabled( bool enabled );
    void setGridEnabled( bool enabled );
    void setWatermarkEnabled( bool enabled );
    void setWatermarkText( const QString &text );

    bool isScaleBarEnabled() const { return mScaleBarEnabled; }
    bool isNorthArrowEnabled() const { return mNorthArrowEnabled; }
    bool isGridEnabled() const { return mGridEnabled; }
    bool isWatermarkEnabled() const { return mWatermarkEnabled; }

    // Currently-enabled decorations, in paint order.
    QList<QgsMapDecoration *> decorationItems() const;

    QgsMapCanvas *canvas() const { return mCanvas; }

  public slots:
    // Paints all enabled decorations. Connected to the canvas's
    // renderComplete signal; safe to call with any painter for tests.
    void paintDecorations( QPainter *painter );

  private:
    QgsMapCanvas *mCanvas = nullptr; // not owned; also QObject parent
    QWidget *mOverlay = nullptr;

    // Owned decoration instances; enabled flags gate which are painted.
    std::unique_ptr<PaleoFaciesLegendDecoration> mLegend;
    bool mLegendEnabled = false;
    std::unique_ptr<PaleoScaleBarDecoration> mScaleBar;
    std::unique_ptr<PaleoNorthArrowDecoration> mNorthArrow;
    std::unique_ptr<PaleoGridDecoration> mGrid;
    std::unique_ptr<PaleoWatermarkDecoration> mWatermark;

    bool mScaleBarEnabled = false;
    bool mNorthArrowEnabled = false;
    bool mGridEnabled = false;
    bool mWatermarkEnabled = false;
};
