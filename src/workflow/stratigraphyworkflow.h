// 层：功能
#pragma once
#include <QObject>
#include <QList>
#include <QStringList>
#include <QVector>
#include <memory>
#include <functional>
#include <QJsonObject>
namespace paleo::data { struct InterpretationDocument; struct CorrelationAlgorithmProfile; struct InterpretationClustering; struct InterpretationSeries; }
class PaleoTaskService;
namespace WellComposite { struct ComprehensiveWellData; }
struct StratigraphyWell { qlonglong id; QString name; };
struct StratigraphyRow { qlonglong wellId; QString well, id, name, level, status; double top, bottom;
 bool cycle=false,locked=false;int direction=0;QString source,evidence,segment,parent;double original=0;QString role; };
struct StratigraphyGroup { QString cycle;int number=0,direction=0; };
struct StratigraphyVersion { QString id,name,head; };
struct StratigraphyTemplateChoice { QString key,name;bool sameGroup=false; };
struct StratigraphyAssignment { qlonglong well=0;QString name,segment,key;QList<StratigraphyTemplateChoice> choices; };
struct StratigraphyIssue { qlonglong well;QString field,message;QList<qlonglong> candidates; };
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
 bool previewWellData(qlonglong well, WellComposite::ComprehensiveWellData *data, QString *error) const;
 void undo();
 void redo();
 bool canUndo() const;
 bool canRedo() const;
 bool hasUnsavedChanges() const;
 bool isRunning() const;
 void exportCurve(const paleo::data::InterpretationSeries& series,const QString& path);
 const paleo::data::InterpretationDocument& document() const;
 void loadWells(const QList<qlonglong>& wells);
 void derive(const QString& segment);
 void propagate(const QList<StratigraphyAssignment>& assignments,const QString& segment);
 QList<StratigraphyVersion> templates() const;
 QList<StratigraphyAssignment> prepareAssignments(const QString& segment,const QString& selected) const;
 void setProfile(const paleo::data::CorrelationAlgorithmProfile& profile);
 QList<StratigraphyVersion> versions(const QString& interpretation={}) const;
 bool loadVersion(const QString& interpretation,const QString& revision);
 void reorderWells(const QList<qlonglong>& order);
 void cancel();
 bool selectionMatches(const QList<qlonglong>& wells) const;
 bool canSubdivide(const QList<qlonglong>& wells,const QString& segment={}) const;
 QList<StratigraphyIssue> inputIssues(const QList<qlonglong>& wells) const;
 QJsonObject geometrySettings() const;
 void setGeometrySettings(const QJsonObject& value);
 void setViewOptions(const QJsonObject& value);
 paleo::data::InterpretationClustering previewClustering(int count);
 void saveClustering(const paleo::data::InterpretationClustering& value);
 QList<StratigraphyResource> resources(qlonglong well, const QString &kind) const;
 void chooseResource(qlonglong well, const QString &kind, qlonglong id);
 StratigraphyConventions conventions() const;
 void setConventions(const StratigraphyConventions &value);
 void calculate(const QList<qlonglong> &wells, bool subdivide,const QString& segment={});
 void save();
 void activateClassification(qlonglong wellId);
 void moveBoundary(qlonglong well, const QString &boundary, double md);
 void editBoundary(qlonglong well,const QString& boundary,double md,const QString& name,bool locked);
 void addBoundary(qlonglong well,double md,const QString& name);
 void removeBoundary(qlonglong well,const QString& boundary);
 void confirmBoundary(qlonglong well,const QString& boundary);
 void editCycle(qlonglong well,const QString& cycle,const QString& name,int level,int direction);
 void editHierarchy(qlonglong well,const QString& segment,const QList<StratigraphyGroup>& groups);
 void makeTemplate(qlonglong well,const QString& name,double tolerance);
signals:
 void changed();
 void busyChanged(bool busy);
 void statusChanged(const QString &message);
 void saved();
 void classificationChanged(const QStringList &horizons);
 void boundarySelected(qlonglong well,const QString& boundary);
 void documentReplaced();
private:
 bool edit(const std::function<void(paleo::data::InterpretationDocument&)>& change);
 void runCalculation(const QList<qlonglong>& wells,int mode,const QString& segment={},const QList<StratigraphyAssignment>& assignments={});
 struct State;
 std::unique_ptr<State> d;
};
