// 层：视图
// 方向 65：D2.10 卷帘 B 图提取 / D2.11 道头信息卡 / D2.12 书签（操作+持久化）/
// D2.13 复制·打印。卷帘与书签各有独立的在途任务与请求号守卫，语义原样保留。
#include "ui/seismicsection/seismicsectiondockwidget.h"

#include "services/fspathutils.h" // #291 QString↔filesystem::path 走 UTF-16（MSVC 窄构造按 ANSI 解码）
#include "services/seismictaskservice.h"

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
    if (m_cboSectionMode->currentIndex() != bm.modeIndex)
        m_cboSectionMode->setCurrentIndex(bm.modeIndex); // 触发重提取
    else if (bm.modeIndex <= 2 && m_spinSlice->value() != bm.sliceValue)
        m_spinSlice->setValue(bm.sliceValue);
    m_canvas->setViewState(bm.view);
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
    const SeismicTraceHeaderInfo info = SeismicTaskService::readTraceHeader(sgyPath, traceIndex);

    if (!m_traceCard) {
        m_traceCard = new QDialog(this);
        m_traceCard->setWindowTitle(tr("道头信息"));
        m_traceCard->setModal(false);
        m_traceCard->setMinimumSize(360, 300);
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
                                              float amplitude, double, double) {
    // D2.11：点击道 → 道头信息卡；状态栏同步读数
    showTraceHeaderCard(traceIndex);
    onTraceHovered(traceIndex, twtMs, depthM, amplitude, 0.0, 0.0);
}

// ---- D2.13 复制 / 打印 ----
void SeismicSectionDockWidget::onCopyImage() {
    QApplication::clipboard()->setImage(m_canvas->grabCanvasImage(2.0));
    setLineTitle(tr("剖面图已复制到剪贴板"));
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
