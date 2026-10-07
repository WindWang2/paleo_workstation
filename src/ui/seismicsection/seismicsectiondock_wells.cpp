// 层：视图
// 方向 65：D5 井震与任意线 TU——井旁道小图控件、井轨迹投影、合成地震记录、
// 任意线路径编辑器。投影/定位口径（#129/#146/#147）逐条照搬。
// token 例外：DESIGN 数据符号例外：井旁道的分层位置标记，与数据图像使用原有蓝色。（tools/ui-token-exceptions.json 精确计数）。
#include "ui/seismicsection/seismicsectiondockwidget.h"

#include "domain/seismic/sgycoordinatemapper.h"
#include "domain/seismic/welltracelocate.h"
#include "services/seismictaskservice.h"

#include "ui/paleotheme.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include "../notifications/paleonotify.h"
#include <QPainter>
#include <QRegularExpression>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace seismic {

namespace {

// ---- D4 解释工具 ---------------------------------------------------------------

// D5.6 井旁道 wiggle 小图（匿名命名空间——本文件局部）
class WellSideTraceWidget : public QWidget {
public:
    WellSideTraceWidget(const SectionWellInfo *well, int col, const SgySliceImage &slice,
                        float dtMs, double t0Ms, QWidget *parent)
        : QWidget(parent), m_well(well), m_dtMs(dtMs > 0.01f ? dtMs : 2.0f), m_t0Ms(t0Ms)
    {
        const int h = slice.height;
        m_trace.reserve(std::size_t(h));
        for (int y = 0; y < h; ++y)
            m_trace.push_back(slice.Value(col, y));
        setMinimumSize(260, 460);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), PaleoTheme::tokens().surface);
        if (m_trace.empty())
            return;
        float maxAbs = 1e-6f;
        for (float v : m_trace)
            if (std::isfinite(v))
                maxAbs = std::max(maxAbs, std::abs(v));
        const double halfW = width() * 0.32;
        const double cx = width() * 0.5;
        const double y0 = 24.0, y1 = height() - 8.0;
        p.setPen(QPen(PaleoTheme::tokens().border, 1.0));
        p.drawLine(QPointF(cx, y0), QPointF(cx, y1));
        QPolygonF wave;
        QPolygonF fill;
        bool fillOpen = false;
        for (std::size_t i = 0; i < m_trace.size(); ++i) {
            const float v = m_trace[i];
            const double py = y0 + double(i) / double(m_trace.size() - 1) * (y1 - y0);
            const double amp = std::isfinite(v) ? std::clamp(double(v) / maxAbs, -1.0, 1.0) : 0.0;
            const double px = cx + amp * halfW;
            wave.append(QPointF(px, py));
            if (amp > 0.0) {
                if (!fillOpen) { fill.append(QPointF(cx, py)); fillOpen = true; }
                fill.append(QPointF(px, py));
            } else if (fillOpen) {
                fill.append(QPointF(cx, py));
                fillOpen = false;
            }
        }
        if (fill.size() >= 3) {
            p.setPen(Qt::NoPen);
            p.setBrush(PaleoTheme::tokens().text);
            p.drawPolygon(fill);
        }
        p.setPen(QPen(PaleoTheme::tokens().text, 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(wave);
        // 分层刻度（右缘）
        p.setFont(PaleoTheme::bodyFont(PaleoTheme::tokens().labelPt));
        for (const WellTopItem &top : m_well->tops) {
            // 道首样 TWT = 记录延迟 t0（#146 同口径）
            const double frac = std::clamp((top.twtMs - m_t0Ms) / double(m_trace.size() * m_dtMs),
                                           0.0, 1.0);
            const double py = y0 + frac * (y1 - y0);
            p.setPen(QPen(QColor(QStringLiteral("#1B73D0")), 1.4));
            p.drawLine(QPointF(width() - 46.0, py), QPointF(width() - 6.0, py));
            p.setPen(PaleoTheme::tokens().textMuted);
            p.drawText(QRectF(width() - 90.0, py - 7.0, 46.0, 14.0), Qt::AlignRight, top.topName);
        }
    }

private:
    const SectionWellInfo *m_well = nullptr;
    std::vector<float> m_trace;
    float m_dtMs = 2.0f;
    double m_t0Ms = 0.0;
};

} // namespace

// ---- D5 井震与任意线 -----------------------------------------------------------

void SeismicSectionDockWidget::setCandidateWells(const std::vector<SectionWellInfo> &wells) {
    m_candidateWells = wells;
    computeSyntheticOverlays();
}

// 点到折线的最近投影（剖面横向比例 0..1）
static double ProjectPointOntoPolyline(const std::vector<glm::dvec2> &poly,
                                       const glm::dvec2 &pt, double *offsetOut = nullptr) {
    double bestT = 0.0, bestDist = std::numeric_limits<double>::max(), bestOffset = 0.0;
    double total = 0.0;
    for (std::size_t i = 1; i < poly.size(); ++i) {
        const glm::dvec2 a = poly[i - 1], b = poly[i];
        const glm::dvec2 ab = b - a;
        const double len2 = glm::dot(ab, ab);
        const double t = len2 > 1e-12 ? std::clamp(glm::dot(pt - a, ab) / len2, 0.0, 1.0) : 0.0;
        const glm::dvec2 proj = a + ab * t;
        const double d = glm::length(pt - proj);
        if (d < bestDist) {
            bestDist = d;
            bestT = (total + t * std::sqrt(len2));
            bestOffset = d;
        }
        total += std::sqrt(len2);
    }
    if (offsetOut)
        *offsetOut = bestOffset;
    return total > 1e-9 ? bestT / total : 0.0;
}

void SeismicSectionDockWidget::computeWellTrajectories(const std::vector<glm::dvec2> &mapPolyline) {
    m_lastMapPolyline = mapPolyline;
    std::vector<SeismicSectionCanvas::WellTrajectory> trajectories;
    if (mapPolyline.size() >= 2) {
        for (const SectionWellInfo &well : m_candidateWells) {
            SeismicSectionCanvas::WellTrajectory t;
            t.wellId = well.wellId;
            t.topTracePos = ProjectPointOntoPolyline(mapPolyline, {well.surfaceX, well.surfaceY});
            const glm::dvec2 bottom(well.bottomX != 0.0 ? well.bottomX : well.surfaceX,
                                    well.bottomY != 0.0 ? well.bottomY : well.surfaceY);
            t.bottomTracePos = ProjectPointOntoPolyline(mapPolyline, bottom);
            t.topTwtMs = 0.0;
            t.bottomTwtMs = m_canvas->timeDepthModel().DepthToTwtMs(well.totalDepth);
            // goal/well-trajectory：测斜站逐点投影成折线（twt 用 workbench
            // 逐站校准值；未对齐站回退画布时深模型，再不行顶点不画）。
            const TimeDepthModel &td = m_canvas->timeDepthModel();
            for (const WellTrajSample &s : well.trajectory) {
                SeismicSectionCanvas::TrajVertex v;
                v.tracePos = ProjectPointOntoPolyline(mapPolyline, {s.x, s.y});
                v.twtMs = std::isfinite(s.twtMs) && s.twtMs > 0.0
                              ? s.twtMs
                              : (s.tvd > 0.0 ? td.DepthToTwtMs(s.tvd) : s.twtMs);
                t.vertices.push_back(v);
            }
            trajectories.push_back(t);
        }
    }
    m_canvas->setWellTrajectories(trajectories);

    // D5.3 原因态：时深无实测检查点 → 状态栏注记（仍按均速投影）
    if (!m_candidateWells.empty() && !m_canvas->timeDepthModel().hasCheckshots())
        setLineTitle(tr("%1（时深用默认均速——无实测检查点表）").arg(m_lblTitle->text()));
}

void SeismicSectionDockWidget::computeSyntheticOverlays() {
    std::vector<SeismicSectionCanvas::SyntheticOverlay> overlays;
    const TimeDepthModel &td = m_canvas->timeDepthModel();
    for (const SectionWellInfo &well : m_candidateWells) {
        SeismicSectionCanvas::SyntheticOverlay ov;
        ov.wellId = well.wellId;
        const WellCurveItem *ac = nullptr;
        const WellCurveItem *den = nullptr;
        for (const WellCurveItem &c : well.curves) {
            if (c.curveName == QLatin1String("AC") && !c.values.empty())
                ac = &c;
            if ((c.curveName == QLatin1String("DEN") || c.curveName == QLatin1String("RHOB")) && !c.values.empty())
                den = &c;
        }
        if (!ac || !den) {
            ov.reason = !ac ? tr("缺声波曲线 AC") : tr("缺密度曲线 DEN");
            overlays.push_back(ov);
            continue;
        }
        const auto result = SeismicTaskService::computeSyntheticSeismogram(
            ac->depthsM, ac->values, den->depthsM, den->values, td, 25.0);
        ov.ok = result.ok;
        ov.reason = result.reason;
        ov.twtMs = result.twtMs;
        ov.amplitude = result.amplitude;
        overlays.push_back(ov);
    }
    m_canvas->setSyntheticOverlays(overlays);
}

// D5.1 任意线路径编辑器：多段折线（il xl 每行一节点）→ 提取
void SeismicSectionDockWidget::showArbitraryLineEditor() {
    if (!m_volume || !m_volume->IsLoaded()) {
        PaleoNotify::information(this, tr("任意线编辑器"), tr("请先加载地震体。"));
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(tr("任意线编辑器（每行一个节点：inline xline）"));
    dlg.setMinimumSize(420, 320);
    auto *lay = new QVBoxLayout(&dlg);
    auto *edit = new QTextEdit(&dlg);
    edit->setFont(PaleoTheme::monoFont(PaleoTheme::tokens().bodyPt));
    edit->setPlaceholderText(tr("1000 2000\n1002 2005\n1005 2012"));
    lay->addWidget(edit, 1);
    auto *chkWells = new QCheckBox(tr("投影候选井（井震综合）"), &dlg);
    chkWells->setChecked(!m_candidateWells.empty());
    lay->addWidget(chkWells);
    auto *btnBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(btnBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(btnBox);
    if (dlg.exec() != QDialog::Accepted)
        return;

    std::vector<glm::ivec2> pathPoints;
    const auto rows = edit->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &row : rows) {
        const auto parts = row.simplified().split(QRegularExpression(QStringLiteral("[ ,\\t]+")), Qt::SkipEmptyParts);
        if (parts.size() != 2)
            continue;
        bool okA = false, okB = false;
        const int il = parts[0].toInt(&okA);
        const int xl = parts[1].toInt(&okB);
        if (okA && okB)
            pathPoints.push_back({il, xl});
    }
    if (pathPoints.size() < 2) {
        PaleoNotify::warning(this, tr("任意线编辑器"), tr("至少需要 2 个有效节点。"));
        return;
    }
    const std::vector<SectionWellInfo> wells =
        chkWells->isChecked() ? m_candidateWells : std::vector<SectionWellInfo>{};
    extractSectionFromVolumeAsync(
        m_volume, pathPoints, tr("任意线（%1 节点）").arg(pathPoints.size()), {}, wells);
}

// D5.6 井旁道小图：最近井位置的地震道 wiggle + 分层刻度
void SeismicSectionDockWidget::showWellSideTrace() {
    if (!m_volume || !m_volume->IsLoaded() || m_candidateWells.empty()) {
        PaleoNotify::information(this, tr("井旁道"), tr("无可用的候选井。"));
        return;
    }
    // 最近井（离当前剖面最近）
    const SectionWellInfo *best = &m_candidateWells.front();
    for (const SectionWellInfo &w : m_candidateWells)
        if (std::abs(w.offsetDistanceM) < std::abs(best->offsetDistanceM))
            best = &w;
    // 井口 XY → 测网 (IL, XL)（#129：FindNearest* 的参数是测线号，不能直接喂米制坐标；
    // 先经道头拟合的测网仿射换算，再吸附到体的实际线号）。
    if (!m_volume->Index()) {
        PaleoNotify::warning(this, tr("井旁道"), tr("地震体无道头索引，无法把井口坐标换算到测网。"));
        return;
    }
    // 换算/覆盖/吸附/取列统一走 domain/seismic/welltracelocate（tst_welltracelocate
    // 以 IL 步长 2、XL 步长 4、旋转测网钉住）；覆盖余量按实际线距的半步。
    const SgyCoordinateMapper mapper = SgyCoordinateMapper::Fit(*m_volume->Index());
    const seismic::WellTraceLocation loc =
        seismic::locateWellTrace(*m_volume->Index(), mapper, best->surfaceX, best->surfaceY);
    if (!loc.ok && loc.outOfCoverage) {
        PaleoNotify::information(this, tr("井旁道"),
                                 tr("井 %1 井口 (%2, %3) 在测网覆盖范围外（连续解 IL %4 / XL %5），不取井旁道。")
                                     .arg(best->wellName)
                                     .arg(best->surfaceX, 0, 'f', 1)
                                     .arg(best->surfaceY, 0, 'f', 1)
                                     .arg(loc.inlineF, 0, 'f', 1)
                                     .arg(loc.xlineF, 0, 'f', 1));
        return;
    }
    if (!loc.ok && !mapper.valid()) {
        PaleoNotify::warning(this, tr("井旁道"),
                             tr("测网坐标拟合不可用：%1").arg(QString::fromStdString(mapper.Describe())));
        return;
    }
    if (!loc.ok) {
        PaleoNotify::warning(this, tr("井旁道"),
                             tr("井口无法定位到测网道：%1").arg(QString::fromStdString(loc.error)));
        return;
    }
    const int il = loc.inlineNo;
    const int xl = loc.xlineNo;

    // 提取该 IL 剖面再取井列（同步——单剖面读取毫秒级）
    SgySliceImage slice;
    std::string err;
    if (!m_volume->ExtractSlice(SgySliceType::Inline, il, slice, err)) {
        PaleoNotify::warning(this, tr("井旁道"), tr("道提取失败：%1").arg(QString::fromStdString(err)));
        return;
    }
    // 列 = xl 在体实际线号表中的位置（#147 同口径：线距可 >1）；剖面宽度须与
    // 线号表一致，否则列轴对不上——如实拒绝，不按单位线距猜。
    const int col = int(m_volume->XlineValues().size()) == slice.width ? loc.column : -1;
    if (col < 0 || col >= slice.width) {
        PaleoNotify::warning(this, tr("井旁道"), tr("XL %1 不在 IL %2 剖面列轴上").arg(xl).arg(il));
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(tr("井旁道 · %1（IL %2 / XL %3）").arg(best->wellName).arg(il).arg(xl));
    dlg.setMinimumSize(300, 520);
    auto *lay = new QVBoxLayout(&dlg);
    auto *trace = new class WellSideTraceWidget(best, col, slice, m_canvas->sampleIntervalMs(),
                                                 m_timeOriginMs, &dlg);
    lay->addWidget(trace, 1);
    auto *btnClose = new QToolButton(&dlg);
    btnClose->setText(tr("关闭"));
    connect(btnClose, &QToolButton::clicked, &dlg, &QDialog::close);
    lay->addWidget(btnClose, 0, Qt::AlignRight);
    dlg.exec();
}

} // namespace seismic
