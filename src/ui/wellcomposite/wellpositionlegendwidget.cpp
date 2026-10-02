// 层：视图
#include "wellpositionlegendwidget.h"
#include "wellcompositetrack.h"
#include "../paleotheme.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QTableWidget>
#include <QTabWidget>
#include <QVBoxLayout>
#include <cmath>

namespace WellComposite
{

// ----------------------------------------------------------------------------
// GraphicScaleBar: 物理线段比例尺图例
// ----------------------------------------------------------------------------
GraphicScaleBar::GraphicScaleBar(QWidget *parent)
  : QWidget(parent)
{
  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  setFixedWidth(72);
  setFixedHeight(24);
  setToolTip(tr("物理标尺：根据当前测深缩放倍率计算屏幕真实对应米数"));
}

void GraphicScaleBar::setScaleMetrics(double pxPerMeter, const QString &scaleRatioStr)
{
  m_pxPerMeter = qMax(0.001, pxPerMeter);
  m_scaleRatio = scaleRatioStr;

  // 寻找合适的标尺整米步长（使得屏幕长度在 30px ~ 65px 之间）
  static const QVector<double> candidateMeters = {
      0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 25.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0};

  m_segmentMeters = 10.0;
  for (double c : candidateMeters)
  {
    if (c * m_pxPerMeter >= 30.0)
    {
      m_segmentMeters = c;
      break;
    }
  }

  m_pixelLength = m_segmentMeters * m_pxPerMeter;
  update();
}

void GraphicScaleBar::paintEvent(QPaintEvent * /*event*/)
{
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setRenderHint(QPainter::TextAntialiasing, true);

  const qreal w = width();
  const qreal h = height();

  const qreal barW = qBound(25.0, m_pixelLength, w - 8.0);
  const qreal startX = 3.0;
  const qreal barH = 4.0;
  const qreal barY = h - 8.0;

  // 文字字体 (JetBrains Mono 8pt)
  QFont font = p.font();
  font.setFamilies({QStringLiteral("JetBrains Mono"), QStringLiteral("monospace")});
  font.setStyleHint(QFont::TypeWriter);
  font.setPointSize(8);
  p.setFont(font);

  // 刻度标注文字 (0, mid, max m)
  p.setPen(QColor(QStringLiteral("#5D6E80")));
  p.drawText(QRectF(startX - 8, 0, 16, barY - 1), Qt::AlignCenter, QStringLiteral("0"));

  const double halfMeters = m_segmentMeters * 0.5;
  const QString midStr = (std::fmod(halfMeters, 1.0) == 0.0)
                             ? QString::number(halfMeters, 'f', 0)
                             : QString::number(halfMeters, 'f', 1);
  p.drawText(QRectF(startX + barW * 0.5 - 15, 0, 30, barY - 1), Qt::AlignCenter, midStr);

  const QString maxStr = QStringLiteral("%1m").arg(
      (std::fmod(m_segmentMeters, 1.0) == 0.0) ? QString::number(m_segmentMeters, 'f', 0)
                                               : QString::number(m_segmentMeters, 'f', 1));
  p.drawText(QRectF(startX + barW - 15, 0, 30, barY - 1), Qt::AlignCenter, maxStr);

  // 标尺条分为左右两段黑白相间
  const qreal midX = startX + barW * 0.5;

  // 左半段：深灰黑底
  p.fillRect(QRectF(startX, barY, barW * 0.5, barH), QColor(QStringLiteral("#24303E")));
  // 右半段：白底
  p.fillRect(QRectF(midX, barY, barW * 0.5, barH), QColor(QStringLiteral("#FFFFFF")));

  // 边框与刻度齿
  p.setPen(QPen(QColor(QStringLiteral("#24303E")), 1.0));
  p.drawRect(QRectF(startX, barY, barW, barH));
  p.drawLine(QPointF(startX, barY - 2), QPointF(startX, barY));
  p.drawLine(QPointF(midX, barY - 2), QPointF(midX, barY));
  p.drawLine(QPointF(startX + barW, barY - 2), QPointF(startX + barW, barY));
}

// ----------------------------------------------------------------------------
// WellOverviewMiniBar: 全井位置微缩示意图例 / 导航条
// ----------------------------------------------------------------------------
WellOverviewMiniBar::WellOverviewMiniBar(QWidget *parent)
  : QWidget(parent)
{
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  setFixedHeight(18);
  setCursor(Qt::PointingHandCursor);
  setToolTip(tr("全井微缩示意图例：蓝色方框指示当前视口位置，点击或拖拽可快速定位井深"));
}

void WellOverviewMiniBar::setDepthRange(double minDepth, double maxDepth)
{
  m_minDepth = minDepth;
  m_maxDepth = (maxDepth > minDepth) ? maxDepth : minDepth + 100.0;
  update();
}

void WellOverviewMiniBar::setVisibleRange(double topDepth, double bottomDepth)
{
  m_topDepth = topDepth;
  m_bottomDepth = bottomDepth;
  update();
}

void WellOverviewMiniBar::setFormations(const QVector<FormationInterval> &formations)
{
  m_formations = formations;
  update();
}

void WellOverviewMiniBar::handleMouseAt(const QPoint &pos)
{
  const double span = m_maxDepth - m_minDepth;
  if (span <= 1e-4 || width() <= 0) return;

  const double pct = qBound(0.0, static_cast<double>(pos.x()) / static_cast<double>(width()), 1.0);
  const double clickedDepth = m_minDepth + pct * span;
  const double viewSpan = qMax(1.0, m_bottomDepth - m_topDepth);

  // 以点击处为视口中心
  emit requestScrollDepth(clickedDepth - viewSpan * 0.5);
}

void WellOverviewMiniBar::mousePressEvent(QMouseEvent *event)
{
  if (event->button() == Qt::LeftButton)
  {
    m_dragging = true;
    handleMouseAt(event->pos());
    event->accept();
    return;
  }
  QWidget::mousePressEvent(event);
}

void WellOverviewMiniBar::mouseMoveEvent(QMouseEvent *event)
{
  if (m_dragging)
  {
    handleMouseAt(event->pos());
    event->accept();
    return;
  }
  QWidget::mouseMoveEvent(event);
}

void WellOverviewMiniBar::mouseReleaseEvent(QMouseEvent *event)
{
  if (event->button() == Qt::LeftButton)
  {
    m_dragging = false;
    event->accept();
    return;
  }
  QWidget::mouseReleaseEvent(event);
}

void WellOverviewMiniBar::paintEvent(QPaintEvent * /*event*/)
{
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);

  const qreal w = width();
  const qreal h = height();
  const double span = qMax(1.0, m_maxDepth - m_minDepth);

  // 底色与边框（chrome 走当前主题 token——与底部图例栏同款）
  const QRectF baseRect(0.5, 0.5, w - 1.0, h - 1.0);
  const auto &tok = PaleoTheme::tokens();
  p.fillRect(baseRect, tok.surfaceAlt);
  p.setPen(tok.border);
  p.drawRoundedRect(baseRect, 3.0, 3.0);

  // 绘制地层分段色块
  for (const auto &f : m_formations)
  {
    const double fTop = qBound(m_minDepth, f.topDepth, m_maxDepth);
    const double fBottom = qBound(m_minDepth, f.bottomDepth, m_maxDepth);
    if (fBottom <= fTop) continue;

    const qreal x1 = (fTop - m_minDepth) / span * w;
    const qreal x2 = (fBottom - m_minDepth) / span * w;
    const qreal segW = qMax(1.0, x2 - x1);

    p.fillRect(QRectF(x1, 1.0, segW, h - 2.0), f.color);
  }

  // 绘制当前视口滑块（高亮微缩指示）
  const double vTop = qBound(m_minDepth, m_topDepth, m_maxDepth);
  const double vBottom = qBound(m_minDepth, m_bottomDepth, m_maxDepth);
  const qreal vx1 = (vTop - m_minDepth) / span * w;
  const qreal vx2 = (vBottom - m_minDepth) / span * w;
  const qreal vW = qMax(4.0, vx2 - vx1);

  const QRectF viewportRect(vx1, 1.0, vW, h - 2.0);
  // 视口滑块 = 选中/交互指示（primary 合法用途；两主题同值）
  QColor sel = tok.primary;
  sel.setAlpha(80);
  p.fillRect(viewportRect, sel);
  p.setPen(QPen(tok.primary, 1.5));
  p.drawRect(viewportRect);

  // 中间小抓手指示线
  if (vW >= 10.0)
  {
    const qreal midX = vx1 + vW * 0.5;
    p.setPen(QPen(tok.primary, 1.0));
    p.drawLine(QPointF(midX - 1.0, 2), QPointF(midX - 1.0, h - 2));
    p.drawLine(QPointF(midX + 1.0, 2), QPointF(midX + 1.0, h - 2));
  }
}

void WellOverviewMiniBar::changeEvent(QEvent *event)
{
  if (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::StyleChange)
    update();
  QWidget::changeEvent(event);
}

// ----------------------------------------------------------------------------
// WellLegendDialog: 地质与道图例弹窗
// ----------------------------------------------------------------------------
WellLegendDialog::WellLegendDialog(const ComprehensiveWellData &wellData, QWidget *parent)
  : QDialog(parent)
{
  setWindowTitle(tr("综合柱状图 — 地质与道图例"));
  resize(620, 480);
  // chrome 跟随主题；普通页签选中态用深字+bold，不占用 primary 交互蓝
  PaleoTheme::applyThemedStyleSheet(this, [] {
    const auto &t = PaleoTheme::tokens();
    return QStringLiteral(
               "QDialog { background: %1; }"
               "QTabWidget::pane { border: 1px solid %2; background: %1; border-radius: 4px; }"
               "QTabBar::tab { background: %3; color: %4; padding: 6px 16px; margin-right: 2px; border-top-left-radius: 4px; border-top-right-radius: 4px; font-size: 9pt; }"
               "QTabBar::tab:selected { background: %1; color: %5; font-weight: bold; border: 1px solid %2; border-bottom: none; }"
               "QTableWidget { border: none; background: %1; gridline-color: %3; }"
               "QHeaderView::section { background: %3; color: %5; font-weight: 500; border: none; padding: 4px; }")
        .arg(t.surface.name(), t.border.name(), t.surfaceAlt.name(),
             t.textMuted.name(), t.text.name());
  });
  setupUi(wellData);
}

void WellLegendDialog::setupUi(const ComprehensiveWellData &data)
{
  auto *rootLay = new QVBoxLayout(this);
  rootLay->setContentsMargins(16, 16, 16, 16);
  rootLay->setSpacing(8);

  auto *tabWidget = new QTabWidget(this);

  // --- 1. 岩性花纹图例页 ---
  auto *lithPage = new QWidget(tabWidget);
  auto *lithLay = new QVBoxLayout(lithPage);
  lithLay->setContentsMargins(8, 8, 8, 8);

  auto *lithTable = new QTableWidget(lithPage);
  lithTable->setColumnCount(3);
  lithTable->setHorizontalHeaderLabels({tr("图例花纹"), tr("岩性名称"), tr("地质说明")});
  lithTable->horizontalHeader()->setStretchLastSection(true);
  lithTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
  lithTable->setColumnWidth(0, 100);
  lithTable->setColumnWidth(1, 120);
  lithTable->verticalHeader()->setVisible(false);

  struct LithDef {
    QString name;
    QString desc;
  };
  static const QVector<LithDef> lithDefs = {
      {QStringLiteral("细砂岩"), tr("含油气储层主要赋存骨架岩性")},
      {QStringLiteral("中砂岩"), tr("中等颗粒碎屑岩储集层")},
      {QStringLiteral("粗砂岩"), tr("高孔高渗粗粒碎屑岩储层")},
      {QStringLiteral("砾岩"), tr("近源快速沉积扇体骨架")},
      {QStringLiteral("泥岩"), tr("主力生烃与区域优质泥岩盖层")},
      {QStringLiteral("粉砂岩"), tr("过渡相薄互层砂质泥岩微相")},
      {QStringLiteral("灰岩"), tr("海相碳酸盐岩储集体")},
      {QStringLiteral("白云岩"), tr("晶粒/残余粒屑白云岩储层")},
      {QStringLiteral("页岩"), tr("富有机质致密页岩油气层")},
      {QStringLiteral("煤层"), tr("含煤地层标志层与烃源岩")},
      {QStringLiteral("火成岩"), tr("基底或火成岩裂缝储层")},
      {QStringLiteral("膏盐岩"), tr("强封闭性区域盐膏质蒸发盖层")},
  };

  lithTable->setRowCount(lithDefs.size());
  for (int r = 0; r < lithDefs.size(); ++r)
  {
    const auto &def = lithDefs[r];
    // 图例花纹预览图像
    QImage swatch(80, 24, QImage::Format_ARGB32_Premultiplied);
    swatch.fill(QColor(QStringLiteral("#FFFFFF")));
    QPainter sp(&swatch);
    const QBrush brush = LithologyPatternFactory::getBrush(def.name);
    sp.fillRect(QRectF(0, 0, 80, 24), brush);
    sp.setPen(QColor(QStringLiteral("#DFE5EC")));
    sp.drawRect(QRectF(0, 0, 79, 23));
    sp.end();

    auto *lblImg = new QLabel(lithTable);
    lblImg->setPixmap(QPixmap::fromImage(swatch));
    lblImg->setAlignment(Qt::AlignCenter);
    lithTable->setCellWidget(r, 0, lblImg);

    auto *item1 = new QTableWidgetItem(def.name);
    item1->setTextAlignment(Qt::AlignCenter);
    lithTable->setItem(r, 1, item1);

    auto *item2 = new QTableWidgetItem(def.desc);
    lithTable->setItem(r, 2, item2);
    lithTable->setRowHeight(r, 28);
  }
  lithLay->addWidget(lithTable);
  tabWidget->addTab(lithPage, tr("岩性花纹图例"));

  // --- 2. 沉积相纹理图例页 ---
  auto *faciesPage = new QWidget(tabWidget);
  auto *faciesLay = new QVBoxLayout(faciesPage);
  faciesLay->setContentsMargins(8, 8, 8, 8);

  auto *faciesTable = new QTableWidget(faciesPage);
  faciesTable->setColumnCount(4);
  faciesTable->setHorizontalHeaderLabels({tr("相纹理"), tr("沉积微相"), tr("所属亚相/大相"), tr("水动力特征及环境说明")});
  faciesTable->horizontalHeader()->setStretchLastSection(true);
  faciesTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
  faciesTable->setColumnWidth(0, 100);
  faciesTable->setColumnWidth(1, 110);
  faciesTable->setColumnWidth(2, 130);
  faciesTable->verticalHeader()->setVisible(false);

  struct FaciesDef {
    QString patType;
    QString name;
    QString parentFacies;
    QString desc;
  };
  static const QVector<FaciesDef> faciesDefs = {
      {QStringLiteral("distributary_channel"), QStringLiteral("水下分流河道"), QStringLiteral("三角洲前缘"), tr("高能牵引流前积分流砂体，优质骨架储层")},
      {QStringLiteral("mouth_bar"), QStringLiteral("河口坝"), QStringLiteral("三角洲前缘"), tr("入水波浪顶托减速沉降微相，双凸镜状厚砂体")},
      {QStringLiteral("sheet_sand"), QStringLiteral("席状砂"), QStringLiteral("三角洲前缘"), tr("波浪淘洗再沉积薄层展布砂，连通性好")},
      {QStringLiteral("interdistributary_bay"), QStringLiteral("分流间湾"), QStringLiteral("三角洲前缘/平原"), tr("分流河道间静水弱水动力泥质充填沉积")},
      {QStringLiteral("delta_front"), QStringLiteral("三角洲前缘"), QStringLiteral("三角洲相"), tr("砂泥薄互层，斜向交错前积结构")},
      {QStringLiteral("delta_plain"), QStringLiteral("分流平原"), QStringLiteral("三角洲相"), tr("近源河流相及陆上分流平原，发育炭质根迹")},
      {QStringLiteral("prodelta"), QStringLiteral("前三角洲泥"), QStringLiteral("三角洲相"), tr("低能深水悬浮沉降厚层暗色泥岩，优良区域盖层/源岩")},
      {QStringLiteral("shallow_marine"), QStringLiteral("浅海陆棚/礁滩"), QStringLiteral("浅海/海相台地"), tr("浪基面附近浅海波状沉积或碳酸盐台地边缘生物滩")},
      {QStringLiteral("turbidite"), QStringLiteral("浊积砂体"), QStringLiteral("半深湖/重力流"), tr("深水重力流侵蚀搬运递变粒序砂体，潜在隐蔽油气藏")},
      {QStringLiteral("channel_lag"), QStringLiteral("滞留沉积"), QStringLiteral("河流/分流河道"), tr("河床底部高能冲刷滞留砾石及粗粒残积物")},
      {QStringLiteral("tidal_flat"), QStringLiteral("潮坪微相"), QStringLiteral("海陆过渡相"), tr("双向潮汐流往复作用下形成之人字形层理砂泥互层")},
  };

  faciesTable->setRowCount(faciesDefs.size());
  for (int r = 0; r < faciesDefs.size(); ++r)
  {
    const auto &def = faciesDefs[r];
    QImage swatch(80, 24, QImage::Format_ARGB32_Premultiplied);
    swatch.fill(QColor(QStringLiteral("#FFFFFF")));
    QPainter sp(&swatch);
    const QBrush brush = FaciesPatternFactory::getBrush(def.patType);
    sp.fillRect(QRectF(0, 0, 80, 24), brush);
    sp.setPen(QColor(QStringLiteral("#DFE5EC")));
    sp.drawRect(QRectF(0, 0, 79, 23));
    sp.end();

    auto *lblImg = new QLabel(faciesTable);
    lblImg->setPixmap(QPixmap::fromImage(swatch));
    lblImg->setAlignment(Qt::AlignCenter);
    faciesTable->setCellWidget(r, 0, lblImg);

    auto *item1 = new QTableWidgetItem(def.name);
    item1->setTextAlignment(Qt::AlignCenter);
    faciesTable->setItem(r, 1, item1);

    auto *item2 = new QTableWidgetItem(def.parentFacies);
    item2->setTextAlignment(Qt::AlignCenter);
    faciesTable->setItem(r, 2, item2);

    auto *item3 = new QTableWidgetItem(def.desc);
    faciesTable->setItem(r, 3, item3);
    faciesTable->setRowHeight(r, 28);
  }
  faciesLay->addWidget(faciesTable);
  tabWidget->addTab(faciesPage, tr("沉积相纹理图例"));

  // --- 3. 地层分层图例页 ---
  auto *formPage = new QWidget(tabWidget);
  auto *formLay = new QVBoxLayout(formPage);
  formLay->setContentsMargins(8, 8, 8, 8);

  auto *formTable = new QTableWidget(formPage);
  formTable->setColumnCount(4);
  formTable->setHorizontalHeaderLabels({tr("色标"), tr("地层单元"), tr("顶深 (m)"), tr("底深 (m)")});
  formTable->horizontalHeader()->setStretchLastSection(true);
  formTable->verticalHeader()->setVisible(false);

  formTable->setRowCount(data.formationIntervals.size());
  for (int r = 0; r < data.formationIntervals.size(); ++r)
  {
    const auto &f = data.formationIntervals[r];
    QImage colImg(40, 18, QImage::Format_ARGB32_Premultiplied);
    colImg.fill(f.color);
    auto *lblCol = new QLabel(formTable);
    lblCol->setPixmap(QPixmap::fromImage(colImg));
    lblCol->setAlignment(Qt::AlignCenter);
    formTable->setCellWidget(r, 0, lblCol);

    auto *itemN = new QTableWidgetItem(f.name);
    itemN->setTextAlignment(Qt::AlignCenter);
    formTable->setItem(r, 1, itemN);

    auto *itemT = new QTableWidgetItem(QString::number(f.topDepth, 'f', 1));
    itemT->setTextAlignment(Qt::AlignCenter);
    formTable->setItem(r, 2, itemT);

    auto *itemB = new QTableWidgetItem(QString::number(f.bottomDepth, 'f', 1));
    itemB->setTextAlignment(Qt::AlignCenter);
    formTable->setItem(r, 3, itemB);
    formTable->setRowHeight(r, 26);
  }
  formLay->addWidget(formTable);
  tabWidget->addTab(formPage, tr("地层分层图例"));

  // --- 3. 测井曲线样式页 ---
  auto *curvePage = new QWidget(tabWidget);
  auto *curveLay = new QVBoxLayout(curvePage);
  curveLay->setContentsMargins(8, 8, 8, 8);

  auto *curveTable = new QTableWidget(curvePage);
  curveTable->setColumnCount(5);
  curveTable->setHorizontalHeaderLabels({tr("曲线"), tr("单位"), tr("最小值"), tr("最大值"), tr("样式")});
  curveTable->horizontalHeader()->setStretchLastSection(true);
  curveTable->verticalHeader()->setVisible(false);

  curveTable->setRowCount(data.continuousCurves.size());
  for (int r = 0; r < data.continuousCurves.size(); ++r)
  {
    const auto &c = data.continuousCurves[r];
    auto *iN = new QTableWidgetItem(c.name);
    iN->setTextAlignment(Qt::AlignCenter);
    curveTable->setItem(r, 0, iN);

    auto *iU = new QTableWidgetItem(c.unit.isEmpty() ? QStringLiteral("—") : c.unit);
    iU->setTextAlignment(Qt::AlignCenter);
    curveTable->setItem(r, 1, iU);

    auto *iMin = new QTableWidgetItem(QString::number(c.minScale, 'f', 1));
    iMin->setTextAlignment(Qt::AlignCenter);
    curveTable->setItem(r, 2, iMin);

    auto *iMax = new QTableWidgetItem(QString::number(c.maxScale, 'f', 1));
    iMax->setTextAlignment(Qt::AlignCenter);
    curveTable->setItem(r, 3, iMax);

    // 绘制曲线色条
    QImage lineImg(60, 16, QImage::Format_ARGB32_Premultiplied);
    lineImg.fill(Qt::white);
    QPainter lp(&lineImg);
    lp.setRenderHint(QPainter::Antialiasing, true);
    lp.setPen(QPen(c.color, 2.0));
    lp.drawLine(QPointF(4, 8), QPointF(56, 8));
    lp.end();
    auto *lblLine = new QLabel(curveTable);
    lblLine->setPixmap(QPixmap::fromImage(lineImg));
    lblLine->setAlignment(Qt::AlignCenter);
    curveTable->setCellWidget(r, 4, lblLine);

    curveTable->setRowHeight(r, 26);
  }
  curveLay->addWidget(curveTable);
  tabWidget->addTab(curvePage, tr("测井曲线样式"));

  // --- 4. 解释符号图例页 ---
  auto *symPage = new QWidget(tabWidget);
  auto *symLay = new QVBoxLayout(symPage);
  symLay->setContentsMargins(8, 8, 8, 8);

  auto *symTable = new QTableWidget(symPage);
  symTable->setColumnCount(3);
  symTable->setHorizontalHeaderLabels({tr("符号色标"), tr("解释结论"), tr("水动力及储层属性")});
  symTable->horizontalHeader()->setStretchLastSection(true);
  symTable->verticalHeader()->setVisible(false);

  struct SymDef {
    QString name;
    QString desc;
    QColor color;
  };
  static const QVector<SymDef> symDefs = {
      {tr("油层"), tr("高含油饱和度工业油流层段"), QColor(QStringLiteral("#D32F2F"))},
      {tr("差油层"), tr("低丰度/弱显示含油储集层"), QColor(QStringLiteral("#FF7043"))},
      {tr("气层"), tr("高电阻强声波异常天然气层"), QColor(QStringLiteral("#F57C00"))},
      {tr("油水同层"), tr("过渡带油水混合流体产出层"), QColor(QStringLiteral("#7B1FA2"))},
      {tr("含水层"), tr("高水淹低电阻纯水层"), QColor(QStringLiteral("#1976D2"))},
      {tr("干层"), tr("物性极差致密未充注层段"), QColor(QStringLiteral("#78909C"))},
  };
  symTable->setRowCount(symDefs.size());
  for (int r = 0; r < symDefs.size(); ++r)
  {
    const auto &s = symDefs[r];
    QImage sImg(40, 18, QImage::Format_ARGB32_Premultiplied);
    sImg.fill(s.color);
    auto *lblS = new QLabel(symTable);
    lblS->setPixmap(QPixmap::fromImage(sImg));
    lblS->setAlignment(Qt::AlignCenter);
    symTable->setCellWidget(r, 0, lblS);

    auto *iN = new QTableWidgetItem(s.name);
    iN->setTextAlignment(Qt::AlignCenter);
    symTable->setItem(r, 1, iN);

    auto *iD = new QTableWidgetItem(s.desc);
    symTable->setItem(r, 2, iD);
    symTable->setRowHeight(r, 26);
  }
  symLay->addWidget(symTable);
  tabWidget->addTab(symPage, tr("储层解释符号"));

  rootLay->addWidget(tabWidget);

  // 底部关闭按钮
  auto *btnBox = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(btnBox, &QDialogButtonBox::rejected, this, &QDialog::accept);
  rootLay->addWidget(btnBox);
}

// ----------------------------------------------------------------------------
// WellPositionLegendWidget: 位置显示图例综合控件
// ----------------------------------------------------------------------------
WellPositionLegendWidget::WellPositionLegendWidget(QWidget *parent)
  : QWidget(parent)
{
  setObjectName(QStringLiteral("wellPositionLegendWidget"));
  setFixedHeight(32);
  // 底部栏 chrome 跟随主题；导航按钮用正文 text 色，不占用 primary 交互蓝
  PaleoTheme::applyThemedStyleSheet(this, [] {
    const auto &t = PaleoTheme::tokens();
    return QStringLiteral(
               "#wellPositionLegendWidget { background: %1; border-top: 1px solid %2; }"
               "QLabel { color: %3; font-size: 8pt; }")
        .arg(t.surface.name(), t.border.name(), t.text.name()) + PaleoTheme::toolButtonStyleSheet();
  });

  auto *mainLay = new QHBoxLayout(this);
  mainLay->setContentsMargins(6, 1, 6, 1);
  mainLay->setSpacing(6);

  // 1. 物理线段比例尺图例
  m_scaleBar = new GraphicScaleBar(this);
  mainLay->addWidget(m_scaleBar);

  // 2. 真实比例尺与屏幕实物换算标注文字
  QFont monoFont;
  monoFont.setFamilies({QStringLiteral("JetBrains Mono"), QStringLiteral("monospace")});
  monoFont.setStyleHint(QFont::TypeWriter);
  monoFont.setPointSize(8);
  monoFont.setStyleStrategy(QFont::PreferAntialias);

  m_lblScaleRatio = new QLabel(QStringLiteral("1:500 (1cm≈5m)"), this);
  m_lblScaleRatio->setObjectName(QStringLiteral("lblScaleRatio"));
  m_lblScaleRatio->setFont(monoFont);
  mainLay->addWidget(m_lblScaleRatio);

  // 分隔线
  auto *sep1 = new QFrame(this);
  sep1->setFrameShape(QFrame::VLine);
  sep1->setFrameShadow(QFrame::Sunken);
  PaleoTheme::applyThemedStyleSheet(sep1, [] {
    return QStringLiteral("color: %1;").arg(PaleoTheme::tokens().border.name());
  });
  mainLay->addWidget(sep1);

  // 3. 当前显示区域与视口范围标注
  m_lblViewportRange = new QLabel(tr("视口: 0.0 ~ 500.0m (跨度 500.0m)"), this);
  m_lblViewportRange->setObjectName(QStringLiteral("lblViewportRange"));
  m_lblViewportRange->setFont(monoFont);
  mainLay->addWidget(m_lblViewportRange);

  // 4. 全井范围与相对位置标注
  m_lblWellRange = new QLabel(tr("全井: 0.0 ~ 3000.0m (0.0%~16.7%)"), this);
  m_lblWellRange->setObjectName(QStringLiteral("lblWellRange"));
  m_lblWellRange->setFont(monoFont);
  PaleoTheme::applyThemedStyleSheet(m_lblWellRange, [] {
    return PaleoTheme::mutedCaptionStyleSheet();
  });
  mainLay->addWidget(m_lblWellRange);

  mainLay->addStretch(1);

  // 5. 全井位置微缩示意图例 / 导航条
  auto *lblNavTitle = new QLabel(tr("全井导航:"), this);
  PaleoTheme::applyThemedStyleSheet(lblNavTitle, [] {
    return PaleoTheme::mutedCaptionStyleSheet() + QStringLiteral(" font-size: 8pt;");
  });
  mainLay->addWidget(lblNavTitle);

  m_miniBar = new WellOverviewMiniBar(this);
  m_miniBar->setObjectName(QStringLiteral("wellOverviewMiniBar"));
  m_miniBar->setFixedWidth(120);
  mainLay->addWidget(m_miniBar);

  connect(m_miniBar, &WellOverviewMiniBar::requestScrollDepth, this, &WellPositionLegendWidget::requestScrollDepth);

  // 6. 悬停光标测深标注
  m_lblCursorDepth = new QLabel(tr("光标: —"), this);
  m_lblCursorDepth->setObjectName(QStringLiteral("lblCursorDepth"));
  m_lblCursorDepth->setFont(monoFont);
  m_lblCursorDepth->setMinimumWidth(80);
  mainLay->addWidget(m_lblCursorDepth);

  // 分隔线
  auto *sep2 = new QFrame(this);
  sep2->setFrameShape(QFrame::VLine);
  sep2->setFrameShadow(QFrame::Sunken);
  PaleoTheme::applyThemedStyleSheet(sep2, [] {
    return QStringLiteral("color: %1;").arg(PaleoTheme::tokens().border.name());
  });
  mainLay->addWidget(sep2);

  // 7. 地质与道图例弹窗按钮
  m_btnLegend = new QToolButton(this);
  m_btnLegend->setObjectName(QStringLiteral("btnOpenLegend"));
  m_btnLegend->setText(tr("地质图例"));
  m_btnLegend->setToolTip(tr("查看岩性花纹、地层色标、曲线样式与储层解释符号图例"));
  connect(m_btnLegend, &QToolButton::clicked, this, &WellPositionLegendWidget::openLegendDialog);
  mainLay->addWidget(m_btnLegend);
}

void WellPositionLegendWidget::updateViewport(double topDepth, double bottomDepth, double span)
{
  m_currentTopDepth = topDepth;
  m_currentBottomDepth = bottomDepth;
  m_currentSpan = span;

  // 更新当前显示区域文字
  m_lblViewportRange->setText(
      tr("视口: %1 ~ %2m (跨度 %3m)")
          .arg(QString::number(topDepth, 'f', 1))
          .arg(QString::number(bottomDepth, 'f', 1))
          .arg(QString::number(span, 'f', 1)));

  // 更新全井范围与相对位置百分比
  const double totalSpan = qMax(1.0, m_maxDepth - m_minDepth);
  const double pctStart = qBound(0.0, (topDepth - m_minDepth) / totalSpan * 100.0, 100.0);
  const double pctEnd = qBound(0.0, (bottomDepth - m_minDepth) / totalSpan * 100.0, 100.0);

  m_lblWellRange->setText(
      tr("全井: %1 ~ %2m (%3%~%4%)")
          .arg(QString::number(m_minDepth, 'f', 0))
          .arg(QString::number(m_maxDepth, 'f', 0))
          .arg(QString::number(pctStart, 'f', 1))
          .arg(QString::number(pctEnd, 'f', 1)));

  // 更新全井微缩条高亮滑块
  m_miniBar->setVisibleRange(topDepth, bottomDepth);
}

void WellPositionLegendWidget::updateScale(double pxPerMeter, const QString &scaleRatio)
{
  m_pxPerMeter = qMax(0.001, pxPerMeter);
  m_scaleRatioStr = scaleRatio;

  // 计算屏幕 1cm 真实代表的地下米数 (以 96 DPI，1cm = 37.79528 px 计)
  const double metersPerCm = 37.79528 / m_pxPerMeter;
  const QString meterStr = (metersPerCm < 1.0) ? QString::number(metersPerCm, 'f', 2)
                           : (metersPerCm < 10.0) ? QString::number(metersPerCm, 'f', 1)
                                                  : QString::number(metersPerCm, 'f', 0);

  m_lblScaleRatio->setText(QStringLiteral("%1 (1cm≈%2m)").arg(scaleRatio, meterStr));
  m_scaleBar->setScaleMetrics(pxPerMeter, scaleRatio);
}

void WellPositionLegendWidget::updateHoverDepth(double depth)
{
  if (depth >= m_minDepth && depth <= m_maxDepth)
  {
    m_lblCursorDepth->setText(tr("光标: %1m").arg(QString::number(depth, 'f', 1)));
  }
  else
  {
    m_lblCursorDepth->setText(tr("光标: —"));
  }
}

void WellPositionLegendWidget::setWellData(const ComprehensiveWellData &data)
{
  m_wellData = data;
  setDepthRange(data.minDepth, data.maxDepth);
  m_miniBar->setFormations(data.formationIntervals);
}

void WellPositionLegendWidget::setDepthRange(double minDepth, double maxDepth)
{
  m_minDepth = minDepth;
  m_maxDepth = (maxDepth > minDepth) ? maxDepth : minDepth + 100.0;
  m_miniBar->setDepthRange(m_minDepth, m_maxDepth);
  updateViewport(m_currentTopDepth, m_currentBottomDepth, m_currentSpan);
}

void WellPositionLegendWidget::openLegendDialog()
{
  WellLegendDialog dlg(m_wellData, this);
  dlg.exec();
}

} // namespace WellComposite