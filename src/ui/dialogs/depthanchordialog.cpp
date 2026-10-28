// 层：视图
#include "depthanchordialog.h"

#include "../../services/imagelod.h"
#include "../../workflow/wellattachmentops.h"
#include "../paleotheme.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QUuid>
#include <QVBoxLayout>

namespace
{

// 预览位图：单张全载（LOD 单张检视口径）+ 透明垫棋盘底。透明图垫棋盘底
// （纸面图件中性灰，不随暗色翻转）。
QImage previewImage(const QImage &img)
{
  if (img.isNull() || !img.hasAlphaChannel())
    return img;
  QImage flat(img.size(), QImage::Format_ARGB32_Premultiplied);
  QPainter p(&flat);
  p.fillRect(flat.rect(), paleo::imagelod::alphaCheckerboard());
  p.drawImage(0, 0, img);
  p.end();
  return flat;
}

// 有界同步解码（解码失败兜底路径）：QImageReader::setScaledSize 只缩到预览
// 边长，装载内存峰值 = 缩略字节而非全图字节。
constexpr int kPreviewEdge = 280;

QString sourceText(const QString &source)
{
  if (source == QLatin1String("filename"))
    return QObject::tr("文件名");
  if (source == QLatin1String("manual"))
    return QObject::tr("手工");
  return QString();
}

} // namespace

namespace PaleoDepthAnchorDialog
{

bool prompt(QWidget *parent, const Context &ctx, Result *out)
{
  if (out)
    *out = Result{};
  QDialog dlg(parent);
  dlg.setObjectName(QStringLiteral("depthAnchorDialog"));
  const QString subject = ctx.wellName.isEmpty()
                               ? ctx.fileName
                               : QStringLiteral("%1 · %2").arg(ctx.wellName, ctx.fileName);
  dlg.setWindowTitle(ctx.hasAnchor
                         ? QObject::tr("编辑锚深 — %1").arg(subject)
                         : QObject::tr("补锚深度 — %1").arg(subject));
  dlg.setModal(true);

  auto *lay = new QVBoxLayout(&dlg);
  lay->setContentsMargins(PaleoTheme::tokens().spacingMd,
                          PaleoTheme::tokens().spacingMd,
                          PaleoTheme::tokens().spacingMd,
                          PaleoTheme::tokens().spacingMd);
  lay->setSpacing(PaleoTheme::tokens().spacingSm);

  auto *body = new QGridLayout;
  body->setHorizontalSpacing(PaleoTheme::tokens().spacingMd);
  body->setVerticalSpacing(PaleoTheme::tokens().spacingSm);
  lay->addLayout(body);

  // 预览（单张全载；等比缩到 ≤ 280px，放大检视口径）。岩心照片可达数十
  // MB——全载放工作线程（imagelod LOD 口径：GUI 线程只碰 QPixmap），界面
  // 先显示装载态；排队失败才退回有界同步解码，不把卡死当默认路径。
  auto *preview = new QLabel(&dlg);
  preview->setObjectName(QStringLiteral("depthAnchorPreview"));
  preview->setMinimumSize(160, 120);
  preview->setAlignment(Qt::AlignCenter);
  const auto showPreview = [preview](const QImage &img) {
    const QImage flat = previewImage(img);
    if (flat.isNull())
    {
      preview->setText(QObject::tr("图片无法读取"));
      return;
    }
    preview->setPixmap(QPixmap::fromImage(
        flat.scaled(kPreviewEdge, kPreviewEdge, Qt::KeepAspectRatio,
                    Qt::SmoothTransformation)));
  };
  if (!ctx.imagePath.isEmpty())
  {
    preview->setText(QObject::tr("正在读取图片…"));
    const QPointer<QDialog> guard(&dlg);
    const QString key =
        QStringLiteral("depth-anchor|%1").arg(QUuid::createUuid().toString());
    const bool queued = paleo::imagelod::FullImageCache::shared().requestDecode(
        key, ctx.imagePath, [guard, showPreview](const QImage &image) {
          if (!guard)
            return;
          showPreview(image);
        });
    if (!queued)
      showPreview(paleo::imagelod::loadThumbnail(ctx.imagePath, kPreviewEdge)
                      .thumbnail);
  }
  else
    preview->setText(QObject::tr("无预览"));
  body->addWidget(preview, 0, 0, 3, 1);

  auto *fileLbl = new QLabel(ctx.fileName, &dlg);
  fileLbl->setWordWrap(true);
  auto *fileCap = new QLabel(QObject::tr("文件"), &dlg);
  fileCap->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  PaleoTheme::applyThemedStyleSheet(fileCap, [] {
    return PaleoTheme::mutedCaptionStyleSheet();
  });
  body->addWidget(fileCap, 0, 1);
  body->addWidget(fileLbl, 0, 2);

  const QString current =
      ctx.hasAnchor
          ? QObject::tr("%1 m（来源：%2）")
                .arg(ctx.currentDepth, 0, 'f', 2)
                .arg(sourceText(ctx.anchorSource))
          : QObject::tr("未锚定");
  auto *curLbl = new QLabel(current, &dlg);
  curLbl->setFont(PaleoTheme::monoFont());
  auto *curCap = new QLabel(QObject::tr("当前锚深"), &dlg);
  curCap->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  PaleoTheme::applyThemedStyleSheet(curCap, [] {
    return PaleoTheme::mutedCaptionStyleSheet();
  });
  body->addWidget(curCap, 1, 1);
  body->addWidget(curLbl, 1, 2);

  auto *input = new QLineEdit(&dlg);
  input->setObjectName(QStringLiteral("depthAnchorInput"));
  input->setFont(PaleoTheme::monoFont());
  input->setPlaceholderText(ctx.hasAnchor
                                ? QObject::tr("新深度，如 1849.35")
                                : QObject::tr("深度，如 1849.35"));
  input->setText(ctx.hasAnchor
                     ? QString::number(ctx.currentDepth, 'f', 2)
                     : QString());
  input->setToolTip(QObject::tr("深度单位固定为米（m），可带 m/米 后缀；"
                                "留空并点「清除锚定」可回到未锚定态"));
  auto *inputCap = new QLabel(QObject::tr("深度 (m)"), &dlg);
  inputCap->setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt));
  PaleoTheme::applyThemedStyleSheet(inputCap, [] {
    return PaleoTheme::mutedCaptionStyleSheet();
  });
  body->addWidget(inputCap, 2, 1);
  body->addWidget(input, 2, 2);

  auto *errLbl = new QLabel(&dlg);
  errLbl->setObjectName(QStringLiteral("depthAnchorError"));
  errLbl->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(errLbl, [] {
    return QStringLiteral("color: %1;").arg(PaleoTheme::tokens().errorText.name());
  });
  lay->addWidget(errLbl);

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                           QDialogButtonBox::Cancel,
                                       &dlg);
  auto *clearBtn = new QPushButton(QObject::tr("清除锚定"), &dlg);
  clearBtn->setObjectName(QStringLiteral("depthAnchorClearBtn"));
  clearBtn->setToolTip(
      QObject::tr("移除锚深（版本元数据操作，不删文件）——图片回到「未锚定」，"
                  "可随时再补"));
  clearBtn->setEnabled(ctx.hasAnchor);
  buttons->addButton(clearBtn, QDialogButtonBox::DestructiveRole);
  lay->addWidget(buttons);

  const auto showReason = [errLbl](paleo::DepthInputStatus st) {
    errLbl->setText(paleo::WellAttachmentOps::reasonText(st));
  };

  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  QObject::connect(clearBtn, &QPushButton::clicked, &dlg, [&] {
    if (out)
    {
      out->accepted = true;
      out->clear = true;
    }
    dlg.accept();
  });
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
    double depth = 0.0;
    // #763：解析器已修（原先 QLatin1String("米") 按 Latin-1 逐字节解释，UTF-8
    // 源码对不上 U+7C73，「1500米」被误判 BadUnit）——这里不再需要把「米」预换
    // 成 m 的 workaround，直接交给 parseDepthInput。
    const QString text = QString(input->text());
    const paleo::DepthInputStatus st =
        paleo::WellAttachmentOps::parseDepthInput(text, &depth);
    if (st == paleo::DepthInputStatus::Ok)
    {
      if (out)
      {
        out->accepted = true;
        out->clear = false;
        out->depth = depth;
      }
      dlg.accept();
      return;
    }
    if (st == paleo::DepthInputStatus::Clear)
      errLbl->setText(QObject::tr(
          "输入为空：填新深度，或点「清除锚定」回到未锚定态"));
    else
      showReason(st);
  });

  if (dlg.exec() != QDialog::Accepted || !out || !out->accepted)
    return false;
  return true;
}

} // namespace PaleoDepthAnchorDialog
