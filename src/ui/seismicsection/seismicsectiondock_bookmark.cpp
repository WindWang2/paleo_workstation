// 层：视图
// 方向 65：D2.10 卷帘 B 图提取 / D2.11 道头信息卡 / D2.12 书签（操作+持久化）/
// D2.13 复制·打印。卷帘与书签各有独立的在途任务与请求号守卫，语义原样保留。
#include "ui/seismicsection/seismicsectiondockwidget.h"

#include "domain/seismic/sectionaxis.h" // opt-02：道头卡列号→线号换算
#include "services/fspathutils.h" // #291 QString↔filesystem::path 走 UTF-16（MSVC 窄构造按 ANSI 解码）
#include "services/seismictaskservice.h"
#include "../notifications/paleonotify.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPainter>
#include <QPrintDialog>
#include <QPrinter>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace seismic {

// ---- D2.12 书签操作 ----
void SeismicSectionDockWidget::addBookmark(const QString &name) {
    SectionBookmark bm;
    bm.name = name;
    bm.modeIndex = m_cboSectionMode->currentIndex();
    bm.sliceValue = m_spinSlice->value();
    bm.view = m_canvas->viewState();
    m_bookmarks.append(bm);
    saveBookmarksToSettings();
    m_cboBookmark->addItem(bm.name);
}

void SeismicSectionDockWidget::removeBookmark(int index) {
    if (index < 0 || index >= m_bookmarks.size())
        return;
    m_bookmarks.removeAt(index);
    saveBookmarksToSettings();
    m_cboBookmark->removeItem(index);
}

void SeismicSectionDockWidget::applyBookmark(int index) {
    if (index < 0 || index >= m_bookmarks.size())
        return;
    const SectionBookmark &bm = m_bookmarks[index];
    const bool modeChanged = m_cboSectionMode->currentIndex() != bm.modeIndex;
    bool sliceChanged = false;
    if (modeChanged)
        m_cboSectionMode->setCurrentIndex(bm.modeIndex); // 触发重提取（先落中线）
    // 换模式后 spin 被 onSectionModeChanged 置成新模式的中线——书签存的线号
    // 必须再落一次，否则跨模式书签恢复的是中线而非书签线（与 locateRequested
    // 同款两段式：先切模式、再比线号）。
    if (bm.modeIndex <= 2 && m_spinSlice->value() != bm.sliceValue) {
        m_spinSlice->setValue(bm.sliceValue); // 触发重提取
        sliceChanged = true;
    }
    if (modeChanged || sliceChanged) {
        // 换线/换模式触发异步重提取：到达回调里 setSectionData/setTimeSliceData
        // 必走 fitToWindow——此刻 setViewState 会被复位（跨线书签的缩放/平移
        // 从不恢复）。目标视口暂存，提取完成后重放（见切片到达回调）。
        m_pendingViewReplay = bm.view;
        m_hasPendingViewReplay = true;
    } else {
        // 同线同模式（无重提取）：直接应用。
        m_canvas->setViewState(bm.view);
    }
}

// ---- D2.12 书签持久化 ----
QString SeismicSectionDockWidget::volumeSettingsKey() const {
    if (!m_volume || !m_volume->IsLoaded())
        return QString();
    const auto &path = m_volume->Path();
    QString key = paleo::fromFsPath(path);
    // Windows 的 path.string() 用反斜杠——QSettings 注册表键不允许 '\'，
    // 两种分隔符都归一为 '_'（平台稳定的体身份键）。
    key.replace(QLatin1Char('/'), QLatin1Char('_'));
    key.replace(QLatin1Char('\\'), QLatin1Char('_'));
    return key;
}

void SeismicSectionDockWidget::saveBookmarksToSettings() const {
    const QString key = volumeSettingsKey();
    if (key.isEmpty())
        return;
    QJsonArray arr;
    for (const auto &bm : m_bookmarks) {
        QJsonObject o;
        o.insert("name", bm.name);
        o.insert("mode", bm.modeIndex);
        o.insert("slice", bm.sliceValue);
        o.insert("zoomX", bm.view.zoomX);
        o.insert("zoomY", bm.view.zoomY);
        o.insert("panX", bm.view.panX);
        o.insert("panY", bm.view.panY);
        arr.append(o);
    }
    QSettings settings;
    settings.setValue(QStringLiteral("seismic/sectionBookmarks/%1").arg(key),
                      QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

void SeismicSectionDockWidget::loadBookmarksFromSettings() {
    m_bookmarks.clear();
    m_cboBookmark->clear();
    const QString key = volumeSettingsKey();
    if (key.isEmpty())
        return;
    QSettings settings;
    const QString raw = settings.value(QStringLiteral("seismic/sectionBookmarks/%1").arg(key)).toString();
    if (raw.isEmpty())
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
    for (const auto &v : doc.array()) {
        const QJsonObject o = v.toObject();
        SectionBookmark bm;
        bm.name = o.value("name").toString();
        bm.modeIndex = o.value("mode").toInt();
        bm.sliceValue = o.value("slice").toInt();
        bm.view.zoomX = o.value("zoomX").toDouble();
        bm.view.zoomY = o.value("zoomY").toDouble();
        bm.view.panX = o.value("panX").toDouble();
        bm.view.panY = o.value("panY").toDouble();
        if (!bm.name.isEmpty()) {
            m_bookmarks.append(bm);
            m_cboBookmark->addItem(bm.name);
        }
    }
}

// ---- D2.10 卷帘 B 图：相邻线提取（同一切片服务通道） ----
void SeismicSectionDockWidget::updateCompareSlice() {
    if (!m_volume || !m_volume->IsLoaded() || !m_taskService)
        return;
    const int mode = m_cboSectionMode->currentIndex();
    if (mode != 0 && mode != 1)
        return; // 卷帘仅支持 IL/XL 模式
    const int current = m_spinSlice->value();
    int neighbor = -1;
    QString label;
    if (mode == 0) { // Inline：取相邻 IL（优先 +1，没有则 -1）
        const auto &ils = m_volume->InlineValues();
        const auto it = std::find(ils.begin(), ils.end(), current);
        if (it != ils.end()) {
            if (it + 1 != ils.end())
                neighbor = *(it + 1);
            else if (it != ils.begin())
                neighbor = *(it - 1);
        }
        label = QStringLiteral("IL %1").arg(neighbor);
    } else {
        const auto &xls = m_volume->XlineValues();
        const auto it = std::find(xls.begin(), xls.end(), current);
        if (it != xls.end()) {
            if (it + 1 != xls.end())
                neighbor = *(it + 1);
            else if (it != xls.begin())
                neighbor = *(it - 1);
        }
        label = QStringLiteral("XL %1").arg(neighbor);
    }
    if (neighbor < 0) {
        m_canvas->setCompareData(SgySliceImage{}, tr("无相邻线"));
        return;
    }

    // 顶替旧在途相邻线请求（快速换线时不再排队）；与主切片共用切片 LRU——
    // 相邻线一旦看过，滑到该线的主图即缓存命中。
    if (m_compareTask) {
        m_compareTask->requestCancel();
        m_compareTask.clear();
    }
    auto vol = std::make_shared<SgyVolume>(*m_volume);
    const auto type = mode == 0 ? SgySliceType::Inline : SgySliceType::Xline;
    const auto generation = m_generation;
    const auto request = ++m_compareRequest;
    QPointer<SeismicSectionDockWidget> guard(this);

    PaleoTask *rawTask = m_taskService->startSliceExtraction(
        vol, type, neighbor,
        [guard, generation, request, label](bool ok,
                                            std::shared_ptr<const SgySliceImage> image,
                                            const QString &error) {
            // 世代号（切体）+ 请求号（被新相邻线顶替）守卫：取消不弹错。
            if (!guard || guard->m_generation != generation || guard->m_compareRequest != request)
                return;
            guard->m_compareTask.clear();
            if (!ok) {
                // 如实降级：失败给原因文案，不静默留空白帘（诚实失败契约）。
                guard->m_canvas->setCompareData(
                    SgySliceImage{}, guard->tr("相邻线提取失败: %1").arg(error));
                return;
            }
            if (!image || image->values.empty()) {
                guard->m_canvas->setCompareData(
                    SgySliceImage{}, guard->tr("%1\n该线无有效地震道").arg(label));
                return;
            }
            guard->m_canvas->setCompareData(*image, label);
        });
    m_compareTask = rawTask;
}

// ---- D2.11 道头信息卡 ----
void SeismicSectionDockWidget::showTraceHeaderCard(int traceIndex) {
    if (!m_volume || !m_volume->IsLoaded())
        return;
    const QString sgyPath = paleo::fromFsPath(m_volume->Path());
    // opt-02：入参 traceIndex 是「剖面列号」，不是文件道序——旧实现直传，
    // 除首条 inline 剖面外全部读错道（IL 非最首 / XL 剖面 / 任意线 / 时间
    // 片）。经 SectionRef 列→实际线号 + SgyIndex 线号→文件道号两级换算；
    // 时间切片/任意线无单一物理道，如实给原因不展示错道数据。
    int fileTrace = -1;
    QString resolveNote;
    const SectionRef ref = m_canvas ? m_canvas->sectionRef() : SectionRef{};
    int lineNo = 0;
    if (!ref.valid || ref.type == SgySliceType::Time) {
        resolveNote = tr("当前视图无单一物理道（时间切片/任意线）——请切换到 "
                         "纵/横测线剖面后点击查看道头");
    } else if (!sectionLineForColumn(ref.colLines, ref.colMin, traceIndex,
                                     &lineNo)) {
        resolveNote = tr("剖面列 %1 无法换算到测线号").arg(traceIndex);
    } else {
        const int inl =
            ref.type == SgySliceType::Inline ? ref.index : lineNo;
        const int xl =
            ref.type == SgySliceType::Inline ? lineNo : ref.index;
        if (m_volume->Index())
            fileTrace = m_volume->Index()->FindTraceIndex(inl, xl);
        if (fileTrace < 0)
            resolveNote =
                tr("测线 (IL %1, XL %2) 不在体索引内").arg(inl).arg(xl);
    }
    SeismicTraceHeaderInfo info;
    if (fileTrace >= 0)
        // finding-94：把已在途的体索引交给 readTraceHeader——它不再自行
        // 全量加载 .sgyidx（那会在本主线程调用上秒级冻结 UI）。
        info = SeismicTaskService::readTraceHeader(sgyPath, fileTrace,
                                                  m_volume->Index());
    else {
        info.ok = false;
        info.error = resolveNote;
    }

    if (!m_traceCard) {
        m_traceCard = new QDialog(this);
        m_traceCard->setWindowTitle(tr("道头信息"));
        m_traceCard->setModal(false);
        m_traceCard->setMinimumSize(280, 220);  // #849：原 360x300
        auto *lay = new QVBoxLayout(m_traceCard);
        m_traceCardTable = new QTableWidget(m_traceCard);
        m_traceCardTable->setColumnCount(2);
        m_traceCardTable->setHorizontalHeaderLabels({tr("字段"), tr("值")});
        m_traceCardTable->horizontalHeader()->setStretchLastSection(true);
        m_traceCardTable->verticalHeader()->setVisible(false);
        m_traceCardTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        lay->addWidget(m_traceCardTable);
        auto *btnClose = new QToolButton(m_traceCard);
        btnClose->setText(tr("关闭"));
        connect(btnClose, &QToolButton::clicked, m_traceCard, &QDialog::close);
        lay->addWidget(btnClose, 0, Qt::AlignRight);
    }
    const auto addRow = [this](const QString &k, const QString &v) {
        const int row = m_traceCardTable->rowCount();
        m_traceCardTable->insertRow(row);
        m_traceCardTable->setItem(row, 0, new QTableWidgetItem(k));
        m_traceCardTable->setItem(row, 1, new QTableWidgetItem(v));
    };
    m_traceCardTable->setRowCount(0);
    if (info.ok) {
        addRow(tr("道序号（0 基）"), QString::number(info.traceIndex));
        addRow(tr("文件偏移 (B)"), QString::number(info.fileOffset));
        addRow(tr("INLINE (189-192)"), QString::number(info.inlineNo));
        addRow(tr("CROSSLINE (193-196)"), QString::number(info.xlineNo));
        addRow(tr("field record (9-12)"), QString::number(info.fieldRecord));
        addRow(tr("CDP ensemble (21-24)"), QString::number(info.cdpEnsemble));
        addRow(tr("CDP X (73-76)"), QString::number(info.cdpX, 'f', 2));
        addRow(tr("CDP Y (77-80)"), QString::number(info.cdpY, 'f', 2));
        addRow(tr("采样数 (115-116)"), QString::number(info.sampleCount));
        addRow(tr("采样间隔 (117-118, μs)"), QString::number(info.sampleIntervalUs));
    } else {
        addRow(tr("错误"), info.error);
    }
    m_traceCard->show();
    m_traceCard->raise();
    m_traceCard->activateWindow();
}

void SeismicSectionDockWidget::onTraceClicked(int traceIndex, double twtMs, double depthM,
                                              float amplitude, double mapX, double mapY) {
    // D2.11：点击道 → 道头信息卡；状态栏同步读数
    showTraceHeaderCard(traceIndex);
    // finding-154：mapX/mapY 是画布上报的地理坐标（时间切片模式下就是
    // XL/IL）——旧实现填 0.0 让状态栏显示伪造的「XL:0/IL:0」，原样透传。
    onTraceHovered(traceIndex, twtMs, depthM, amplitude, mapX, mapY);
}

// ---- D2.13 复制 / 打印 ----
void SeismicSectionDockWidget::onCopyImage() {
    QApplication::clipboard()->setImage(m_canvas->grabCanvasImage(2.0));
    // finding-158：瞬态操作反馈不得写进测线标题——标题承载「当前剖面」
    // 语义（下一次提取前一直挂着「已复制」，读图时误导）。改走通知通道
    // （ErrorHub info，与导出图件同口径）；两条文案均复用已在 .ts 的字面量
    // （标题沿用旧反馈原文，正文取按钮 tooltip），不新增可译条目。
    PaleoNotify::information(this, tr("剖面图已复制到剪贴板"),
                             tr("复制剖面图到剪贴板（含坐标轴与色标）"));
}

void SeismicSectionDockWidget::onPrintImage() {
    QPrinter printer(QPrinter::HighResolution);
    QPrintDialog dlg(&printer, this);
    dlg.setWindowTitle(tr("打印地震剖面"));
    if (dlg.exec() != QDialog::Accepted)
        return;
    QPainter painter(&printer);
    const QImage img = m_canvas->grabCanvasImage(2.0);
    const QRectF pageRect = printer.pageRect(QPrinter::DevicePixel);
    const double scale = std::min(pageRect.width() / img.width(), pageRect.height() / img.height());
    const QSizeF target(img.width() * scale, img.height() * scale);
    const QPointF offset((pageRect.width() - target.width()) / 2.0, (pageRect.height() - target.height()) / 2.0);
    painter.drawImage(QRectF(offset, target), img);
    painter.end();
}

} // namespace seismic
