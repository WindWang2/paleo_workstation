// 层：视图
#include "aiassistdock.h"
#include "../paleoicons.h"

#include <QDropEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QPushButton>
#include <QScrollArea>
#include <QTextBrowser>
#include <QTextEdit>
#include <QUrl>
#include <QVBoxLayout>

void AiAssistDock::buildFileImportControls(QHBoxLayout *actions, QVBoxLayout *layout) {
  m_importFiles = new QPushButton(PaleoIcons::commandIcon(QStringLiteral("importFiles")),
                                  tr("导入文件…"), this);
  m_importFiles->setIconSize(PaleoIcons::toolbarSize());
  m_importFiles->setObjectName(QStringLiteral("aiAssistantImportFiles"));
  actions->addWidget(m_importFiles);
  connect(m_importFiles, &QPushButton::clicked, this, &AiAssistDock::chooseFilesForImport);
  m_importStatus = new QLabel(this);
  m_importStatus->setObjectName(QStringLiteral("aiAssistantImportStatus"));
  m_importStatus->setTextFormat(Qt::PlainText);
  m_importStatus->setWordWrap(true);
  m_importStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_importStatus->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  m_importStatus->hide();
  layout->addWidget(m_importStatus);
  setFileImportContext(0, tr("请先打开可写工程"));
  setAcceptDrops(true);
  installEventFilter(this);
  // 编辑区默认会把 file URL 当成文本粘贴；只截获 URL，普通文本拖放仍归编辑器。
  m_input->viewport()->installEventFilter(this);
  m_transcript->viewport()->installEventFilter(this);
  m_welcome->viewport()->setAcceptDrops(true);
  m_welcome->viewport()->installEventFilter(this);
}

void AiAssistDock::setFileImportContext(quint64 projectSession, const QString &unavailableReason) {
  if (m_importProjectSession != projectSession || !unavailableReason.isEmpty())
    showFileImportStatus({});
  m_importProjectSession = projectSession;
  m_importFiles->setEnabled(unavailableReason.isEmpty());
  m_importFiles->setToolTip(unavailableReason.isEmpty()
    ? tr("选择文件或将文件拖到助手中，自动识别并导入当前工程") : unavailableReason);
  updateEmptyState();
}

void AiAssistDock::showFileImportStatus(const QString &message) {
  m_importStatus->setText(message);
  m_importStatus->setVisible(!message.isEmpty());
}

void AiAssistDock::chooseFilesForImport() {
  const quint64 session = m_importProjectSession;
  auto *picker = new QFileDialog(this, tr("选择要导入当前工程的文件"));
  picker->setAttribute(Qt::WA_DeleteOnClose);
  picker->setFileMode(QFileDialog::ExistingFiles);
  connect(picker, &QFileDialog::filesSelected, this, [this, session](const QStringList &files) {
    if (files.isEmpty()) return;
    if (session != m_importProjectSession || !m_importFiles->isEnabled()) {
      showFileImportStatus(tr("工程已变化，请重新选择文件。"));
      return;
    }
    emit importFilesRequested(files, session);
  });
  picker->open();
}

bool AiAssistDock::filterFileDrop(QObject *watched, QEvent *event) {
  if (event->type() != QEvent::DragEnter && event->type() != QEvent::DragMove &&
      event->type() != QEvent::Drop)
    return false;
  if (!m_importFiles || (watched != this && watched != m_input &&
      watched != m_input->viewport() && watched != m_transcript->viewport() &&
      watched != m_welcome->viewport()))
    return false;
  auto *drop = static_cast<QDropEvent *>(event);
  if (!drop->mimeData() || !drop->mimeData()->hasUrls())
    return false;
  drop->ignore();
  if (!m_importFiles->isEnabled() || !(drop->possibleActions() & Qt::CopyAction))
    return true;
  QStringList paths;
  for (const auto &url : drop->mimeData()->urls()) {
    if (!url.isLocalFile() || url.toLocalFile().isEmpty())
      return true;
    paths.append(url.toLocalFile());
  }
  if (!paths.isEmpty()) {
    if (event->type() == QEvent::DragEnter)
      m_dragProjectSession = m_importProjectSession;
    if (m_dragProjectSession != m_importProjectSession)
      return true;
    drop->setDropAction(Qt::CopyAction);
    drop->accept();
    if (event->type() == QEvent::Drop)
      emit importFilesRequested(paths, m_importProjectSession);
  }
  return true;
}
