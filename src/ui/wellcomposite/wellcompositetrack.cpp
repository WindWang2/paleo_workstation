// 层：视图
// token 例外：DESIGN 地质文档纸面例外：岩性/地层/曲线域配色、纸面纹理与数据标注，保持导出一致；标准纸面中性色已转 Light token。（tools/ui-token-exceptions.json 精确计数）。
#include "ui/paleotheme.h"
#include "wellcompositetrack.h"

namespace WellComposite
{

// ----------------------------------------------------------------------------
// WellTrack 公共道头三行区（D1.11）：标题 | 刻度 | 单位，随道宽自适应截断
// ----------------------------------------------------------------------------
void WellTrack::paintHeaderChrome(QPainter &painter, const QRectF &headerRect,
                                  const QString &scaleText, const QString &unitText) const
{
  painter.save();
  painter.setClipRect(headerRect);

  painter.fillRect(headerRect, PaleoTheme::tokens(PaleoTheme::Theme::Light).surfaceAlt);
  painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).border);
  painter.drawLine(headerRect.topRight(), headerRect.bottomRight());
  painter.drawLine(headerRect.bottomLeft(), headerRect.bottomRight());

  const bool hasScale = !scaleText.isEmpty();
  const bool hasUnit = !unitText.isEmpty();

  // 三行布局：标题 26px 置顶，刻度/单位行按存在性均分余高；无副行时标题纵向居中
  const qreal titleBandH = hasScale || hasUnit ? qMin<qreal>(28.0, headerRect.height() * 0.45)
                                               : headerRect.height();
  const QRectF titleBand(headerRect.left() + 2, headerRect.top() + 4,
                         headerRect.width() - 4, titleBandH - 6);
  painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).text);
  QFont fTitle = painter.font();
  fTitle.setPointSize(PaleoTheme::tokens().bodyPt);
  fTitle.setBold(true);
  painter.setFont(fTitle);
  painter.drawText(titleBand, Qt::AlignCenter | Qt::TextWrapAnywhere,
                   elideTitle(painter, title(), titleBand.width()));

  QFont fSub = painter.font();
  fSub.setPointSize(PaleoTheme::tokens().labelPt);
  fSub.setBold(false);
  painter.setFont(fSub);

  qreal rowTop = headerRect.top() + titleBandH;
  const qreal rowH = (headerRect.height() - titleBandH) / (hasScale + hasUnit ? (hasScale && hasUnit ? 2.0 : 1.0) : 1.0);
  const QFontMetrics fm(fSub);
  const auto drawRow = [&](const QString &raw, bool isUnit) {
    if (raw.isEmpty()) return;
    const QRectF rowRect(headerRect.left() + 3, rowTop, headerRect.width() - 6, rowH);
    painter.setPen(QColor(isUnit ? QStringLiteral("#5D6E80") : QStringLiteral("#455A64")));
    painter.drawText(rowRect, Qt::AlignCenter,
                     fm.elidedText(raw, Qt::ElideRight, static_cast<int>(rowRect.width())));
    rowTop += rowH;
  };
  drawRow(scaleText, false);
  drawRow(unitText, true);

  painter.restore();
}

QString WellTrack::elideTitle(const QPainter &painter, const QString &text, qreal widthPx) const
{
  const QFontMetrics fm(painter.font());
  if (fm.horizontalAdvance(text) <= widthPx)
    return text;
  // 两行机会：先按最长行宽折半截断（竖排语义由窄道自适配合并到一行截断）
  return fm.elidedText(text, Qt::ElideMiddle, static_cast<int>(widthPx));
}

} // namespace WellComposite
