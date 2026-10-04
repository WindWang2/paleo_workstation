// 层：视图
#pragma once
#include <QAbstractListModel>
#include "../../catalog/datacatalog.h"

// All association combos of a given entity type share one value-backed model.
// The popup paints visible entries; neither rows nor combo population allocate
// one item/widget per entity per rendered asset.
class AssetEntityChoiceModel final : public QAbstractListModel {
public:
  AssetEntityChoiceModel(QVector<CatalogEntity> entities, QString placeholder, QObject *parent)
    : QAbstractListModel(parent), m_entities(std::move(entities)), m_placeholder(std::move(placeholder)) {}
  int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : m_entities.size() + 1; }
  QVariant data(const QModelIndex &index, int role) const override {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
    if (index.row() == 0) {
      if (role == Qt::DisplayRole) return m_placeholder;
      if (role == Qt::UserRole) return QString();
      return {};
    }
    const auto &e = m_entities.at(index.row() - 1);
    if (role == Qt::DisplayRole) return e.name.isEmpty() ? e.id : e.name;
    if (role == Qt::UserRole) return e.id;
    return {};
  }
private:
  QVector<CatalogEntity> m_entities;
  QString m_placeholder;
};
