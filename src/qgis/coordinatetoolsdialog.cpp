// 层：QGIS 封装
#include "coordinatetoolsdialog.h"
#include "coordinateeditor.h"
#include "crsselector.h"

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>
#include <qgscoordinateoperationwidget.h>
#include <qgscoordinatetransform.h>
#include <qgsexception.h>
#include <qgsmapcanvas.h>

PaleoCoordinateToolsDialog::PaleoCoordinateToolsDialog(QgsMapCanvas *canvas, QWidget *parent)
  : QObject(parent), m_dialog(new QDialog(parent)), m_canvas(canvas)
{
  setObjectName(QStringLiteral("coordinateToolsDialog"));
  m_dialog->setObjectName(objectName());
  m_dialog->setWindowTitle(tr("QGIS 坐标工具"));
  m_dialog->resize(640, 560);
  auto *root = new QVBoxLayout(m_dialog);
  auto *description = new QLabel(tr("转换当前输入点，支持经纬度格式、地图拾取和基准操作；不会修改原始资料或工程基准。"), m_dialog);
  description->setWordWrap(true);
  root->addWidget(description);
  m_editor = new PaleoCoordinateEditor(m_dialog);
  root->addWidget(m_editor->widget());
  m_editor->setMapCanvas(m_canvas);
  auto *form = new QFormLayout;
  m_target = new PaleoCrsSelector(false, m_dialog);
  m_target->setObjectName(QStringLiteral("coordinateTargetCrs"));
  form->addRow(tr("目标坐标系"), m_target->widget());
  root->addLayout(form);
  const auto initialCrs = m_canvas ? m_canvas->mapSettings().destinationCrs()
                                   : QgsCoordinateReferenceSystem(QStringLiteral("EPSG:4326"));
  m_editor->setSourceCrs(initialCrs);
  m_target->setDefinition(paleo::coordinates::crsDefinition(initialCrs));
  if (m_canvas) setSourcePoint(m_canvas->center());
  m_operationToggle = new QToolButton(m_dialog);
  m_operationToggle->setObjectName(QStringLiteral("coordinateOperationToggle"));
  m_operationToggle->setText(tr("基准转换操作（可选）"));
  m_operationToggle->setCheckable(true);
  m_operationToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  m_operationToggle->setArrowType(Qt::RightArrow);
  root->addWidget(m_operationToggle);
  m_operation = new QgsCoordinateOperationWidget(m_dialog);
  m_operation->setObjectName(QStringLiteral("coordinateOperation"));
  m_operation->setShowMakeDefault(false);
  m_operation->setShowFallbackOption(true);
  m_operation->setMinimumHeight(120);
  m_operation->setMaximumHeight(210);
  m_operation->hide();
  root->addWidget(m_operation);
  connect(m_operationToggle, &QToolButton::toggled, this, [this](bool expanded) {
    m_operation->setVisible(expanded);
    m_operationToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
  });
  auto *actions = new QHBoxLayout;
  m_convert = new QPushButton(tr("转换坐标"), m_dialog);
  m_convert->setObjectName(QStringLiteral("coordinateConvertButton"));
  m_swap = new QPushButton(tr("交换来源与目标"), m_dialog);
  m_swap->setObjectName(QStringLiteral("coordinateSwapButton"));
  actions->addWidget(m_convert);
  actions->addWidget(m_swap);
  actions->addStretch(1);
  root->addLayout(actions);
  m_resultCaption = new QLabel(tr("转换结果"), m_dialog);
  m_resultCaption->setWordWrap(true);
  root->addWidget(m_resultCaption);
  m_result = new QLineEdit(m_dialog);
  m_result->setObjectName(QStringLiteral("coordinateResult"));
  m_result->setReadOnly(true);
  m_result->setPlaceholderText(tr("选择来源和目标坐标系后执行转换"));
  root->addWidget(m_result);
  m_error = new QLabel(m_dialog);
  m_error->setObjectName(QStringLiteral("coordinateToolsError"));
  m_error->setWordWrap(true);
  m_error->hide();
  root->addWidget(m_error);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, m_dialog);
  m_copy = buttons->addButton(tr("复制结果"), QDialogButtonBox::ActionRole);
  m_copy->setObjectName(QStringLiteral("coordinateCopyResultButton"));
  m_copy->setEnabled(false);
  m_locate = buttons->addButton(tr("定位到结果"), QDialogButtonBox::ActionRole);
  m_locate->setObjectName(QStringLiteral("coordinateLocateButton"));
  m_locate->setEnabled(false);
  m_locate->setToolTip(m_canvas ? tr("先完成有效转换，再把地图中心移到结果点；保持当前缩放。")
                                : tr("未绑定地图画布，只能转换和复制坐标。"));
  root->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::rejected, m_dialog, &QDialog::reject);
  connect(m_convert, &QPushButton::clicked, this, &PaleoCoordinateToolsDialog::convert);
  connect(m_swap, &QPushButton::clicked, this, &PaleoCoordinateToolsDialog::swapCrs);
  connect(m_copy, &QPushButton::clicked, this, &PaleoCoordinateToolsDialog::copyResult);
  connect(m_locate, &QPushButton::clicked, this, &PaleoCoordinateToolsDialog::locateResult);
  connect(m_target, &PaleoCrsSelector::definitionChanged, this, [this](const QString &) { updateOperation(); });
  connect(m_editor->sourceSelector(), &PaleoCrsSelector::definitionChanged,
          this, [this](const QString &) { updateOperation(); });
  connect(m_editor, &PaleoCoordinateEditor::pointChanged, this, [this] {
    m_hasResult = false;
    m_result->clear();
    m_copy->setEnabled(false);
    m_locate->setEnabled(false);
  });
  updateOperation();
}

PaleoCoordinateToolsDialog::~PaleoCoordinateToolsDialog()
{
  delete m_dialog.data();
}

QDialog *PaleoCoordinateToolsDialog::dialog() const
{
  return m_dialog.data();
}

void PaleoCoordinateToolsDialog::setSourcePoint(const QgsPointXY &point)
{
  m_editor->setPoint(point);
}

bool PaleoCoordinateToolsDialog::convertedPoint(QgsPointXY *point) const
{
  if (!m_hasResult || !point) return false;
  *point = m_resultPoint;
  return true;
}

QString PaleoCoordinateToolsDialog::resultText() const
{
  return m_result->text();
}

bool PaleoCoordinateToolsDialog::locateResult()
{
  if (!m_hasResult || !m_canvas) return false;
  auto mapped = paleo::coordinates::point();
  QString error;
  if (!paleo::coordinates::transformPoint(
          m_resultPoint, m_target->crs(), m_canvas->mapSettings().destinationCrs(),
          m_canvas->mapSettings().transformContext(), &mapped, &error))
  {
    m_error->setText(error);
    m_error->show();
    return false;
  }
  m_canvas->setCenter(mapped);
  m_canvas->refresh();
  emit mapCenterChanged();
  return true;
}

void PaleoCoordinateToolsDialog::convert()
{
  m_hasResult = false;
  m_copy->setEnabled(false);
  m_locate->setEnabled(false);
  m_error->hide();
  m_result->clear();
  QString error;
  QgsPointXY source;
  if (!m_editor->sourcePoint(&source, &error) || !m_target->validate(&error))
  {
    m_error->setText(error);
    m_error->show();
    return;
  }
  const auto sourceCrs = m_editor->sourceSelector()->crs();
  const auto targetCrs = m_target->crs();
  auto context = m_canvas ? m_canvas->mapSettings().transformContext()
                          : QgsCoordinateTransformContext();
  bool allowFallback = true;
  if (m_operation->isVisible() && sourceCrs != targetCrs && m_operation->hasSelection())
  {
    const auto operation = m_operation->selectedOperation();
    allowFallback = operation.allowFallback;
    if (!operation.isAvailable)
      error = tr("所选基准操作缺少必要的网格文件；请安装网格或选择其他操作，不能静默近似。");
    else if (!operation.proj.isEmpty() &&
             !context.addCoordinateOperation(sourceCrs, targetCrs, operation.proj, operation.allowFallback))
      error = tr("无法注册所选基准操作。");
  }
  if (!error.isEmpty())
  {
    m_error->setText(error);
    m_error->show();
    return;
  }
  try
  {
    QgsCoordinateTransform transform(sourceCrs, targetCrs, context);
    transform.setAllowFallbackTransforms(allowFallback);
    if (!transform.isValid()) throw QgsCsException(tr("无法建立坐标转换"));
    const auto transformed = transform.transform(source);
    if (!std::isfinite(transformed.x()) || !std::isfinite(transformed.y()))
      throw QgsCsException(tr("坐标转换返回了无效位置"));
    m_resultPoint = transformed;
    m_hasResult = true;
    const int precision = targetCrs.isGeographic() ? 9 : 3;
    m_result->setText(paleo::coordinates::formatPoint(transformed, targetCrs,
                                                      m_editor->format(), precision));
    m_resultCaption->setText(tr("%1 → %2%3")
        .arg(paleo::coordinates::crsDefinition(sourceCrs), paleo::coordinates::crsDefinition(targetCrs),
             transform.fallbackOperationOccurred() ? tr("；QGIS 已使用近似回退转换") : QString()));
    m_copy->setEnabled(true);
    m_locate->setEnabled(m_canvas != nullptr);
  }
  catch (const QgsCsException &exception)
  {
    m_error->setText(tr("坐标转换失败：%1").arg(exception.what()));
    m_error->show();
  }
}

void PaleoCoordinateToolsDialog::updateOperation()
{
  const auto source = m_editor->sourceSelector()->crs();
  const auto target = m_target->crs();
  m_editor->setTargetCrs(target, m_canvas ? m_canvas->mapSettings().transformContext()
                                          : QgsCoordinateTransformContext());
  m_operation->setSourceCrs(source);
  m_operation->setDestinationCrs(target);
  if (m_canvas)
    m_operation->setSelectedOperationUsingContext(m_canvas->mapSettings().transformContext());
  m_operationToggle->setEnabled(source.isValid() && target.isValid() && source != target);
  m_operationToggle->setToolTip(source == target ? tr("来源和目标相同，不需要坐标转换。")
                                                 : tr("查看 QGIS 提供的基准操作、精度、适用范围和网格依赖。"));
  m_hasResult = false;
  m_result->clear();
  m_copy->setEnabled(false);
  m_locate->setEnabled(false);
}

void PaleoCoordinateToolsDialog::swapCrs()
{
  const auto source = m_editor->sourceSelector()->definition();
  const bool useResult = m_hasResult;
  const auto result = m_resultPoint;
  m_editor->setSourceCrs(m_target->crs());
  m_target->setDefinition(source);
  if (useResult) m_editor->setPoint(result);
  updateOperation();
}

void PaleoCoordinateToolsDialog::copyResult()
{
  if (m_hasResult) QApplication::clipboard()->setText(m_result->text());
}
