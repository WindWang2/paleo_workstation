// 层：视图
// 综合图道·复合道（地层系统组 StratigraphyCompound / 沉积相 FaciesCompound）与访问器——自 wellcompositetrack.cpp 拆出（方向 66，行为零变更）
#include "wellcompositetrack.h"
#include "ui/paleotheme.h"
#include <QCoreApplication>
#include <cmath>

namespace WellComposite
{
// ----------------------------------------------------------------------------
// 9. 地层系统组组合道 (StratigraphyCompoundTrack)
// ----------------------------------------------------------------------------
StratigraphyCompoundTrack::StratigraphyCompoundTrack(const QString &title, qreal width)
  : m_width(width)
{
  m_title = title.isEmpty() ? QCoreApplication::translate("WellCompositeTrack", "地层") : title;
}

void StratigraphyCompoundTrack::setSubColumnWidths(qreal sysW, qreal serW)
{
  m_systemWidth = qMax(20.0, sysW);
  m_seriesWidth = qMax(20.0, serW);
}

void StratigraphyCompoundTrack::paintHeader(QPainter &painter, const QRectF &headerRect, double /*currentDepth*/)
{
  painter.save();
  painter.setClipRect(headerRect);

  // 背景
  painter.fillRect(headerRect, PaleoTheme::tokens(PaleoTheme::Theme::Light).surfaceAlt);
  painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).border);
  painter.drawLine(headerRect.topRight(), headerRect.bottomRight());
  painter.drawLine(headerRect.bottomLeft(), headerRect.bottomRight());

  const qreal topH = std::floor(headerRect.height() * 0.5);
  const qreal botH = headerRect.height() - topH;

  // 顶层合并道头：「地层」
  const QRectF topRect(headerRect.left(), headerRect.top(), headerRect.width(), topH);
  painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).border);
  painter.drawLine(topRect.bottomLeft(), topRect.bottomRight());

  painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).text);
  QFont fTitle = painter.font();
  fTitle.setPointSize(PaleoTheme::tokens().bodyPt);
  fTitle.setBold(true);
  painter.setFont(fTitle);
  painter.drawText(topRect, Qt::AlignCenter, title());

  // 底层次级道头：3 列分栏「系 | 统 | 组」
  const qreal col1W = m_systemWidth;
  const qreal col2W = m_seriesWidth;
  const qreal col3W = qMax<qreal>(20.0, headerRect.width() - col1W - col2W);

  const QRectF rSys(headerRect.left(), headerRect.top() + topH, col1W, botH);
  const QRectF rSer(rSys.right(), headerRect.top() + topH, col2W, botH);
  const QRectF rForm(rSer.right(), headerRect.top() + topH, col3W, botH);

  painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).border);
  painter.drawLine(rSys.topRight(), rSys.bottomRight());
  painter.drawLine(rSer.topRight(), rSer.bottomRight());

  QFont fSub = painter.font();
  fSub.setPointSize(PaleoTheme::tokens().labelPt);
  fSub.setBold(true);
  painter.setFont(fSub);
  painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).textMuted); // text-muted

  painter.drawText(rSys, Qt::AlignCenter, QCoreApplication::translate("WellCompositeTrack", "系"));
  painter.drawText(rSer, Qt::AlignCenter, QCoreApplication::translate("WellCompositeTrack", "统"));
  painter.drawText(rForm, Qt::AlignCenter, QCoreApplication::translate("WellCompositeTrack", "组"));

  painter.restore();
}

void StratigraphyCompoundTrack::paintBody(QPainter &painter, const QRectF &bodyRect,
                                          double topDepth, double bottomDepth, double pxPerMeter)
{
  painter.save();
  painter.setClipRect(bodyRect);
  painter.fillRect(bodyRect, PaleoTheme::tokens(PaleoTheme::Theme::Light).surface);

  const qreal col1W = m_systemWidth;
  const qreal col2W = m_seriesWidth;
  const qreal col3W = qMax<qreal>(20.0, bodyRect.width() - col1W - col2W);

  const qreal col1X = bodyRect.left();
  const qreal col2X = col1X + col1W;
  const qreal col3X = col2X + col2W;

  // 绘制竖向分割线
  painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).border);
  painter.drawLine(QPointF(col2X, bodyRect.top()), QPointF(col2X, bodyRect.bottom()));
  painter.drawLine(QPointF(col3X, bodyRect.top()), QPointF(col3X, bodyRect.bottom()));
  painter.drawLine(bodyRect.topRight(), bodyRect.bottomRight());

  QFont fBody = painter.font();
  fBody.setPointSize(PaleoTheme::tokens().labelPt);
  fBody.setBold(true);
  painter.setFont(fBody);

  // 1. 绘制「系」：对连续相同系名称进行跨层合并绘制
  struct MergedSys {
    float topD = 0;
    float botD = 0;
    QString name;
    QColor color;
  };
  QVector<MergedSys> sysGroups;
  for (const auto &it : m_intervals)
  {
    if (!sysGroups.isEmpty() && sysGroups.last().name == it.system)
    {
      sysGroups.last().botD = qMax(sysGroups.last().botD, it.bottomDepth);
    }
    else
    {
      sysGroups.append({it.topDepth, it.bottomDepth, it.system, it.systemColor});
    }
  }

  for (const auto &grp : sysGroups)
  {
    if (grp.botD < topDepth || grp.topD > bottomDepth) continue;
    const qreal y0 = bodyRect.top() + (grp.topD - topDepth) * pxPerMeter;
    const qreal y1 = bodyRect.top() + (grp.botD - topDepth) * pxPerMeter;
    const QRectF box(col1X, y0, col1W, qMax<qreal>(4.0, y1 - y0));

    painter.fillRect(box, grp.color);
    painter.setPen(QPen(QColor(QStringLiteral("#78909C")), 1.0));
    painter.drawLine(box.topLeft(), box.topRight());
    painter.drawLine(box.bottomLeft(), box.bottomRight());

    const QRectF textBox = box.intersected(bodyRect);
    if (textBox.height() >= 16.0 && !grp.name.isEmpty())
    {
      painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).text);
      QString dt = grp.name;
      if (textBox.height() >= 45.0 && col1W <= 42.0)
      {
        QStringList chars;
        for (const QChar &ch : grp.name) chars << QString(ch);
        dt = chars.join(QLatin1Char('\n'));
      }
      painter.drawText(textBox.adjusted(1, 2, -1, -2), Qt::AlignCenter, dt);
    }
  }

  // 2. 绘制「统」：对连续相同统名称进行跨层合并绘制
  struct MergedSer {
    float topD = 0;
    float botD = 0;
    QString name;
    QColor color;
  };
  QVector<MergedSer> serGroups;
  for (const auto &it : m_intervals)
  {
    if (!serGroups.isEmpty() && serGroups.last().name == it.series)
    {
      serGroups.last().botD = qMax(serGroups.last().botD, it.bottomDepth);
    }
    else
    {
      serGroups.append({it.topDepth, it.bottomDepth, it.series, it.seriesColor});
    }
  }

  for (const auto &grp : serGroups)
  {
    if (grp.botD < topDepth || grp.topD > bottomDepth) continue;
    const qreal y0 = bodyRect.top() + (grp.topD - topDepth) * pxPerMeter;
    const qreal y1 = bodyRect.top() + (grp.botD - topDepth) * pxPerMeter;
    const QRectF box(col2X, y0, col2W, qMax<qreal>(4.0, y1 - y0));

    painter.fillRect(box, grp.color);
    painter.setPen(QPen(QColor(QStringLiteral("#90A4AE")), 1.0));
    painter.drawLine(box.topLeft(), box.topRight());
    painter.drawLine(box.bottomLeft(), box.bottomRight());

    const QRectF textBox = box.intersected(bodyRect);
    if (textBox.height() >= 16.0 && !grp.name.isEmpty())
    {
      painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).text);
      QString dt = grp.name;
      if (textBox.height() >= 45.0 && col2W <= 46.0)
      {
        QStringList chars;
        for (const QChar &ch : grp.name) chars << QString(ch);
        dt = chars.join(QLatin1Char('\n'));
      }
      painter.drawText(textBox.adjusted(1, 2, -1, -2), Qt::AlignCenter, dt);
    }
  }

  // 3. 绘制「组」：具体地层分层
  for (const auto &it : m_intervals)
  {
    if (it.bottomDepth < topDepth || it.topDepth > bottomDepth) continue;
    const qreal y0 = bodyRect.top() + (it.topDepth - topDepth) * pxPerMeter;
    const qreal y1 = bodyRect.top() + (it.bottomDepth - topDepth) * pxPerMeter;
    const QRectF box(col3X, y0, col3W, qMax<qreal>(4.0, y1 - y0));

    painter.fillRect(box, it.formationColor);
    painter.setPen(QPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).text, 1.0));
    painter.drawLine(box.topLeft(), box.topRight());
    painter.drawLine(box.bottomLeft(), box.bottomRight());

    const QRectF textBox = box.intersected(bodyRect);
    if (textBox.height() >= 14.0 && !it.formation.isEmpty())
    {
      painter.setPen(QColor(QStringLiteral("#1A237E")));
      painter.drawText(textBox.adjusted(2, 2, -2, -2), Qt::AlignCenter | Qt::TextWordWrap, it.formation);
    }
  }

  painter.restore();
}

void StratigraphyCompoundTrack::autoDeriveStratigraphy(const QVector<FormationInterval> &formations,
                                                      double minDepth, double maxDepth)
{
  Q_UNUSED(minDepth);
  Q_UNUSED(maxDepth);
  m_intervals.clear();

  if (!formations.isEmpty())
  {
    for (const auto &f : formations)
    {
      StratigraphyInterval si;
      si.topDepth = f.topDepth;
      si.bottomDepth = f.bottomDepth;
      si.formation = f.name;
      si.formationColor = f.color;

      const QString n = f.name.trimmed();
      if (n.contains(QStringLiteral("粤海")) || n.contains(QStringLiteral("万山")))
      {
        si.system = QStringLiteral("新近系");
        si.series = QStringLiteral("上新统");
        si.systemColor = QColor(QStringLiteral("#FFF9C4"));
        si.seriesColor = QColor(QStringLiteral("#FFF59D"));
      }
      else if (n.contains(QStringLiteral("韩江")))
      {
        si.system = QStringLiteral("新近系");
        si.series = QStringLiteral("中新统");
        si.systemColor = QColor(QStringLiteral("#FFF9C4"));
        si.seriesColor = QColor(QStringLiteral("#FFE082"));
      }
      else if (n.contains(QStringLiteral("珠江")))
      {
        si.system = QStringLiteral("新近系");
        si.series = QStringLiteral("早中新统");
        si.systemColor = QColor(QStringLiteral("#FFF9C4"));
        si.seriesColor = QColor(QStringLiteral("#FFD54F"));
      }
      else if (n.contains(QStringLiteral("珠海")))
      {
        si.system = QStringLiteral("古近系");
        si.series = QStringLiteral("渐新统");
        si.systemColor = QColor(QStringLiteral("#FFF3E0"));
        si.seriesColor = QColor(QStringLiteral("#FFCC80"));
      }
      else if (n.contains(QStringLiteral("恩平")))
      {
        si.system = QStringLiteral("古近系");
        si.series = QStringLiteral("始新统");
        si.systemColor = QColor(QStringLiteral("#FFF3E0"));
        si.seriesColor = QColor(QStringLiteral("#FFA726"));
      }
      else if (n.contains(QStringLiteral("文昌")))
      {
        si.system = QStringLiteral("古近系");
        si.series = QStringLiteral("始新统");
        si.systemColor = QColor(QStringLiteral("#FFF3E0"));
        si.seriesColor = QColor(QStringLiteral("#FFA726"));
      }
      else if (n.contains(QStringLiteral("新近")))
      {
        si.system = QStringLiteral("新近系");
        si.series = QStringLiteral("中新统");
        si.systemColor = QColor(QStringLiteral("#FFF9C4"));
        si.seriesColor = QColor(QStringLiteral("#FFE082"));
      }
      else if (n.contains(QStringLiteral("古近")))
      {
        si.system = QStringLiteral("古近系");
        si.series = QStringLiteral("古新统");
        si.systemColor = QColor(QStringLiteral("#FFF3E0"));
        si.seriesColor = QColor(QStringLiteral("#FFCC80"));
      }
      else if (n.contains(QStringLiteral("白垩")))
      {
        si.system = QStringLiteral("白垩系");
        si.series = QStringLiteral("上白垩统");
        si.systemColor = QColor(QStringLiteral("#E8F5E9"));
        si.seriesColor = QColor(QStringLiteral("#C8E6C9"));
      }
      // 未识别层名：不臆造系/统，仅显示真实层名
      m_intervals.append(si);
    }
  }
}

// ----------------------------------------------------------------------------
// 10. 沉积相组合道 (FaciesCompoundTrack)
// ----------------------------------------------------------------------------
FaciesCompoundTrack::FaciesCompoundTrack(const QString &title, qreal width)
  : m_width(width)
{
  m_title = title.isEmpty() ? QCoreApplication::translate("WellCompositeTrack", "沉积相") : title;
}

void FaciesCompoundTrack::setSubColumnWidths(qreal majW, qreal subW)
{
  m_majorWidth = qMax(20.0, majW);
  m_subWidth = qMax(20.0, subW);
}

void FaciesCompoundTrack::paintHeader(QPainter &painter, const QRectF &headerRect, double /*currentDepth*/)
{
  painter.save();
  painter.setClipRect(headerRect);

  // 背景
  painter.fillRect(headerRect, PaleoTheme::tokens(PaleoTheme::Theme::Light).surfaceAlt);
  painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).border);
  painter.drawLine(headerRect.topRight(), headerRect.bottomRight());
  painter.drawLine(headerRect.bottomLeft(), headerRect.bottomRight());

  const qreal topH = std::floor(headerRect.height() * 0.5);
  const qreal botH = headerRect.height() - topH;

  // 顶层合并道头：「沉积相」
  const QRectF topRect(headerRect.left(), headerRect.top(), headerRect.width(), topH);
  painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).border);
  painter.drawLine(topRect.bottomLeft(), topRect.bottomRight());

  painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).text);
  QFont fTitle = painter.font();
  fTitle.setPointSize(PaleoTheme::tokens().bodyPt);
  fTitle.setBold(true);
  painter.setFont(fTitle);
  painter.drawText(topRect, Qt::AlignCenter, title());

  // 底层次级道头：3 列分栏「相 | 亚 | 微」
  const qreal col1W = m_majorWidth;
  const qreal col2W = m_subWidth;
  const qreal col3W = qMax<qreal>(20.0, headerRect.width() - col1W - col2W);

  const QRectF rMaj(headerRect.left(), headerRect.top() + topH, col1W, botH);
  const QRectF rSub(rMaj.right(), headerRect.top() + topH, col2W, botH);
  const QRectF rMic(rSub.right(), headerRect.top() + topH, col3W, botH);

  painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).border);
  painter.drawLine(rMaj.topRight(), rMaj.bottomRight());
  painter.drawLine(rSub.topRight(), rSub.bottomRight());

  QFont fSub = painter.font();
  fSub.setPointSize(PaleoTheme::tokens().labelPt);
  fSub.setBold(true);
  painter.setFont(fSub);
  painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).textMuted); // text-muted

  painter.drawText(rMaj, Qt::AlignCenter, QCoreApplication::translate("WellCompositeTrack", "相"));
  painter.drawText(rSub, Qt::AlignCenter, QCoreApplication::translate("WellCompositeTrack", "亚"));
  painter.drawText(rMic, Qt::AlignCenter, QCoreApplication::translate("WellCompositeTrack", "微"));

  painter.restore();
}

void FaciesCompoundTrack::paintBody(QPainter &painter, const QRectF &bodyRect,
                                    double topDepth, double bottomDepth, double pxPerMeter)
{
  painter.save();
  painter.setClipRect(bodyRect);
  painter.fillRect(bodyRect, PaleoTheme::tokens(PaleoTheme::Theme::Light).surface);

  const qreal col1W = m_majorWidth;
  const qreal col2W = m_subWidth;
  const qreal col3W = qMax<qreal>(20.0, bodyRect.width() - col1W - col2W);

  const qreal col1X = bodyRect.left();
  const qreal col2X = col1X + col1W;
  const qreal col3X = col2X + col2W;

  // 绘制竖向分割线（#DFE5EC 在白底上近乎不可见，加深一档）
  painter.setPen(QColor(QStringLiteral("#B0BEC5")));
  painter.drawLine(QPointF(col2X, bodyRect.top()), QPointF(col2X, bodyRect.bottom()));
  painter.drawLine(QPointF(col3X, bodyRect.top()), QPointF(col3X, bodyRect.bottom()));
  painter.drawLine(bodyRect.topRight(), bodyRect.bottomRight());

  QFont fBody = painter.font();
  fBody.setPointSize(PaleoTheme::tokens().bodyPt);
  fBody.setBold(true);
  painter.setFont(fBody);

  // 1. 绘制「相」：对连续相同相名称进行跨层合并绘制
  struct MergedMajor {
    float topD = 0;
    float botD = 0;
    QString name;
    QColor color;
  };
  QVector<MergedMajor> majGroups;
  for (const auto &it : m_intervals)
  {
    if (!majGroups.isEmpty() && majGroups.last().name == it.majorFacies)
    {
      majGroups.last().botD = qMax(majGroups.last().botD, it.bottomDepth);
    }
    else
    {
      majGroups.append({it.topDepth, it.bottomDepth, it.majorFacies, it.majorColor});
    }
  }

  for (const auto &grp : majGroups)
  {
    if (grp.botD < topDepth || grp.topD > bottomDepth) continue;
    const qreal y0 = bodyRect.top() + (grp.topD - topDepth) * pxPerMeter;
    const qreal y1 = bodyRect.top() + (grp.botD - topDepth) * pxPerMeter;
    const QRectF box(col1X, y0, col1W, qMax<qreal>(4.0, y1 - y0));

    painter.fillRect(box, grp.color);
    painter.setPen(QPen(QColor(QStringLiteral("#546E7A")), 1.4));
    painter.drawLine(box.topLeft(), box.topRight());
    painter.drawLine(box.bottomLeft(), box.bottomRight());

    const QRectF textBox = box.intersected(bodyRect);
    if (textBox.height() >= 16.0 && !grp.name.isEmpty())
    {
      painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).text);
      QString dt = grp.name;
      if (textBox.height() >= 45.0 && col1W <= 52.0)
      {
        QStringList chars;
        for (const QChar &ch : grp.name) chars << QString(ch);
        dt = chars.join(QLatin1Char('\n'));
      }
      painter.drawText(textBox.adjusted(1, 2, -1, -2), Qt::AlignCenter, dt);
    }
  }

  // 2. 绘制「亚」：对同一主要相内连续相同亚相名称进行合并绘制
  struct MergedSub {
    float topD = 0;
    float botD = 0;
    QString major;
    QString sub;
    QColor color;
  };
  QVector<MergedSub> subGroups;
  for (const auto &it : m_intervals)
  {
    if (!subGroups.isEmpty() && subGroups.last().major == it.majorFacies && subGroups.last().sub == it.subFacies)
    {
      subGroups.last().botD = qMax(subGroups.last().botD, it.bottomDepth);
    }
    else
    {
      subGroups.append({it.topDepth, it.bottomDepth, it.majorFacies, it.subFacies, it.subColor});
    }
  }

  for (const auto &grp : subGroups)
  {
    if (grp.botD < topDepth || grp.topD > bottomDepth) continue;
    const qreal y0 = bodyRect.top() + (grp.topD - topDepth) * pxPerMeter;
    const qreal y1 = bodyRect.top() + (grp.botD - topDepth) * pxPerMeter;
    const QRectF box(col2X, y0, col2W, qMax<qreal>(4.0, y1 - y0));

    painter.fillRect(box, grp.color);
    painter.setPen(QPen(QColor(QStringLiteral("#78909C")), 1.2));
    painter.drawLine(box.topLeft(), box.topRight());
    painter.drawLine(box.bottomLeft(), box.bottomRight());

    const QRectF textBox = box.intersected(bodyRect);
    if (textBox.height() >= 16.0 && !grp.sub.isEmpty())
    {
      painter.setPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).text);
      QString dt = grp.sub;
      if (textBox.height() >= 55.0 && col2W <= 56.0)
      {
        QStringList chars;
        for (const QChar &ch : grp.sub) chars << QString(ch);
        dt = chars.join(QLatin1Char('\n'));
      }
      painter.drawText(textBox.adjusted(1, 2, -1, -2), Qt::AlignCenter, dt);
    }
  }

  // 3. 绘制「微」（微相）：地质纹理全填充，带半透明高对比胶囊文字保证极致可读性
  for (const auto &it : m_intervals)
  {
    if (it.bottomDepth < topDepth || it.topDepth > bottomDepth) continue;
    const qreal y0 = bodyRect.top() + (it.topDepth - topDepth) * pxPerMeter;
    const qreal y1 = bodyRect.top() + (it.bottomDepth - topDepth) * pxPerMeter;
    const QRectF box(col3X, y0, col3W, qMax<qreal>(4.0, y1 - y0));

    // 使用地质沉积相纹理画刷填充
    const QBrush brush = FaciesPatternFactory::getBrush(
        it.patternType.isEmpty() ? it.microFacies : it.patternType, it.microColor);
    painter.fillRect(box, brush);

    // 上下边界线
    painter.setPen(QPen(PaleoTheme::tokens(PaleoTheme::Theme::Light).text, 1.0));
    painter.drawLine(box.topLeft(), box.topRight());
    painter.drawLine(box.bottomLeft(), box.bottomRight());

    // 绘制微相名称：采用半透明白色胶囊底衬，确保任何复杂纹理下文字 100% 极佳清晰度
    const QRectF textBox = box.intersected(bodyRect);
    if (textBox.height() >= 15.0 && !it.microFacies.isEmpty())
    {
      QFontMetrics fm(painter.font());
      const int tw = fm.horizontalAdvance(it.microFacies);
      const int th = fm.height();
      const qreal pillW = qMin(box.width() - 4.0, static_cast<qreal>(tw + 10));
      const qreal pillH = qMin(textBox.height() - 4.0, static_cast<qreal>(th + 4));

      const QRectF pill(box.center().x() - pillW * 0.5,
                        textBox.center().y() - pillH * 0.5,
                        pillW, pillH);

      painter.fillRect(pill, QColor(255, 255, 255, 220));
      painter.setPen(QPen(QColor(QStringLiteral("#B0BEC5")), 0.8));
      painter.drawRoundedRect(pill, PaleoTheme::tokens().radiusSm, PaleoTheme::tokens().radiusSm);

      painter.setPen(QColor(QStringLiteral("#1A237E")));
      painter.drawText(pill, Qt::AlignCenter, it.microFacies);
    }
  }

  painter.restore();
}

// ----------------------------------------------------------------------------
// D1.9/D3.x 道内数据访问器（tooltip 与编辑会话共用）
// ----------------------------------------------------------------------------
int FaciesCompoundTrack::intervalIndexAtDepth(float depth) const
{
  for (int i = 0; i < m_intervals.size(); ++i)
  {
    if (depth >= m_intervals.at(i).topDepth && depth <= m_intervals.at(i).bottomDepth)
      return i;
  }
  return -1;
}

bool FaciesCompoundTrack::replaceIntervalAt(int idx, const FaciesInterval &interval)
{
  if (idx < 0 || idx >= m_intervals.size())
    return false;
  m_intervals[idx] = interval;
  return true;
}

QString FaciesCompoundTrack::trackToolTip(double depth) const
{
  const int idx = intervalIndexAtDepth(depth);
  if (idx < 0)
    return title();
  const auto &fi = m_intervals.at(idx);
  return QStringLiteral("%1\n相: %2\n亚: %3\n微: %4")
      .arg(title(), fi.majorFacies, fi.subFacies, fi.microFacies);
}

} // namespace WellComposite
