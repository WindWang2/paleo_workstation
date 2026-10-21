// 层：视图
#include "datapreviewtabs.h"
#include "datapreviewtabs_internal.h"
#include <QPdfDocument>
#include <QPdfView>
using namespace paleo::datapreview_detail;
QWidget *DataPreviewTabs::buildReferenceDocumentContent(const QString &path, QWidget *host, QVBoxLayout *layout) {
    // PDF 原件仍走 QtPdf；Office 后缀已在上方接入 Calligra，绝不转换。
    if (QFileInfo(path).suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0)
    {
      auto *doc = new QPdfDocument(host);
      if (doc->load(path) == QPdfDocument::Error::None)
      {
        auto *view = new QPdfView(host);
        view->setObjectName(QStringLiteral("pdfView"));
        view->setDocument(doc);
        view->setPageMode(QPdfView::PageMode::MultiPage);
        layout->addWidget(view, 1);
      }
      else
        layout->addWidget(stateLabel(tr("PDF 原件无法加载\n%1").arg(path), host, true), 1);
    }
    else
    {
      layout->addWidget(stateLabel(tr("该格式暂无内嵌预览"), host), 1);
      layout->addWidget(makeOpenExternalRow(path, host));
    }
    return host;
}
