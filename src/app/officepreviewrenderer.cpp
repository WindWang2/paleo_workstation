// 层：组装根
// Calligra's GUI-thread document engine lives exclusively in this child process.
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QPainter>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <KLocalizedString>
#include <KoDocument.h>
#include <KoDocumentEntry.h>
#include <KoPart.h>
#include <KoPADocument.h>
#include <KoPAPageBase.h>
#include <KoShapePainter.h>
#include <KoShapeManager.h>
#include <KoZoomHandler.h>
#include <KWCanvasItem.h>
#include <KWDocument.h>
#include <KWPage.h>
#include <frames/KWTextFrameSet.h>
#include <KoTextDocumentLayout.h>
#include <KoTextLayoutRootArea.h>
#include <sheets/core/Map.h>
#include <sheets/core/PrintSettings.h>
#include <sheets/core/Sheet.h>
#include <sheets/core/SheetPrint.h>
#include <sheets/part/Doc.h>
#include <sheets/ui/SheetView.h>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {
QString tr(const char *text) { return QCoreApplication::translate("OfficePreviewRenderer", text); }
void send(const QJsonObject &message)
{
  const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
  std::fwrite(line.constData(), 1, line.size(), stdout);
  std::fflush(stdout);
}
struct Page {
  KWPage word;
  KoPAPageBase *slide = nullptr;
  Calligra::Sheets::Sheet *sheet = nullptr;
  int sheetPage = 0;
  QSizeF size;
  QString label;
};
constexpr int maxPages = 4000;
constexpr int maxPixels = 2200;
}

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  app.setQuitOnLastWindowClosed(false);
  app.setApplicationName(QStringLiteral("paleo_office_renderer"));
  KLocalizedString::setApplicationDomain("calligra");
  const QStringList args = app.arguments();
  if (args.size() != 3) return 2;
  const QString source = QFileInfo(args[1]).absoluteFilePath();
  const QString output = QFileInfo(args[2]).absoluteFilePath();
  auto error = [&app](const QString &reason) {
    send({{"type", "error"}, {"message", reason}});
    app.exit(1);
  };
  struct SharedState {
    QVector<Page> pages;
    KoPart *part = nullptr;
    KoDocument *document = nullptr;
    KWCanvasItem *wordCanvas = nullptr;
    std::atomic<bool> ready{false};
    std::atomic<bool> wordLayoutReady{false};
    std::atomic<bool> alive{true};
  };
  auto state = std::make_shared<SharedState>();

  QTimer deadline;
  deadline.setSingleShot(true);
  QObject::connect(&deadline, &QTimer::timeout, &app, [&] { error(tr("文档解析或排版超时")); });
  deadline.start(120000);
  // Parent sends small JSON commands; blocking stdin belongs to a reader thread.
  // EOF is the lifetime boundary. No input or painting is handled by Paleo's GUI.
  std::thread([state, &app, output, error] {
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line.size() > 1024) break;
      const QByteArray bytes(line.data(), int(line.size()));
      QMetaObject::invokeMethod(&app, [state, bytes, output, error] {
        if (!state->alive.load()) return;
        const QJsonObject command = QJsonDocument::fromJson(bytes).object();
        if (command.value("type") != "page" || !state->ready.load()) return;
        const int index = command.value("index").toInt(-1);
        const int request = command.value("request").toInt();
        if (index < 0 || index >= state->pages.size()) { error(tr("页码无效")); return; }
        const Page &page = state->pages[index];
        const QSize size = page.size.toSize().scaled(maxPixels, maxPixels, Qt::KeepAspectRatio);
        QImage image;
        if (state->wordCanvas) {
          // Thumbnail's waitUntilReady() can start another layout for unused
          // text frames. Paint the completed layout without mutating pagination.
          image = QImage(size, QImage::Format_RGB32);
          image.fill(Qt::white);
          KoZoomHandler zoom;
          zoom.setResolution(1, 1);
          zoom.setZoom(qMin(size.width() / page.size.width(), size.height() / page.size.height()));
          QPainter painter(&image);
          painter.setRenderHint(QPainter::Antialiasing);
          painter.translate(0, -zoom.documentToViewY(page.word.offsetInDocument()));
          painter.setClipRect(zoom.documentToView(page.word.rect()));
          state->wordCanvas->shapeManager()->paint(painter, zoom, true);
        } else if (page.slide) {
          image = static_cast<KoPADocument *>(state->document)->pageThumbImage(page.slide, size);
        } else if (page.sheet) {
          image = QImage(size, QImage::Format_RGB32);
          image.fill(Qt::white);
          QPainter painter(&image);
          const double scale = qMin(size.width() / page.size.width(), size.height() / page.size.height());
          KoZoomHandler zoom;
          zoom.setResolution(1, 1); // document dimensions are points, independent of screen DPI
          const auto *settings = page.sheet->printSettings();
          const KoPageLayout layout = settings->pageLayout();
          const double contentScale = scale * settings->zoom();
          zoom.setZoom(contentScale);
          Calligra::Sheets::SheetView view(page.sheet);
          view.setViewConverter(&zoom);
          view.setPaintCellRange(page.sheet->print()->cellRange(page.sheetPage));
          const QRectF area = page.sheet->print()->documentArea(page.sheetPage);
          painter.setClipRect(image.rect());
          painter.translate(layout.leftMargin * scale, layout.topMargin * scale);
          view.paintCells(painter, QRectF(QPointF(0, 0), area.size()), QPointF(0, 0));
          painter.save();
          painter.translate(-area.left() * contentScale, -area.top() * contentScale);
          KoShapePainter shapes;
          shapes.setShapes(page.sheet->shapes());
          shapes.paint(painter, zoom);
          painter.restore();
        }
        const QString file = QStringLiteral("page-%1.png").arg(index);
        if (!image.isNull()) {
          image.setDotsPerMeterX(qRound(image.width() / page.size.width() * 72.0 / 0.0254));
          image.setDotsPerMeterY(qRound(image.height() / page.size.height() * 72.0 / 0.0254));
        }
        if (image.isNull() || !image.save(QDir(output).filePath(file), "PNG")) {
          error(tr("无法渲染文档页面")); return;
        }
        send({{"type", "page"}, {"index", index}, {"request", request}, {"file", file}});
      }, Qt::QueuedConnection);
    }
    QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
  }).detach();
  QTimer layoutPoll;
  layoutPoll.setInterval(40);
  QObject::connect(&layoutPoll, &QTimer::timeout, &app, [state, &app, &layoutPoll, &deadline, error] {
    if (!state->document || state->document->isLoading()) return;
    auto *words = qobject_cast<KWDocument *>(state->document);
    if (words && !state->wordLayoutReady.load()) return;
    if (words) {
      // Flush the canvas spatial index before checking layout readiness: its
      // collision notifications can invalidate text areas during initial setup.
      if (words->pageCount() > 0)
        state->wordCanvas->shapeManager()->shapesAt(words->pageManager()->page(1).rect());
      for (KWFrameSet *frame : words->frameSets()) {
        auto *text = dynamic_cast<KWTextFrameSet *>(frame);
        if (!text) continue;
        auto *layout = qobject_cast<KoTextDocumentLayout *>(text->document()->documentLayout());
        if (!layout) continue;
        if (frame == words->mainFrameSet() && layout->rootAreas().isEmpty()) return;
        for (KoTextLayoutRootArea *area : layout->rootAreas())
          if (area->isDirty()) return;
      }
    }
    layoutPoll.stop();
    if (words) {
      if (words->pageCount() > maxPages) { error(tr("文档超过 4000 页预览上限")); return; }
      for (const KWPage &page : words->pageManager()->pages())
        state->pages.append({page, nullptr, nullptr, 0, page.rect().size(), QString::number(page.pageNumber())});
    } else if (auto *slides = qobject_cast<KoPADocument *>(state->document)) {
      if (slides->pageCount() > maxPages) { error(tr("文档超过 4000 页预览上限")); return; }
      int number = 0;
      for (KoPAPageBase *page : slides->pages(false)) {
        const KoPageLayout layout = page->pageLayout();
        state->pages.append({{}, page, nullptr, 0, QSizeF(layout.width, layout.height), QString::number(++number)});
      }
    } else if (auto *sheets = qobject_cast<Calligra::Sheets::Doc *>(state->document)) {
      for (Calligra::Sheets::SheetBase *base : sheets->map()->sheetList()) {
        auto *sheet = dynamic_cast<Calligra::Sheets::Sheet *>(base);
        if (!sheet || sheet->isHidden()) continue;
        // Use all printed pages, including later rows/columns and all visible sheets.
        sheet->print()->setSettings(*sheet->printSettings(), true);
        const KoPageLayout layout = sheet->printSettings()->pageLayout();
        const int count = qMax(1, sheet->print()->pageCount());
        if (count > maxPages - state->pages.size()) { error(tr("工作簿超过 4000 页预览上限")); return; }
        for (int number = 1; number <= count; ++number)
          state->pages.append({{}, nullptr, sheet, number, QSizeF(layout.width, layout.height), sheet->sheetName() + " · " + QString::number(number)});
      }
    }
    if (state->pages.isEmpty() || state->pages.size() > maxPages) { error(tr("文档没有可预览页面，或超过 4000 页上限")); return; }
    QJsonArray labels;
    for (const Page &page : state->pages) {
      if (page.size.width() <= 0 || page.size.height() <= 0) { error(tr("文档页面尺寸无效")); return; }
      labels.append(page.label);
    }
    deadline.stop();
    state->ready.store(true);
    send({{"type", "ready"}, {"engine", "Calligra 26.08.2"}, {"pages", labels}});
  });
  QTimer::singleShot(0, &app, [state, &app, &layoutPoll, source, error] {
    const QString extension = QFileInfo(source).suffix().toLower();
    const QString mime = extension == "doc" || extension == "docx" ? QStringLiteral("application/vnd.oasis.opendocument.text")
      : extension == "xls" || extension == "xlsx" ? QStringLiteral("application/vnd.oasis.opendocument.spreadsheet")
      : extension == "ppt" || extension == "pptx" ? QStringLiteral("application/vnd.oasis.opendocument.presentation") : QString();
    if (mime.isEmpty()) { error(tr("不支持的 Office 文件格式")); return; }
    QString reason;
    state->part = KoDocumentEntry::queryByMimeType(mime).createKoPart(&reason);
    if (!state->part || !(state->document = state->part->document())) { error(tr("Calligra 文档组件不可用：") + reason); return; }
    state->document->setCheckAutoSaveFile(false);
    state->document->setAutoErrorHandlingEnabled(false);
    state->document->setAutoSave(0);
    state->document->setSaveInBatchMode(true);
    if (auto *words = qobject_cast<KWDocument *>(state->document)) {
      state->wordCanvas = static_cast<KWCanvasItem *>(state->part->canvasItem(words));
      QObject::connect(words, &KWDocument::shapeAdded, state->wordCanvas->shapeManager(), &KoShapeManager::addShape);
      QObject::connect(words, &KWDocument::shapeRemoved, state->wordCanvas->shapeManager(), &KoShapeManager::remove);
      QObject::connect(words, &KWDocument::mainTextFrameSetLayoutFinished, &app, [state] { state->wordLayoutReady.store(true); });
    }
    // WPS and other desktop suites can register private MIME names for these
    // extensions. Feed Calligra the standard Microsoft type, keeping its own
    // content validation and mislabeled DOC/DOCX replacement logic intact.
    const QMap<QString, QByteArray> inputMime{
      {"doc", "application/msword"},
      {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
      {"xls", "application/vnd.ms-excel"},
      {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
      {"ppt", "application/vnd.ms-powerpoint"},
      {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"}};
    state->document->setMimeType(inputMime.value(extension));
    if (!state->document->openUrl(QUrl::fromLocalFile(source))) {
      error(tr("Calligra 无法解析原件（损坏、加密或格式不支持）") + "\n" + state->document->errorMessage().left(1024));
      return;
    }
    state->document->setReadWrite(false);
    layoutPoll.start();
  });
  const int result = app.exec();
  state->alive.store(false);
  // Engine teardown has queued GUI dependencies; the OS owns isolated cleanup.
  std::exit(result);
}
