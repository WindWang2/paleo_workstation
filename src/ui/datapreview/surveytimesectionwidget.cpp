// 层：视图
// token 例外：DESIGN 数据符号例外：地震振幅变密度色标（红白蓝），不进 UI
// token，浅深主题同值（与地震剖面八色标同族）。
#include "surveytimesectionwidget.h"
#include "../paleotheme.h"
#include <QImage>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <vector>

SurveyTimeSectionWidget::SurveyTimeSectionWidget(QWidget *parent) : QWidget(parent)
{
  setObjectName(QStringLiteral("surveyTimeSection"));
  setMinimumHeight(150);
  PaleoTheme::applyThemedStyleSheet(
      this,
      []
      {
        return PaleoTheme::metricStyleSheet(
                   QStringLiteral("background: %1; border: 1px solid %2; border-radius: {rounded.sm}px;"))
            .arg(PaleoTheme::tokens().surface.name(), PaleoTheme::tokens().border.name());
      });
}

SurveyTimeSectionWidget::~SurveyTimeSectionWidget() { delete m_image; }

void SurveyTimeSectionWidget::showLoading(const QString &hint)
{
  m_hint = hint;
  m_error.clear();
  m_preview = SurveyLinePreview{};
  delete m_image;
  m_image = nullptr;
  update();
}

void SurveyTimeSectionWidget::setResult(const SurveyLinePreview &preview)
{
  m_hint.clear();
  m_error = preview.ok ? QString() : preview.error;
  m_preview = preview;
  rebuildImage();
  update();
}

void SurveyTimeSectionWidget::rebuildImage()
{
  delete m_image;
  m_image = nullptr;
  m_clip = 0;
  if (!m_preview.ok || m_preview.columns < 1 || m_preview.samples < 1 ||
      m_preview.amplitudes.size() < qsizetype(m_preview.columns) * m_preview.samples)
    return;
  // 削剪限取 |振幅| 99.5 分位（缺道 NaN 不参与），对称映射到色标。
  std::vector<float> magnitudes;
  magnitudes.reserve(size_t(m_preview.amplitudes.size()));
  for (float value : m_preview.amplitudes)
    if (std::isfinite(value))
      magnitudes.push_back(std::abs(value));
  if (magnitudes.empty())
    return;
  const auto rank = magnitudes.begin() + ptrdiff_t(0.995 * double(magnitudes.size() - 1));
  std::nth_element(magnitudes.begin(), rank, magnitudes.end());
  m_clip = std::max(*rank, 1e-12f);
  const int columns = qMin(m_preview.columns, 768);
  const int rows = qMin(m_preview.samples, 2048);
  m_image = new QImage(columns, rows, QImage::Format_RGB32);
  for (int column = 0; column < columns; ++column)
  {
    const int sourceColumn = m_preview.columns > 1
                                 ? qRound(double(column) * (m_preview.columns - 1) / (columns - 1))
                                 : 0;
    for (int row = 0; row < rows; ++row)
    {
      const int sourceRow = m_preview.samples > 1
                                ? qRound(double(row) * (m_preview.samples - 1) / (rows - 1))
                                : 0;
      const float value = m_preview.amplitudes[sourceColumn * m_preview.samples + sourceRow];
      QRgb color;
      if (!std::isfinite(value))
        color = qRgb(235, 239, 245); // 缺道：surface-alt 中性底
      else
      {
        // 变密度：正红负蓝、零白（数据符号色）。
        const double t = std::clamp(double(value) / m_clip, -1.0, 1.0);
        const double magnitude = std::abs(t);
        const int channel = qRound(255 * (1.0 - 0.92 * magnitude));
        color = t >= 0 ? qRgb(255, channel, channel) : qRgb(channel, channel, 255);
      }
      m_image->setPixel(column, row, color);
    }
  }
}

void SurveyTimeSectionWidget::paintEvent(QPaintEvent *)
{
  QPainter painter(this);
  painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
  const auto &tokens = PaleoTheme::tokens();
  const QFont mono = PaleoTheme::monoFont(tokens.labelPt);
  const int axisWidth = 52;
  const QRect body(axisWidth, tokens.spacingSm, width() - axisWidth - tokens.spacingSm,
                   height() - tokens.spacingSm * 2);

  if (!m_hint.isEmpty() || !m_error.isEmpty())
  {
    painter.setPen(m_error.isEmpty() ? QColor(tokens.textMuted) : QColor(tokens.errorText));
    painter.setFont(PaleoTheme::bodyFont(tokens.bodyPt));
    painter.drawText(rect().adjusted(tokens.spacingMd, 0, -tokens.spacingMd, 0),
                     Qt::AlignCenter,
                     m_error.isEmpty() ? m_hint : tr("时间剖面不可用：%1").arg(m_error));
    return;
  }
  if (!m_image || !m_preview.ok)
  {
    painter.setPen(QColor(tokens.textMuted));
    painter.setFont(PaleoTheme::bodyFont(tokens.bodyPt));
    painter.drawText(rect(), Qt::AlignCenter, tr("无地震体 — 时间剖面待数据"));
    return;
  }
  painter.drawImage(body, *m_image);
  painter.setPen(QColor(tokens.border));
  painter.drawRect(body.adjusted(0, 0, -1, -1));

  // 标题（左上角）+ 时间轴刻度（左侧，mono；时间 = first + sample*interval）。
  painter.setPen(QColor(tokens.text));
  QFont titleFont = PaleoTheme::bodyFont(tokens.bodyPt);
  titleFont.setWeight(QFont::Medium);
  painter.setFont(titleFont);
  painter.drawText(QRect(body.x(), 0, body.width(), tokens.spacingSm * 3),
                   Qt::AlignLeft | Qt::AlignVCenter, m_preview.title);
  painter.setFont(mono);
  painter.setPen(QColor(tokens.textMuted));
  const double lastMs = m_preview.firstTimeMs + (m_preview.samples - 1) * m_preview.sampleIntervalMs;
  for (int i = 0; i <= 4; ++i)
  {
    const double ms = m_preview.firstTimeMs + (lastMs - m_preview.firstTimeMs) * i / 4.0;
    const int y = body.bottom() - qRound(double(i) * body.height() / 4.0);
    painter.drawText(QRect(0, y - 8, axisWidth - 6, 16), Qt::AlignRight | Qt::AlignVCenter,
                     QString::number(ms, 'f', 0));
    painter.setPen(QColor(tokens.border));
    painter.drawLine(body.x() - 3, y, body.x(), y);
    painter.setPen(QColor(tokens.textMuted));
  }
}
