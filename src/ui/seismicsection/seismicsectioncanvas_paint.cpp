// 层：视图
// 方向 65：画布绘制主体 TU——paintEvent 整段搬来（776 行单体）。
// 刻意不把它切成私有 helper：那要改类定义，与「公共 API 冻结」红线冲突。
// 绘制顺序（底图 → 井/轨迹/合成 → 十字线 → 解释叠加 → 标尺 → 色标 → 深度轴）
// 逐段原样保留。
// token 例外：DESIGN 数据符号例外：地震振幅密度/wiggle 图像、拾取置信度/断层/井曲线及图像上交互标记，保持地震数据视觉映射。（tools/ui-token-exceptions.json 精确计数）。
#include "ui/seismicsection/seismicsectioncanvas.h"
#include "domain/seismic/sectionaxis.h"

#include "ui/paleotheme.h"

#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <limits>

namespace seismic {

void SeismicSectionCanvas::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // chrome 色每次 paint 现取 tokens()——主题切换后 update() 即跟随。
    const auto &tok = PaleoTheme::tokens();
    const QRect vp = viewportRect();
    QFont monoFont = PaleoTheme::monoFont();
    monoFont.setPointSize(PaleoTheme::tokens().labelPt);
    QFont bodyFont = PaleoTheme::bodyFont();
    bodyFont.setPointSize(PaleoTheme::tokens().labelPt);

    // 1. Clear background
    p.fillRect(rect(), tok.surface);

    // 2. Render seismic image inside viewport（D2.6 缩小时走 LOD 抽稀图）
    p.save();
    p.setClipRect(vp);

    if (hasData() && !m_cachedImage.isNull()) {
        const double x0 = traceToPixelX(0.0);
        const double y0 = timeToPixelY(m_t0Ms);
        const double x1 = traceToPixelX(static_cast<double>(m_traces));
        const double y1 = timeToPixelY(m_t0Ms + static_cast<double>(m_samples) * m_dtMs);
        const QRectF imgDest(x0, y0, x1 - x0, y1 - y0);

        const bool drawDensity = m_displayMode != SectionDisplayMode::WiggleVA ||
                                 m_compareEnabled;
        if (drawDensity) {
            if (m_displayMode == SectionDisplayMode::Mixed)
                p.setOpacity(0.42); // 混合：密度淡显打底
            const QImage &img = (m_zoomY < 1.0) ? lodImage() : m_cachedImage;
            p.drawImage(imgDest, img); // 抽稀保全程，目标矩形不变
            p.setOpacity(1.0);
        } else {
            // 纯 wiggle：白底
            p.fillRect(vp, QColor(QStringLiteral("#FFFFFF")));
        }

        // D2.10 卷帘对比：帘左 A（当前）/ 帘右 B（对比图）
        if (m_compareEnabled && !m_compareImage.isNull()) {
            const double curtainPx = vp.left() + m_curtainPos * vp.width();
            p.save();
            p.setClipRect(QRectF(vp.left(), vp.top(), curtainPx - vp.left(), vp.height()));
            const QImage &imgA = (m_zoomY < 1.0) ? lodImage() : m_cachedImage;
            p.drawImage(imgDest, imgA);
            p.restore();

            p.save();
            p.setClipRect(QRectF(curtainPx, vp.top(), vp.right() - curtainPx, vp.height()));
            const double by1 = y0 + (y1 - y0); // B 图与 A 同几何
            p.drawImage(QRectF(x0, y0, x1 - x0, by1 - y0), m_compareImage);
            p.restore();

            // 分割线 + 拖拽柄 + A/B 标签
            p.setPen(QPen(QColor(QStringLiteral("#1B73D0")), 2.0));
            p.drawLine(QPointF(curtainPx, vp.top()), QPointF(curtainPx, vp.bottom()));
            p.setBrush(QColor(QStringLiteral("#1B73D0")));
            p.drawPolygon(QPolygonF({
                QPointF(curtainPx - 7.0, vp.center().y() - 9.0),
                QPointF(curtainPx + 7.0, vp.center().y() - 9.0),
                QPointF(curtainPx + 2.0, vp.center().y()),
                QPointF(curtainPx + 7.0, vp.center().y() + 9.0),
                QPointF(curtainPx - 7.0, vp.center().y() + 9.0),
                QPointF(curtainPx - 2.0, vp.center().y())}));
            p.setFont(bodyFont);
            p.setPen(QColor(QStringLiteral("#24303E")));
            p.drawText(QRectF(vp.left() + 4.0, vp.top() + 2.0, 120.0, 16.0),
                       Qt::AlignLeft, tr("A · %1").arg(m_compareLabel.isEmpty() ? tr("当前") : tr("当前")));
            p.drawText(QRectF(vp.right() - 160.0, vp.top() + 2.0, 156.0, 16.0),
                       Qt::AlignRight, tr("B · %1").arg(m_compareLabel));
        }

        // goal/seismic-attributes：属性叠加层（同几何半透明色层，位于密度
        // 之上、wiggle/解释要素之下——属性读图不遮挡相位轴与拾取）
        if (hasAttrOverlay() && m_attrAlpha > 0.0) {
            p.setOpacity(m_attrAlpha);
            p.drawImage(imgDest, m_attrImage);
            p.setOpacity(1.0);
        }
        // 层段属性叠在地震属性之上。NaN 已在烘焙时写成透明像素。
        if (hasZoneOverlay() && m_zoneAlpha > 0.0) {
            p.setOpacity(m_zoneAlpha);
            p.drawImage(imgDest, m_zoneImage);
            p.setOpacity(1.0);
        }

        // D2.2 wiggle 叠加（WiggleVA 全强 / Mixed 全强叠在淡密度上）
        if (m_displayMode != SectionDisplayMode::Density && !m_compareEnabled)
            paintWiggleOverlay(p, vp, 1.0);
    } else {
        p.setPen(tok.textMuted);
        p.setFont(bodyFont);
        // D2.14：有原因态时展示原因（如「线号 1234 不在测网…」），否则通用占位
        const QString placeholder = m_noDataReason.isEmpty()
            ? tr("未加载地震剖面数据\n（支持拖拽测线或从地图生成连井/任意剖面）")
            : m_noDataReason;
        p.drawText(vp, Qt::AlignCenter | Qt::TextWordWrap, placeholder);
    }

    // 3. Render Wellbores, Tops, and Curves inside viewport
    if (m_showWells && !m_wells.empty() && hasData()) {
        if (m_orientation == SectionOrientation::TimeSlice) {
            // Horizontal slice wellhead markers & penetrated tops
            for (const auto &well : m_wells) {
                double wellXl = 0.0;
                double wellIl = 0.0;
                if (well.surfaceX >= m_xlineMin && well.surfaceX <= m_xlineMax &&
                    well.surfaceY >= m_inlineMin && well.surfaceY <= m_inlineMax) {
                    wellXl = well.surfaceX;
                    wellIl = well.surfaceY;
                } else {
                    const double norm = std::clamp(well.tracePosition / std::max(1, m_traces - 1), 0.0, 1.0);
                    wellXl = m_xlineMin + norm * (m_xlineMax - m_xlineMin);
                    wellIl = (m_inlineMin + m_inlineMax) * 0.5;
                }

                const double colIdx = (wellXl - m_xlineMin) / std::max(1, m_xlineMax - m_xlineMin) * std::max(1, m_traces - 1);
                const double wx = traceToPixelX(colIdx);
                const double wy = inlineToPixelY(wellIl);

                if (wx < vp.left() - 40 || wx > vp.right() + 40 || wy < vp.top() - 40 || wy > vp.bottom() + 40)
                    continue;

                // Borehole target marker (halo, neutral disc, white crosshair)
                p.setPen(QPen(tok.onPrimary, 4.0));
                p.setBrush(tok.text);
                p.drawEllipse(QPointF(wx, wy), 5.5, 5.5);

                p.setPen(QPen(tok.onPrimary, 1.5));
                p.drawLine(QPointF(wx - 4.0, wy), QPointF(wx + 4.0, wy));
                p.drawLine(QPointF(wx, wy - 4.0), QPointF(wx, wy + 4.0));

                // Well tag badge
                p.setFont(monoFont);
                QString wellTag = well.wellName;
                QString nearTop;
                double minDiff = 1e9;
                for (const auto &top : well.tops) {
                    const double diff = std::abs(top.twtMs - m_currentTimeMs);
                    if (diff < minDiff && diff <= 35.0) {
                        minDiff = diff;
                        nearTop = top.topName;
                    }
                }
                if (!nearTop.isEmpty()) {
                    wellTag += QStringLiteral(" [%1]").arg(nearTop);
                }

                const QFontMetrics fm(monoFont);
                const int tw = fm.horizontalAdvance(wellTag);
                const QRectF tagRect(wx + 8.0, wy - 9.0, tw + 8.0, 18.0);

                p.setBrush(tok.surface);
                p.setPen(QPen(tok.border, 1.0));
                p.drawRoundedRect(tagRect, PaleoTheme::tokens().radiusSm, PaleoTheme::tokens().radiusSm);

                p.setPen(tok.text);
                p.drawText(tagRect, Qt::AlignCenter, wellTag);
            }
        } else {
            // D5.7 多井开关：>0 时只保留离剖面最近的 N 口（|offset| 升序）
            std::vector<const SectionWellInfo *> visible;
            visible.reserve(m_wells.size());
            for (const auto &well : m_wells)
                if (well.isWithinBuffer &&
                    std::abs(well.offsetDistanceM) <= m_bufferDistanceM)
                    visible.push_back(&well);
            if (m_maxVisibleWells > 0 && static_cast<int>(visible.size()) > m_maxVisibleWells) {
                std::partial_sort(visible.begin(), visible.begin() + m_maxVisibleWells, visible.end(),
                                  [](const SectionWellInfo *a, const SectionWellInfo *b) {
                                      return std::abs(a->offsetDistanceM) < std::abs(b->offsetDistanceM);
                                  });
                visible.resize(static_cast<std::size_t>(m_maxVisibleWells));
            }

            for (const SectionWellInfo *wellPtr : visible) {
                const auto &well = *wellPtr;

                const double wx = traceToPixelX(well.tracePosition);
                if (wx < vp.left() - 60 || wx > vp.right() + 60)
                    continue;

                // D5.3 井轨迹：底有投影时画斜轨迹线，否则垂直简化；
                // goal/well-trajectory：测斜折线（≥2 顶点）优先于两点简化。
                double wxBot = wx;
                QPolygonF trajPoly;
                for (const WellTrajectory &t : m_wellTrajectories) {
                    if (t.wellId != well.wellId)
                        continue;
                    wxBot = traceToPixelX(t.bottomTracePos);
                    if (t.vertices.size() < 2)
                        break;
                    const double wellTopYv = timeToPixelY(m_t0Ms);
                    for (const TrajVertex &v : t.vertices) {
                        if (!std::isfinite(v.twtMs) || v.twtMs <= 0.0)
                            continue; // 未对齐顶点不画（诚实面，不猜时间）
                        const double vy = std::max(
                            timeToPixelY(v.twtMs),
                            static_cast<double>(vp.top()) - 5.0);
                        trajPoly.append(QPointF(traceToPixelX(v.tracePos),
                                                std::max(vy, wellTopYv)));
                    }
                    break;
                }

                // Draw wellbore trajectory line (halo under neutral ink, 主题 token)
                const double bottomTwt =
                    well.calibrated ? well.bottomTwtMs
                    : well.totalDepth > 0.0
                        ? m_tdModel.DepthToTwtMs(well.totalDepth)
                        : (m_t0Ms + m_samples * m_dtMs);
                const double wellTopY = timeToPixelY(m_t0Ms);
                const double wellBotY = std::min(
                    timeToPixelY(std::isfinite(bottomTwt) ? bottomTwt : m_t0Ms),
                    static_cast<double>(vp.bottom()));

                if (trajPoly.size() >= 2) {
                    p.setPen(QPen(tok.onPrimary, 4.0));
                    p.drawPolyline(trajPoly);
                    p.setPen(QPen(tok.text, 2.0));
                    p.drawPolyline(trajPoly);
                } else {
                    p.setPen(QPen(tok.onPrimary, 4.0));
                    p.drawLine(QPointF(wx, wellTopY), QPointF(wxBot, wellBotY));

                    p.setPen(QPen(tok.text, 2.0));
                    p.drawLine(QPointF(wx, wellTopY), QPointF(wxBot, wellBotY));
                }

                // D5.4 合成记录 overlay：井位旁的合成道（红波形 + 褶积振幅）
                for (const SyntheticOverlay &syn : m_syntheticOverlays) {
                    if (syn.wellId != well.wellId || syn.twtMs.empty())
                        continue;
                    if (!syn.ok) {
                        // 降级注记：写明原因（如「密度曲线缺失」）
                        p.setFont(PaleoTheme::bodyFont(PaleoTheme::tokens().labelPt));
                        p.setPen(QColor(QStringLiteral("#F29900")));
                        p.drawText(QRectF(wx + 8.0, wellTopY + 4.0, 150.0, 30.0),
                                   Qt::AlignLeft | Qt::TextWordWrap,
                                   tr("合成记录不可用：%1").arg(syn.reason));
                        continue;
                    }
                    const double halfW = 16.0;
                    QPolygonF synWave;
                    for (std::size_t i = 0; i < syn.twtMs.size(); ++i) {
                        const double py = timeToPixelY(syn.twtMs[i]);
                        if (py < vp.top() - 5 || py > vp.bottom() + 5)
                            continue;
                        synWave.append(QPointF(wx + 44.0 + syn.amplitude[i] * halfW, py));
                    }
                    if (synWave.size() >= 2) {
                        p.setPen(QPen(QColor(220, 38, 38), 1.4));
                        p.setBrush(Qt::NoBrush);
                        p.drawPolyline(synWave);
                    }
                }

                // Formation tops
                if (m_showTops) {
                    for (const auto &top : well.tops) {
                        if (!std::isfinite(top.twtMs))
                            continue;
                        const double ty = timeToPixelY(top.twtMs);
                        if (ty < vp.top() || ty > vp.bottom())
                            continue;

                        // Horizontal top cross tick
                        p.setPen(QPen(tok.onPrimary, 4.0));
                        p.drawLine(QPointF(wx - 8.0, ty), QPointF(wx + 8.0, ty));
                        p.setPen(QPen(top.color.isValid() ? top.color : tok.textMuted, 2.0));
                        p.drawLine(QPointF(wx - 8.0, ty), QPointF(wx + 8.0, ty));

                        // Marker label
                        p.setFont(monoFont);
                        const QString tagText = top.topName;
                        const QFontMetrics fm(monoFont);
                        const int tw = fm.horizontalAdvance(tagText);
                        const QRectF tagRect(wx + 10.0, ty - 8.0, tw + 8.0, 16.0);

                        p.setBrush(tok.surfaceAltRaised);
                        p.setPen(QPen(tok.border, 1.0));
                        p.drawRoundedRect(tagRect, PaleoTheme::tokens().radiusSm, PaleoTheme::tokens().radiusSm);

                        p.setPen(tok.text);
                        p.drawText(tagRect, Qt::AlignCenter, tagText);
                    }
                }

                // Well log curves
                if (m_showCurves && !well.curves.empty()) {
                    const double trackWidth = 36.0;
                    for (const auto &curve : well.curves) {
                        if (curve.values.empty() || curve.twtMs.size() != curve.values.size())
                            continue;

                        const float span = std::max(1e-4f, curve.maxVal - curve.minVal);
                        QPolygonF poly;
                        poly.reserve(static_cast<int>(curve.values.size()));

                        for (std::size_t i = 0; i < curve.values.size(); ++i) {
                            const float val = curve.values[i];
                            if (!std::isfinite(val) ||
                                !std::isfinite(curve.twtMs[i])) {
                              p.setPen(QPen(curve.color, 1.5));
                              p.drawPolyline(poly);
                              poly.clear();
                              continue;
                            }
                            const double cy = timeToPixelY(curve.twtMs[i]);
                            if (cy < vp.top() - 10 || cy > vp.bottom() + 10)
                                continue;
                            const double norm = std::clamp(static_cast<double>((val - curve.minVal) / span), 0.0, 1.0);
                            const double cx = wx + 4.0 + norm * trackWidth;
                            poly.append(QPointF(cx, cy));
                        }

                        if (poly.size() >= 2) {
                            p.setPen(QPen(curve.color.isValid() ? curve.color : QColor(QStringLiteral("#43A047")), 1.5));
                            p.setBrush(Qt::NoBrush);
                            p.drawPolyline(poly);
                        }
                    }
                }
            }
        }
    }

    // 4. Crosshairs inside viewport
    if (m_hasHover && vp.contains(m_currentMousePos)) {
        QColor cross = tok.focusRing;
        cross.setAlpha(140);
        p.setPen(QPen(cross, 1.0, Qt::DashLine));
        p.drawLine(QPointF(vp.left(), m_currentMousePos.y()), QPointF(vp.right(), m_currentMousePos.y()));
        p.drawLine(QPointF(m_currentMousePos.x(), vp.top()), QPointF(m_currentMousePos.x(), vp.bottom()));
    }

    // D4.10/D4.4：解释叠加——拾取点按置信度着色 + 断层折线 + 绘制中草稿
    for (const SeismicPick &pick : m_pickOverlays) {
        // 只画属于当前剖面的拾取（IL 剖面：index 匹配 + 列=XL）
        double px = -1.0, py = -1.0;
        if (m_sectionRef.valid && m_sectionRef.type == SgySliceType::Inline &&
            pick.inlineNo == m_sectionRef.index) {
            const int col = sectionColumnForLine(m_sectionRef.colLines, m_sectionRef.colMin,
                                                 pick.xlineNo);
            if (col < 0 || col >= m_traces) continue;
            px = traceToPixelX(col);
            py = (m_orientation == SectionOrientation::TimeSlice) ? -1.0 : timeToPixelY(pick.twtMs);
        } else if (m_sectionRef.valid && m_sectionRef.type == SgySliceType::Xline &&
                   pick.xlineNo == m_sectionRef.index) {
            const int col = sectionColumnForLine(m_sectionRef.colLines, m_sectionRef.colMin,
                                                 pick.inlineNo);
            if (col < 0 || col >= m_traces) continue;
            px = traceToPixelX(col);
            py = (m_orientation == SectionOrientation::TimeSlice) ? -1.0 : timeToPixelY(pick.twtMs);
        }
        if (px < vp.left() - 20 || px > vp.right() + 20 || py < vp.top() - 20 || py > vp.bottom() + 20)
            continue;
        if (py < 0) continue;
        // D4.10 置信度着色：绿(1.0)→黄→红(<0.5)
        const QColor confColor = pick.confidence >= 0.75
            ? QColor(67, 160, 71)
            : (pick.confidence >= 0.5 ? QColor(242, 153, 0) : QColor(229, 57, 53));
        p.setPen(QPen(QColor(Qt::white), 3.0));
        p.setBrush(confColor);
        p.drawEllipse(QPointF(px, py), 4.2, 4.2);
        p.setPen(QPen(confColor.darker(160), 1.4));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(px, py), 5.4, 5.4);
    }
    // 断层：实线段（存档的按剖面匹配）+ 草稿虚线
    p.setBrush(Qt::NoBrush);
    for (const SeismicFaultSegment &seg : m_faultOverlays) {
        if (!m_sectionRef.valid || seg.sectionType != m_sectionRef.type ||
            seg.sectionIndex != m_sectionRef.index)
            continue;
        QPolygonF poly;
        for (const auto &pt : seg.points)
            poly.append(QPointF(traceToPixelX(pt.first * std::max(1, m_traces - 1)),
                                timeToPixelY(pt.second)));
        if (poly.size() >= 2) {
            p.setPen(QPen(QColor(229, 57, 53), 2.2));
            p.drawPolyline(poly);
        }
    }
    if (m_faultDraft.size() >= 2) {
        QPolygonF draft;
        for (const auto &pt : m_faultDraft)
            draft.append(QPointF(traceToPixelX(pt.first), timeToPixelY(pt.second)));
        p.setPen(QPen(QColor(229, 57, 53, 180), 2.0, Qt::DashLine));
        p.drawPolyline(draft);
    }
    // FaultSet 断层棒（goal/fault-interpretation）：剖面身份已在 dock 侧
    // 过滤；选中断层加白色 halo（拾取点白描边同款视觉语言）+ 线宽提亮。
    for (const FaultStickDisplay &stick : m_faultStickOverlays) {
        QPolygonF poly;
        for (const auto &pt : stick.points)
            poly.append(QPointF(traceToPixelX(pt.first * std::max(1, m_traces - 1)),
                                timeToPixelY(pt.second)));
        if (poly.size() < 2)
            continue;
        if (stick.highlighted) {
            p.setPen(QPen(QColor(Qt::white), 4.6));
            p.drawPolyline(poly);
            p.setPen(QPen(QColor(229, 57, 53), 2.6));
        } else {
            p.setPen(QPen(QColor(229, 57, 53), 2.2));
        }
        p.drawPolyline(poly);
    }
    // 断面交线：#F29900 虚线（DESIGN warning，派生结果待复核），叠在拾取棒之上。
    if (m_faultSurfaceCut.visible && m_faultSurfaceCut.points.size() >= 2) {
        QPolygonF cut;
        for (const auto &pt : m_faultSurfaceCut.points)
            cut.append(QPointF(traceToPixelX(pt.first * std::max(1, m_traces - 1)), timeToPixelY(pt.second)));
        p.setPen(QPen(QColor(242, 153, 0), 2.0, Qt::DashLine));
        p.drawPolyline(cut);
    }

    p.restore();

    // 5. Render Top Horizontal Ruler (Distance & Well Pins)
    const QRect topRulerRect(m_leftMargin, 0, vp.width(), m_topMargin);
    p.fillRect(topRulerRect, tok.surfaceAlt);
    p.setPen(tok.border);
    p.drawLine(QPoint(m_leftMargin, m_topMargin), QPoint(vp.right(), m_topMargin));

    if (hasData()) {
        if (m_orientation == SectionOrientation::TimeSlice) {
            // Horizontal Crossline (XL) ruler
            const double minTrace = pixelToTrace(m_leftMargin);
            const double maxTrace = pixelToTrace(vp.right());
            const double spanXl = std::max(1, m_xlineMax - m_xlineMin);
            const double minXl = m_xlineMin + (minTrace / std::max(1.0, static_cast<double>(m_traces - 1))) * spanXl;
            const double maxXl = m_xlineMin + (maxTrace / std::max(1.0, static_cast<double>(m_traces - 1))) * spanXl;

            const auto ticks = NiceStep::GenerateTicks(minXl, maxXl, m_leftMargin, vp.right(), 8, QStringLiteral("%.0f"));

            p.setFont(monoFont);
            for (const auto &tk : ticks) {
                if (tk.pixelPos < m_leftMargin || tk.pixelPos > vp.right())
                    continue;

                p.setPen(tok.textMuted);
                if (tk.isMajor) {
                    p.drawLine(QPointF(tk.pixelPos, m_topMargin - 12.0), QPointF(tk.pixelPos, m_topMargin));
                    const QString xlLabel = QStringLiteral("XL %1").arg(qRound(tk.value));
                    const QFontMetrics fm(monoFont);
                    const int tw = fm.horizontalAdvance(xlLabel);
                    p.setPen(tok.text);
                    p.drawText(QPointF(tk.pixelPos - tw * 0.5, m_topMargin - 16.0), xlLabel);
                } else {
                    p.drawLine(QPointF(tk.pixelPos, m_topMargin - 6.0), QPointF(tk.pixelPos, m_topMargin));
                }
            }
        } else {
            // Distance ticks for vertical profile
            const double minTrace = pixelToTrace(m_leftMargin);
            const double maxTrace = pixelToTrace(vp.right());

            // Distance ticks
            auto distanceAt = [this](double trace) {
              if (m_columnDistances.size() < 2)
                return 0.0;
              const int i = std::clamp(int(std::floor(trace)), 0, m_traces - 2);
              return m_columnDistances[i] +
                     (trace - i) *
                         (m_columnDistances[i + 1] - m_columnDistances[i]);
            };
            auto traceAt = [this](double distance) {
              if (m_columnDistances.size() < 2)
                return 0.0;
              auto it = std::lower_bound(m_columnDistances.begin(),
                                         m_columnDistances.end(), distance);
              const int i =
                  std::clamp(int(std::distance(m_columnDistances.begin(), it)),
                             1, m_traces - 1);
              const double d0 = m_columnDistances[i - 1],
                           d1 = m_columnDistances[i];
              return i - 1 + (d1 > d0 ? (distance - d0) / (d1 - d0) : 0);
            };
            const double minDistM = distanceAt(minTrace),
                         maxDistM = distanceAt(maxTrace);

            const auto ticks = NiceStep::GenerateTicks(minDistM, maxDistM, m_leftMargin, vp.right(), 8, QStringLiteral("%.0f"));

            p.setFont(monoFont);
            for (const auto &tk : ticks) {
              const double tickX = traceToPixelX(traceAt(tk.value));
              if (tickX < m_leftMargin || tickX > vp.right())
                continue;

              p.setPen(tok.textMuted);
              if (tk.isMajor) {
                p.drawLine(QPointF(tickX, m_topMargin - 12.0),
                           QPointF(tickX, m_topMargin));
                QString distLabel;
                if (std::abs(tk.value) >= 1000.0) {
                  distLabel =
                      QStringLiteral("%1 km").arg(tk.value / 1000.0, 0, 'f', 1);
                } else {
                  distLabel = QStringLiteral("%1 m").arg(qRound(tk.value));
                }
                const QFontMetrics fm(monoFont);
                const int tw = fm.horizontalAdvance(distLabel);
                p.setPen(tok.text);
                p.drawText(QPointF(tickX - tw * 0.5, m_topMargin - 16.0),
                           distLabel);
              } else {
                p.drawLine(QPointF(tickX, m_topMargin - 6.0),
                           QPointF(tickX, m_topMargin));
              }
            }

            // Well indicator flags on top ruler
            if (m_showWells) {
                for (const auto &well : m_wells) {
                  if (!well.isWithinBuffer ||
                      std::abs(well.offsetDistanceM) > m_bufferDistanceM)
                    continue;
                  const double wx = traceToPixelX(well.tracePosition);
                  if (wx < m_leftMargin || wx > vp.right())
                    continue;

                    // Triangle pin pointing down
                    const QPolygonF triangle({
                        QPointF(wx - 5.0, m_topMargin - 8.0),
                        QPointF(wx + 5.0, m_topMargin - 8.0),
                        QPointF(wx, m_topMargin - 1.0)
                    });
                    p.setBrush(tok.text);
                    p.setPen(Qt::NoPen);
                    p.drawPolygon(triangle);

                  // Capsule label
                  p.setFont(bodyFont);
                  QString pinText = std::abs(well.offsetDistanceM) > 1.0
                                        ? QStringLiteral("%1 (%2m)")
                                              .arg(well.wellName)
                                              .arg(qRound(well.offsetDistanceM))
                                        : well.wellName;
                  if (!well.alignmentStatus.isEmpty())
                    pinText += " · " + well.alignmentStatus;
                  const QFontMetrics fm(bodyFont);
                  const int tw = fm.horizontalAdvance(pinText);
                  const QRectF badge(wx - tw * 0.5 - 4.0, 4.0, tw + 8.0, 18.0);

                    p.setBrush(tok.surfaceAltRaised);
                    p.setPen(QPen(tok.border, 1.0));
                    p.drawRoundedRect(badge, PaleoTheme::tokens().radiusSm, PaleoTheme::tokens().radiusSm);

                    p.setPen(tok.text);
                    p.drawText(badge, Qt::AlignCenter, pinText);
                }
            }
        }
    }

    // 6. Render Left Vertical Ruler (TWT ms or Depth m, or Inline for TimeSlice)
    const QRect leftRulerRect(0, m_topMargin, m_leftMargin, vp.height());
    p.fillRect(leftRulerRect, tok.surfaceAlt);
    p.setPen(tok.border);
    p.drawLine(QPoint(m_leftMargin, m_topMargin), QPoint(m_leftMargin, height()));

    if (hasData()) {
        if (m_orientation == SectionOrientation::TimeSlice) {
            // Vertical Inline (IL) ruler
            const double topIl = pixelToInline(m_topMargin);
            const double botIl = pixelToInline(height());

            const auto ticks = NiceStep::GenerateTicks(std::min(topIl, botIl), std::max(topIl, botIl),
                                                       m_topMargin, height(), 8, QStringLiteral("%.0f"));

            p.setFont(monoFont);
            for (const auto &tk : ticks) {
                const double py = inlineToPixelY(tk.value);
                if (py < m_topMargin || py > height())
                    continue;

                p.setPen(tok.textMuted);
                if (tk.isMajor) {
                    p.drawLine(QPointF(m_leftMargin - 10.0, py), QPointF(m_leftMargin, py));
                    const QString label = QStringLiteral("IL %1").arg(qRound(tk.value));
                    const QFontMetrics fm(monoFont);
                    const int tw = fm.horizontalAdvance(label);
                    p.setPen(tok.text);
                    p.drawText(QPointF(m_leftMargin - 14.0 - tw, py + 4.0), label);
                } else {
                    p.drawLine(QPointF(m_leftMargin - 5.0, py), QPointF(m_leftMargin, py));
                }
            }

            // Top-left corner box
            p.fillRect(QRect(0, 0, m_leftMargin, m_topMargin), tok.surfaceAlt);
            p.setPen(tok.border);
            p.drawRect(QRect(0, 0, m_leftMargin, m_topMargin));

            p.setFont(bodyFont);
            p.setPen(tok.text);
            const QString cornerStr = m_vertUnit == SectionVerticalUnit::TwoWayTimeMs
                ? tr("时间切片\n%1 ms").arg(m_currentTimeMs, 0, 'f', 1)
                : tr("深度切片\n%1 m").arg(m_tdModel.TwtMsToDepth(m_currentTimeMs), 0, 'f', 1);
            p.drawText(QRect(2, 2, m_leftMargin - 4, m_topMargin - 4), Qt::AlignCenter, cornerStr);
        } else {
            const double minTime = pixelToTime(m_topMargin);
            const double maxTime = pixelToTime(height());

            p.setFont(monoFont);
            if (m_vertUnit == SectionVerticalUnit::TwoWayTimeMs) {
                const auto ticks = NiceStep::GenerateTicks(minTime, maxTime, m_topMargin, height(), 8, QStringLiteral("%.0f"));
                for (const auto &tk : ticks) {
                    if (tk.pixelPos < m_topMargin || tk.pixelPos > height())
                        continue;

                    p.setPen(tok.textMuted);
                    if (tk.isMajor) {
                        p.drawLine(QPointF(m_leftMargin - 10.0, tk.pixelPos), QPointF(m_leftMargin, tk.pixelPos));
                        const QString label = QStringLiteral("%1").arg(qRound(tk.value));
                        const QFontMetrics fm(monoFont);
                        const int tw = fm.horizontalAdvance(label);
                        p.setPen(tok.text);
                        p.drawText(QPointF(m_leftMargin - 14.0 - tw, tk.pixelPos + 4.0), label);
                    } else {
                        p.drawLine(QPointF(m_leftMargin - 5.0, tk.pixelPos), QPointF(m_leftMargin, tk.pixelPos));
                    }
                }
            } else {
                // Depth unit — 反投影贴准（与 D2.5 右缘深度轴同法）：深度刻度先
                // 反解 TWT 再取像素，非常速（校验炮分段/压实）模型下刻度间距
                // 如实非线性，不再按常速线性近似。
                const double minDepth = m_tdModel.TwtMsToDepth(minTime);
                const double maxDepth = m_tdModel.TwtMsToDepth(maxTime);
                const auto ticks = NiceStep::GenerateTicks(std::min(minDepth, maxDepth), std::max(minDepth, maxDepth),
                                                           m_topMargin, height(), 8, QStringLiteral("%.0f"));
                for (const auto &tk : ticks) {
                    const double py = timeToPixelY(m_tdModel.DepthToTwtMs(tk.value));
                    if (py < m_topMargin || py > height())
                        continue;

                    p.setPen(tok.textMuted);
                    if (tk.isMajor) {
                        p.drawLine(QPointF(m_leftMargin - 10.0, py), QPointF(m_leftMargin, py));
                        const QString label = QStringLiteral("%1").arg(qRound(tk.value));
                        const QFontMetrics fm(monoFont);
                        const int tw = fm.horizontalAdvance(label);
                        p.setPen(tok.text);
                        p.drawText(QPointF(m_leftMargin - 14.0 - tw, py + 4.0), label);
                    } else {
                        p.drawLine(QPointF(m_leftMargin - 5.0, py), QPointF(m_leftMargin, py));
                    }
                }
            }

            // Axis unit label in top-left corner box
            p.fillRect(QRect(0, 0, m_leftMargin, m_topMargin), tok.surfaceAlt);
            p.setPen(tok.border);
            p.drawRect(QRect(0, 0, m_leftMargin, m_topMargin));

            p.setFont(bodyFont);
            p.setPen(tok.textMuted);
            const QString unitStr =
                m_vertUnit == SectionVerticalUnit::TwoWayTimeMs
                    ? tr("TWT (ms)")
                    : tr("深度\n(m)");
            p.drawText(QRect(2, 2, m_leftMargin - 4, m_topMargin - 4), Qt::AlignCenter, unitStr);
        }
    }

    // 7. Render Right Color Bar (色标)
    const int colorBarX = width() - m_rightMargin;
    const QRect rightBarRect(colorBarX, 0, m_rightMargin, height());
    p.fillRect(rightBarRect, tok.surfaceAlt);
    p.setPen(tok.border);
    p.drawLine(QPoint(colorBarX, 0), QPoint(colorBarX, height()));

    // Title at the top of the color bar
    QFont barTitleFont = bodyFont;
    barTitleFont.setBold(true);
    p.setFont(barTitleFont);
    p.setPen(tok.text);
    p.drawText(QRect(colorBarX, 8, m_rightMargin, 16), Qt::AlignCenter, tr("色标"));

    p.setFont(bodyFont);
    p.setPen(tok.textMuted);
    p.drawText(QRect(colorBarX, 24, m_rightMargin, 14), Qt::AlignCenter, tr("振幅"));

    if (hasData()) {
        const int barW = 12;
        const int barLeft = colorBarX + 8;
        const int barTop = m_topMargin + 12;
        const int barBottom = height() - 24;
        const int barH = std::max(20, barBottom - barTop);

        // D2.8：色标渐变直接采样当前 LUT（8 预设 + 反转全同步）
        if (m_colorLut.empty())
            rebuildColorLut();
        QLinearGradient grad(barLeft, barTop, barLeft, barBottom);
        for (int i = 0; i <= 8; ++i) {
            const int idx = 255 - i * 255 / 8; // 顶=+Peak
            grad.setColorAt(i / 8.0, QColor(m_colorLut[static_cast<std::size_t>(idx)]));
        }

        const QRectF colorBarRect(barLeft, barTop, barW, barH);
        p.setBrush(grad);
        p.setPen(QPen(tok.border, 1.0));
        p.drawRoundedRect(colorBarRect, PaleoTheme::tokens().radiusSm, PaleoTheme::tokens().radiusSm);

        // Labels next to the bar
        p.setFont(monoFont);
        p.setPen(tok.text);

        const float absMax = std::max(std::abs(m_slice.valueMin), std::abs(m_slice.valueMax));
        const QString maxStr = absMax >= 10000.0f
            ? QStringLiteral("+%1k").arg(absMax / 1000.0f, 0, 'f', 0)
            : (absMax >= 1000.0f ? QStringLiteral("+%1k").arg(absMax / 1000.0f, 0, 'f', 1)
                                 : QStringLiteral("+%1").arg(qRound(absMax)));
        const QString minStr = absMax >= 10000.0f
            ? QStringLiteral("-%1k").arg(absMax / 1000.0f, 0, 'f', 0)
            : (absMax >= 1000.0f ? QStringLiteral("-%1k").arg(absMax / 1000.0f, 0, 'f', 1)
                                 : QStringLiteral("-%1").arg(qRound(absMax)));

        // Top tick (+Max)
        p.drawLine(QPointF(barLeft + barW, barTop), QPointF(barLeft + barW + 3, barTop));
        p.drawText(QRectF(barLeft + barW + 4, barTop - 7, m_rightMargin - barW - 12, 14), Qt::AlignLeft | Qt::AlignVCenter, maxStr);

        // Mid tick (0)
        const double midY = barTop + barH * 0.5;
        p.drawLine(QPointF(barLeft + barW, midY), QPointF(barLeft + barW + 3, midY));
        p.drawText(QRectF(barLeft + barW + 4, midY - 7, m_rightMargin - barW - 12, 14), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("0"));

        // Bottom tick (-Max)
        p.drawLine(QPointF(barLeft + barW, barBottom), QPointF(barLeft + barW + 3, barBottom));
        p.drawText(QRectF(barLeft + barW + 4, barBottom - 7, m_rightMargin - barW - 12, 14), Qt::AlignLeft | Qt::AlignVCenter, minStr);

        // Peak / Trough text annotations（色=数据符号色，不属 chrome token）
        p.setFont(bodyFont);
        if (m_colorMap == SectionColorMapType::RedWhiteBlue) {
            p.setPen(PaleoTheme::tokens().textMuted);
            p.drawText(QRectF(colorBarX, barTop - 13, m_rightMargin - 6, 12), Qt::AlignRight, tr("波峰+"));
            p.setPen(PaleoTheme::tokens().textMuted);
            p.drawText(QRectF(colorBarX, barBottom + 3, m_rightMargin - 6, 12), Qt::AlignRight, tr("波谷-"));
        }
    }

    // D2.5 双刻度：右缘深度轴（TWT 主轴在左）——时深模型无效时自动隐藏
    if (m_dualScale && m_orientation == SectionOrientation::Vertical && hasData() &&
        m_tdModel.isValid()) {
        const int depthAxisX = vp.right() + 4;
        p.setFont(monoFont);
        const double minTime = pixelToTime(m_topMargin);
        const double maxTime = pixelToTime(height());
        const double minDepth = m_tdModel.TwtMsToDepth(minTime);
        const double maxDepth = m_tdModel.TwtMsToDepth(maxTime);
        const auto depthTicks = NiceStep::GenerateTicks(std::min(minDepth, maxDepth),
                                                         std::max(minDepth, maxDepth),
                                                         m_topMargin, height(), 8, QStringLiteral("%.0f"));
        p.setPen(PaleoTheme::tokens().textMuted);
        p.drawText(QRect(depthAxisX, m_topMargin - 16, m_rightMargin - 8, 14),
                   Qt::AlignLeft, tr("深度(m)"));
        for (const auto &tk : depthTicks) {
            if (tk.pixelPos < m_topMargin || tk.pixelPos > height())
                continue;
            // 反算该深度对应 TWT 的像素位置（非线性时深时贴准）
            const double twt = m_tdModel.DepthToTwtMs(tk.value);
            const double py = timeToPixelY(twt);
            if (py < m_topMargin || py > height())
                continue;
            p.setPen(PaleoTheme::tokens().successText);
            p.drawLine(QPointF(depthAxisX, py), QPointF(depthAxisX + 4.0, py));
            p.setPen(PaleoTheme::tokens().textMuted);
            p.drawText(QRectF(depthAxisX + 5.0, py - 6.0, 26.0, 12.0), Qt::AlignLeft,
                       QStringLiteral("%1").arg(qRound(tk.value)));
        }
    }}

} // namespace seismic
