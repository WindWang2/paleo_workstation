// 层：视图
#pragma once

// 方向97（datapreviewtabs 拆分）：地震剖面对面板——自 datapreviewtabs_internal.h
// 逐字节迁出。消费方：datapreviewtabseismic.cpp（剖面构建）、datapreviewtabs.cpp
//（onSectionReady/Failed 挂起应用）；internal.h 组合导出，族内 TU 无需直接 include。
// 自包含修复：internal.h 版依赖包含方自带 nicestep.h，此头自带。

#include <QColor>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
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
#include "../seismicsection/seismiccolormap.h"

namespace paleo::datapreview_detail {

// 地震剖面：一条 inline/crossline 的可切换振幅色标渲染（§4/§7：只解码这一条）。
// 时间轴与色标：左侧显示 TWT(ms) 时间刻度轴，右侧色标与剖面共用当前调色板。
// D61 标定：井的 D61 分层经时深表换算成 ms 后，在剖面上画一条水平标记线。
class SectionPanel : public QWidget
{
public:
  // 振幅显示增益：映射前固定放大 1.35 倍（弱振幅更醒目）。色标顶/底因此对应
  // 实测振幅 maxAmp/1.35（饱和点），刻度按这个标——否则按色标读振幅会静默
  // 偏大 35%。
  static constexpr float kAmplitudeGain = 1.35f;

  SectionPanel(QWidget *parent = nullptr) : QWidget(parent) { setMinimumSize(320, 260); }
  seismic::SectionColorMapType colorMap() const { return m_colorMap; }
  void setColorMap(seismic::SectionColorMapType type)
  {
    if (static_cast<int>(type) < 0 || static_cast<int>(type) >= seismic::kSectionColorMapCount) return;
    m_colorMap = type;
    if (!m_img.isNull()) updateColorTable();
    update();
  }
  QWidget *createColorMapControl(QWidget *parent)
  {
    auto *control = new QWidget(parent);
    auto *layout = new QHBoxLayout(control);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(PaleoTheme::tokens().spacingXs);
    auto *label = new QLabel(QObject::tr("色标:"), control);
    PaleoTheme::applyThemedStyleSheet(label, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    auto *combo = new QComboBox(control);
    combo->setObjectName(QStringLiteral("previewSectionColorMap"));
    combo->setAccessibleName(QObject::tr("二维地震剖面色标"));
    combo->setToolTip(QObject::tr("切换振幅色标；剖面和右侧图例同步更新，无需重新读取测线"));
    combo->addItems({QObject::tr("红白蓝 (双极)"), QObject::tr("灰度 (单极)"), QObject::tr("彩虹谱 (相图)"),
                     QObject::tr("蓝白红 (反双极)"), QObject::tr("黑-白-蓝 (纸面)"), QObject::tr("红-白-黑 (纸面)"),
                     QObject::tr("绿-白-品红"), QObject::tr("青-白-橙")});
    combo->setCurrentIndex(static_cast<int>(m_colorMap));
    connect(combo, &QComboBox::currentIndexChanged, this, [this](int index) {
      setColorMap(static_cast<seismic::SectionColorMapType>(index));
    });
    layout->addWidget(label);
    layout->addWidget(combo);
    return control;
  }
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
    m_img = QImage(w, h, QImage::Format_Indexed8);
    if (m_img.isNull()) { setError(QObject::tr("无法创建剖面图像")); return; }
    updateColorTable();
    m_img.fill(0);
    float amp = 1e-6f;
    for (const SegyTrace &t : traces)
      for (float s : t.samples)
        if (std::isfinite(s)) amp = qMax(amp, qAbs(s));
    for (int x = 0; x < w; ++x)
    {
      const SegyTrace &t = traces.at(x);
      for (int y = 0; y < h; ++y)
      {
        float sample = 0.0f;
        if (!grid.sampleAt(t, y, dtUs, t0Ms, &sample)) continue;
        if (!std::isfinite(sample)) continue;
        const float v = std::clamp((sample / amp) * kAmplitudeGain, -1.0f, 1.0f);
        const int index = qRound(v * 127.0f) + 128;
        // 极弱负振幅仍使用负极性颜色，不能被量化到零后染成正极性。
        m_img.setPixel(x, y, v < 0.0f ? qMin(127, index) : index);
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
    p.drawText(QRect(2, 4, leftMargin - 4, 18), Qt::AlignCenter, QObject::tr("TWT (ms)"));

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
    // PW-135：不再每帧 SmoothTransformation 全图重采样（分配 + 双线性模糊
    // 薄同相轴）；最近邻直绘，与主剖面画布同一口径。
    p.save();
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.drawImage(QRectF(dst), m_img);
    p.restore();

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
    p.drawText(QRect(dst.right(), 4, rightMargin, labelHeight), Qt::AlignCenter,
               QObject::tr("色标 ×%1").arg(kAmplitudeGain, 0, 'f', 2));

    const int barW = 10;
    const int barX = dst.right() + 6;
    const int barTop = topMargin + labelHeight + PaleoTheme::tokens().spacingXs;
    const int barH = std::max(20, dst.height() - labelHeight - PaleoTheme::tokens().spacingLg);

    QLinearGradient grad(barX, barTop, barX, barTop + barH);
    for (int i = 0; i < 255; ++i)
      grad.setColorAt(i / 254.0, QColor::fromRgb(m_img.color(255 - i)));

    p.setBrush(grad);
    p.setPen(QPen(PaleoTheme::tokens().border, 1.0));
    p.drawRoundedRect(QRectF(barX, barTop, barW, barH), PaleoTheme::tokens().radiusSm, PaleoTheme::tokens().radiusSm);

    // 刻度值
    p.setFont(monoFont);
    p.setPen(PaleoTheme::tokens().text);
    // 刻度值：色标两端对应的是「饱和点」实测振幅 = maxAmp / 显示增益
    // （≥ 该值的采样一律染成极色），不是 maxAmp 本身。
    const float satAmp = m_maxAmp / kAmplitudeGain;
    const QString maxStr = satAmp >= 1000.0f
        ? QStringLiteral("+%1k").arg(satAmp / 1000.0f, 0, 'f', 0)
        : QStringLiteral("+%1").arg(qRound(satAmp));
    const QString minStr = satAmp >= 1000.0f
        ? QStringLiteral("-%1k").arg(satAmp / 1000.0f, 0, 'f', 0)
        : QStringLiteral("-%1").arg(qRound(satAmp));

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
  void updateColorTable()
  {
    // 0 保留给缺测；1..255 表达 [-1,1]，128 为零振幅。换色只更新调色板。
    QVector<QRgb> table(256);
    table[0] = qRgb(255, 255, 255);
    for (int i = 1; i < 256; ++i)
      table[i] = seismic::SampleColorMap(m_colorMap, (i - 128) / 127.0f);
    m_img.setColorTable(table);
  }
  seismic::SectionColorMapType m_colorMap = seismic::SectionColorMapType::RedWhiteBlue;
  QImage m_img;
  QString m_caption;
  QString m_error;
  float m_maxAmp = 1.0f;
  double m_t0Ms = 0.0, m_dtMs = 0.0;
  double m_tieMs = qQNaN();
  QString m_tieLabel;
};

} // namespace paleo::datapreview_detail
