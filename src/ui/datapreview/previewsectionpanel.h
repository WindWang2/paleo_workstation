// 层：视图
// token 例外：DESIGN 数据符号例外：地震振幅色带（红白蓝 grad 三档，自 datapreviewtabs_internal.h 迁入）；图表 chrome 已转 token。（tools/ui-token-exceptions.json 精确计数）。
#pragma once

// 方向97（datapreviewtabs 拆分）：地震剖面对面板——自 datapreviewtabs_internal.h
// 逐字节迁出。消费方：datapreviewtabseismic.cpp（剖面构建）、datapreviewtabs.cpp
//（onSectionReady/Failed 挂起应用）；internal.h 组合导出，族内 TU 无需直接 include。
// 自包含修复：internal.h 版依赖包含方自带 nicestep.h，此头自带。

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QPen>
#include <QRectF>
#include <QString>
#include <QVector>
#include <QWidget>

#include <algorithm>   // std::clamp（振幅归一）
#include <cmath>

#include "../../domain/seismic/nicestep.h" // seismic::NiceStep（时间轴刻度）
#include "../../domain/sectiontrace.h"     // SegyTrace/SegySectionGrid
#include "../paleotheme.h"

namespace paleo::datapreview_detail {

// 地震剖面：一条 inline/crossline 的红白蓝双极振幅渲染（§4/§7：只解码这一条）。
// 时间轴与色标：左侧显示 TWT(ms) 时间刻度轴，右侧显示红白蓝振幅色标与极性标注。
// D61 标定：井的 D61 分层经时深表换算成 ms 后，在剖面上画一条水平标记线。
class SectionPanel : public QWidget
{
public:
  SectionPanel(QWidget *parent = nullptr) : QWidget(parent) { setMinimumSize(320, 260); }
  void setTraces(const QVector<SegyTrace> &traces, float dtUs, double t0Ms,
                 const QString &readWarning = {})
  {
    if (traces.isEmpty())
    {
      clearImage();
      return;
    }
    const int w = qMax(1, traces.size());
    const SegySectionGrid grid = SegySectionGrid::forTraces(traces, dtUs, t0Ms);
    const int h = grid.rows;
    m_img = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
    m_img.fill(qRgb(255, 255, 255));
    float amp = 1e-6f;
    for (const SegyTrace &t : traces)
      for (float s : t.samples)
        amp = qMax(amp, qAbs(s));
    for (int x = 0; x < w; ++x)
    {
      const SegyTrace &t = traces.at(x);
      for (int y = 0; y < h; ++y)
      {
        float sample = 0.0f;
        if (!grid.sampleAt(t, y, dtUs, t0Ms, &sample)) continue;
        const float v = std::clamp((sample / amp) * 1.35f, -1.0f, 1.0f);
        const float mag = std::pow(std::abs(v), 0.85f);
        const float k = 1.0f - mag;
        QRgb color;
        if (v < 0.0f) {
            // Deep blue to white (Trough)
            color = qRgb(static_cast<int>(217 * k), static_cast<int>(230 * k), 255);
        } else {
            // White to deep red (Peak)
            color = qRgb(255, static_cast<int>(224 * k), static_cast<int>(214 * k));
        }
        m_img.setPixel(x, y, color);
      }
    }
    m_maxAmp = amp;
    m_t0Ms = grid.startMs;
    m_dtMs = grid.stepMs;
    m_caption = QObject::tr("%1 道 · %2 样点 · %3 ms 采样 · t0 = %4 ms")
                    .arg(traces.size())
                    .arg(grid.rows)
                    .arg(grid.stepMs, 0, 'f', 1)
                    .arg(grid.startMs, 0, 'f', 1);
    if (!readWarning.isEmpty()) m_caption = readWarning + QStringLiteral(" · ") + m_caption;
    setToolTip(readWarning);
    update();
  }
  bool hasImage() const { return !m_img.isNull(); }
  void clearImage()
  {
    m_img = QImage();
    m_caption.clear();
    setToolTip(QString());
    m_error.clear();
    clearTieMarker();
    update();
  }
  void setError(const QString &text)
  {
    m_img = QImage();
    m_error = text;
    clearTieMarker();
    update();
  }
  void setTieMarker(const QString &label, double ms)
  {
    m_tieLabel = label;
    m_tieMs = ms;
    update();
  }
  void clearTieMarker()
  {
    m_tieMs = qQNaN();
    m_tieLabel.clear();
  }

protected:
  void paintEvent(QPaintEvent *) override
  {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), PaleoTheme::tokens().surface);
    if (m_img.isNull())
    {
      p.setPen(PaleoTheme::tokens().textMuted);
      p.drawText(rect(), Qt::AlignCenter,
                 m_error.isEmpty() ? QObject::tr("尚未解码剖面") : m_error);
      return;
    }

    const int leftMargin = 58;
    const int rightMargin = 54;
    const int topMargin = 26;
    const int bottomMargin = 16;
    const QRect dst(leftMargin, topMargin,
                    std::max(10, width() - leftMargin - rightMargin),
                    std::max(10, height() - topMargin - bottomMargin));

    // 1. 左侧时间刻度轴 (TWT ms 时间剖面)
    const QRect leftRuler(0, topMargin, leftMargin, dst.height());
    p.fillRect(leftRuler, PaleoTheme::tokens().surfaceAlt);
    p.setPen(PaleoTheme::tokens().border);
    p.drawLine(leftMargin, topMargin, leftMargin, dst.bottom());

    QFont monoFont = PaleoTheme::monoFont();
    monoFont.setPointSize(PaleoTheme::tokens().labelPt);
    QFont bodyFont = PaleoTheme::bodyFont();
    bodyFont.setPointSize(PaleoTheme::tokens().labelPt);
    const int monoHeight = QFontMetrics(monoFont).height();
    const int labelHeight = QFontMetrics(bodyFont).height();
    p.setFont(bodyFont);
    p.setPen(PaleoTheme::tokens().textMuted);
    p.drawText(QRect(2, 4, leftMargin - 4, 18), Qt::AlignCenter, QStringLiteral("TWT (ms)"));

    if (m_dtMs > 0.0 && m_img.height() > 0)
    {
      const double endTimeMs = m_t0Ms + m_img.height() * m_dtMs;
      const auto ticks = seismic::NiceStep::GenerateTicks(m_t0Ms, endTimeMs, topMargin, dst.bottom(), 6, QStringLiteral("%.0f"));
      p.setFont(monoFont);
      for (const auto &tk : ticks)
      {
        if (tk.pixelPos < topMargin || tk.pixelPos > dst.bottom()) continue;
        p.setPen(PaleoTheme::tokens().textMuted);
        p.drawLine(QPointF(leftMargin - 6.0, tk.pixelPos), QPointF(leftMargin, tk.pixelPos));
        p.setPen(PaleoTheme::tokens().text);
        p.drawText(QRectF(2, tk.pixelPos - monoHeight / 2.0, leftMargin - 10, monoHeight), Qt::AlignRight | Qt::AlignVCenter, QString::number(qRound(tk.value)));
      }
    }

    // 2. 剖面核心地震图像
    p.drawImage(dst, m_img.scaled(dst.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));

    // 3. D61 标定线（时间 ms → 剖面内水平线 + 井名标注）
    if (std::isfinite(m_tieMs) && m_dtMs > 0.0 && m_img.height() > 1)
    {
      const double row = (m_tieMs - m_t0Ms) / m_dtMs;
      const double yFrac = (row + 0.5) / m_img.height();
      if (yFrac >= 0.0 && yFrac <= 1.0)
      {
        const int y = dst.top() + qRound(yFrac * dst.height());
        // Data tie on the amplitude image: retain its light-canvas ink in both themes.
        p.setPen(QPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).text, 1.5));
        p.drawLine(dst.left(), y, dst.right(), y);
        p.setFont(bodyFont);
        p.drawText(QRect(dst.left() + 4, y - 16, dst.width() - 8, 14), Qt::AlignLeft,
                   m_tieLabel);
      }
    }

    // 4. 右侧振幅色标 (Color Bar)
    const QRect rightBarRect(dst.right(), 0, rightMargin, height());
    p.fillRect(rightBarRect, PaleoTheme::tokens().surfaceAlt);
    p.setPen(PaleoTheme::tokens().border);
    p.drawLine(dst.right(), 0, dst.right(), height());

    QFont colorBarFont = bodyFont;
    colorBarFont.setBold(true);
    p.setFont(colorBarFont);
    p.setPen(PaleoTheme::tokens().text);
    p.drawText(QRect(dst.right(), 4, rightMargin, labelHeight), Qt::AlignCenter, tr("色标"));

    const int barW = 10;
    const int barX = dst.right() + 6;
    const int barTop = topMargin + labelHeight + PaleoTheme::tokens().spacingXs;
    const int barH = std::max(20, dst.height() - labelHeight - PaleoTheme::tokens().spacingLg);

    QLinearGradient grad(barX, barTop, barX, barTop + barH);
    grad.setColorAt(0.0, QColor(220, 38, 38));   // Red Peak
    grad.setColorAt(0.5, QColor(255, 255, 255)); // White Zero
    grad.setColorAt(1.0, QColor(25, 118, 210));  // Blue Trough

    p.setBrush(grad);
    p.setPen(QPen(PaleoTheme::tokens().border, 1.0));
    p.drawRoundedRect(QRectF(barX, barTop, barW, barH), PaleoTheme::tokens().radiusSm, PaleoTheme::tokens().radiusSm);

    // 刻度值
    p.setFont(monoFont);
    p.setPen(PaleoTheme::tokens().text);
    const QString maxStr = m_maxAmp >= 1000.0f
        ? QStringLiteral("+%1k").arg(m_maxAmp / 1000.0f, 0, 'f', 0)
        : QStringLiteral("+%1").arg(qRound(m_maxAmp));
    const QString minStr = m_maxAmp >= 1000.0f
        ? QStringLiteral("-%1k").arg(m_maxAmp / 1000.0f, 0, 'f', 0)
        : QStringLiteral("-%1").arg(qRound(m_maxAmp));

    p.drawLine(QPointF(barX + barW, barTop), QPointF(barX + barW + 3, barTop));
    p.drawText(QRectF(barX + barW + 4, barTop - monoHeight / 2.0, rightMargin - barW - 10, monoHeight), Qt::AlignLeft | Qt::AlignVCenter, maxStr);

    const double midY = barTop + barH * 0.5;
    p.drawLine(QPointF(barX + barW, midY), QPointF(barX + barW + 3, midY));
    p.drawText(QRectF(barX + barW + 4, midY - monoHeight / 2.0, rightMargin - barW - 10, monoHeight), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("0"));

    p.drawLine(QPointF(barX + barW, barTop + barH), QPointF(barX + barW + 3, barTop + barH));
    p.drawText(QRectF(barX + barW + 4, barTop + barH - monoHeight / 2.0, rightMargin - barW - 10, monoHeight), Qt::AlignLeft | Qt::AlignVCenter, minStr);

    p.setFont(bodyFont);
    p.setPen(PaleoTheme::tokens().textMuted);
    p.drawText(QRectF(dst.right(), barTop - labelHeight - PaleoTheme::tokens().spacingXs, rightMargin - 4, labelHeight), Qt::AlignRight, tr("波峰+"));
    p.setPen(PaleoTheme::tokens().textMuted);
    p.drawText(QRectF(dst.right(), barTop + barH + PaleoTheme::tokens().spacingXs, rightMargin - 4, labelHeight), Qt::AlignRight, tr("波谷-"));

    // 5. 顶部说明条
    p.setPen(PaleoTheme::tokens().textMuted);
    p.setFont(bodyFont);
    p.drawText(QRect(leftMargin + 4, 4, dst.width() - 8, 18), Qt::AlignLeft | Qt::AlignVCenter, m_caption);
  }

private:
  QImage m_img;
  QString m_caption;
  QString m_error;
  float m_maxAmp = 1.0f;
  double m_t0Ms = 0.0, m_dtMs = 0.0;
  double m_tieMs = qQNaN();
  QString m_tieLabel;
};

} // namespace paleo::datapreview_detail
