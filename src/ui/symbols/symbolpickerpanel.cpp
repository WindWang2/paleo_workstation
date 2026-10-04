// 层：视图
#include "symbolpickerpanel.h"

#include "../../qgis/geopatterns.h"

#include <qgsfillsymbol.h>
#include <qgslinesymbol.h>
#include <qgssymbol.h>

#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QVariant>
#include <QVBoxLayout>

// ui/symbols/symbolpickerpanel.cpp — 三族浏览网格（IconMode 缩略 + 词面），
// 过滤框实时收窄；选中即发 patternPicked（不直接改图层——分层契约）。

namespace
{
  QIcon swatchIconFor(const QString &id, SymbolPickerPanel::Family family)
  {
    QImage image;
    if (family == SymbolPickerPanel::Family::Lithology)
    {
      std::unique_ptr<QgsFillSymbol> sym(GeoPatterns::lithologyFillSymbol(id));
      image = sym ? sym->bigSymbolPreviewImage(
                        nullptr, Qgis::SymbolPreviewFlag::FlagIncludeCrosshairsForMarkerSymbols)
                  : QImage();
    }
    else if (family == SymbolPickerPanel::Family::Facies)
    {
      std::unique_ptr<QgsFillSymbol> sym(GeoPatterns::faciesFillSymbol(id));
      image = sym ? sym->bigSymbolPreviewImage(
                        nullptr, Qgis::SymbolPreviewFlag::FlagIncludeCrosshairsForMarkerSymbols)
                  : QImage();
    }
    else
    {
      std::unique_ptr<QgsLineSymbol> sym(GeoPatterns::lineSymbolFor(id));
      image = sym ? sym->bigSymbolPreviewImage(
                        nullptr, Qgis::SymbolPreviewFlag::FlagIncludeCrosshairsForMarkerSymbols)
                  : QImage();
    }
    return QIcon(QPixmap::fromImage(image));
  }

  QVariantList definitionsFor(SymbolPickerPanel::Family family)
  {
    switch (family)
    {
      case SymbolPickerPanel::Family::Lithology:
        return GeoPatterns::lithologyDefinitions();
      case SymbolPickerPanel::Family::Facies:
        return GeoPatterns::faciesDefinitions();
      case SymbolPickerPanel::Family::LineStyle:
        return GeoPatterns::lineStyleDefinitions();
    }
    return {};
  }
} // namespace

SymbolPickerPanel::SymbolPickerPanel(Family family, QWidget *parent)
  : QWidget(parent), m_family(family)
{
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(8, 8, 8, 8);
  layout->setSpacing(6);

  auto *filterRow = new QHBoxLayout();
  auto *filterLabel = new QLabel(tr("过滤："), this);
  m_filter = new QLineEdit(this);
  m_filter->setPlaceholderText(tr("词面或语义代码"));
  m_filter->setClearButtonEnabled(true);
  filterRow->addWidget(filterLabel);
  filterRow->addWidget(m_filter, 1);
  layout->addLayout(filterRow);

  m_grid = new QListWidget(this);
  m_grid->setObjectName(QStringLiteral("symbolGrid"));
  m_grid->setViewMode(QListWidget::IconMode);
  m_grid->setIconSize(QSize(44, 44));
  m_grid->setResizeMode(QListWidget::Adjust);
  m_grid->setMovement(QListWidget::Static);
  m_grid->setWordWrap(true);
  layout->addWidget(m_grid, 1);

  connect(m_filter, &QLineEdit::textChanged, this, &SymbolPickerPanel::rebuild);
  connect(m_grid, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
    if (item)
      emit patternPicked(item->data(Qt::UserRole).toString());
  });
  connect(m_grid, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
    if (item)
      emit patternPicked(item->data(Qt::UserRole).toString());
  });

  rebuild();
}

void SymbolPickerPanel::setFilter(const QString &text)
{
  m_filter->setText(text);
}

QString SymbolPickerPanel::filter() const
{
  return m_filter->text();
}

QString SymbolPickerPanel::selectedPatternId() const
{
  QListWidgetItem *current = m_grid->currentItem();
  return current ? current->data(Qt::UserRole).toString() : QString();
}

void SymbolPickerPanel::rebuild()
{
  const QString needle = m_filter->text().trimmed();
  m_grid->clear();
  for (const QVariant &v : definitionsFor(m_family))
  {
    const QVariantMap def = v.toMap();
    const QString id = def.value(QStringLiteral("id")).toString();
    const QString title = def.value(QStringLiteral("title")).toString();
    if (!needle.isEmpty() && !title.contains(needle, Qt::CaseInsensitive) &&
        !id.contains(needle, Qt::CaseInsensitive))
      continue;
    auto *item = new QListWidgetItem(swatchIconFor(id, m_family), title, m_grid);
    item->setData(Qt::UserRole, id);
    item->setToolTip(title);
  }
}
