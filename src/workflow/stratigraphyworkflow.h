// 层：功能
#pragma once
#include <QObject>
#include <QList>
#include <QStringList>
#include <QVector>
#include <memory>
class PaleoTaskService;
struct StratigraphyWell { qlonglong id; QString name; };
struct StratigraphyRow { qlonglong wellId; QString well, id, name, level, status; double top, bottom; };
struct StratigraphyTrack { qlonglong wellId; QString name, mnemonic; QVector<float> depths, values; };
struct StratigraphyResource { qlonglong id; QString name; };
struct StratigraphyConventions { QString depthType, depthUnit, timeUnit, evidence; bool provisional=false; };
class StratigraphyWorkflow : public QObject {
 Q_OBJECT
public:
 explicit StratigraphyWorkflow(PaleoTaskService *tasks, QObject *parent=nullptr);
 ~StratigraphyWorkflow() override;
 void setTaskService(PaleoTaskService *tasks);
 void setProject(const QString &project, bool readOnly);
 void refreshWells();
 bool canEdit() const;
 QList<StratigraphyWell> wells() const;
 QList<StratigraphyRow> rows() const;
 QList<StratigraphyTrack> tracks() const;
 void undo();
 QList<StratigraphyResource> resources(qlonglong well, const QString &kind) const;
 void chooseResource(qlonglong well, const QString &kind, qlonglong id);
 StratigraphyConventions conventions() const;
 void setConventions(const StratigraphyConventions &value);
 void calculate(const QList<qlonglong> &wells, bool subdivide);
 void save();
 void activateClassification(qlonglong wellId);
 void moveBoundary(qlonglong well, const QString &boundary, double md);
signals:
 void changed();
 void busyChanged(bool busy);
 void statusChanged(const QString &message);
 void saved();
 void classificationChanged(const QStringList &horizons);
private:
 struct State;
 std::unique_ptr<State> d;
};
