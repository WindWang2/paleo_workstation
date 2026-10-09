// 层：QGIS 封装
#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
#include <QFuture>
#include <memory>

#include "../metadata/layermanifest.h"
#include "../metadata/paleoprojectfile.h"

#include <optional>

class QgsProject;
struct ProjectLoadState;

// P0 spine service — owns QgsProject open/save. Never write .qgz directly:
// writes are sequenced by PaleoProjectStore (gpkg commit -> .qgz backup -> atomic .qgz write).
// Lazy loading: the project file persists the DECLARED layer set (manifest), not just
// instantiated layers — uninstantiated layers serialize as placeholder references (§37).
class QgisProjectService : public QObject
{
  Q_OBJECT
  public:
    explicit QgisProjectService(QObject *parent = nullptr);
    ~QgisProjectService() override;

    QgsProject *project() const;                    // never null after open/create
    bool openProject(const QString &qgzPath);       // resolves manifest placeholders on demand
    bool openProjectAsync(const QString &path);
    // Called on the owner thread to capture a worker-only preparation closure.
    // The worker receives a cancel predicate and must stop between stages.
    using OpenPreparation =
        std::function<std::function<void(const std::function<bool()> &cancelled)>(
            const QString &projectDir)>;
    void setOpenPreparation(const OpenPreparation &prepare) { m_openPreparation = prepare; }
    // 旧工厂不接收谓词（工人闭包看不到取消）。适配后仍可登记，谓词被忽略。
    void setOpenPreparation(
        const std::function<std::function<void()>(const QString &projectDir)> &prepare);
    void cancelOpen();
    bool isOpening() const { return m_opening; }
    bool isBackgroundOpenRunning() const { return m_openFuture.isRunning(); }
    bool createProject(const QString &qgzPath);
    bool writeProject();                            // atomic temp+rename via store ordering
    QString projectPath() const;
    QStringList lastErrors() const { return m_errors; }

    // 工程级地理配准（project.paleo 的 georeference 节；.qgz 内嵌副本兜底）。
    // 打开/新建/关闭时重置；无配准工程为 nullopt。设置侧走 writeProjectFile
    //（工程清单是权威），本服务在 writeProject 时把它镜像进 QgsProject 自定义
    // 属性（paleo/georeference）随 .qgz 持久化。
    const std::optional<PaleoGeoreference> &georeference() const { return m_georeference; }
    void setGeoreference(const PaleoGeoreference &g);
    void clearGeoreference();
    const PaleoProjectFile &mapConfiguration() const { return m_mapConfiguration; }
    bool updateMapConfiguration(const PaleoProjectFile &configuration, QString *error = nullptr);
    void setReadOnly(bool readOnly) { m_readOnly = readOnly; }

    // 关闭当前工程（#152/#153）：发 projectAboutToClose → clear() → 清路径
    // → 发 projectClosed。未打开工程时为空操作。关闭后 writeProject() 拒写
    // （路径为空），不会再把空工程覆盖到任何 .qgz 上。
    void closeProject();

    // 工程会话序号：每次成功 open/create/close 自增。注意：此序号目前**无
    // 框架消费**——在途任务的过期防线在 JobRunner（比对 PaleoTask::session()
    // 与 PaleoTaskService::session()，#235）。新增非 JobRunner 的登记型回调
    // 时如需同款防线，在此自行比对，勿假设已有机制兜底。
    quint64 sessionId() const { return m_sessionId; }

    // 打开闸门（#152）：openProject/createProject 在任何读写（含清单收养、
    // m_project->read/clear）之前调用。返回 false = 拒绝打开，当前工程原样
    // 保留（不发 projectAboutToClose，不动 m_path）；*cancelled = true 表示
    // 用户主动取消（调用方据 lastOpenCancelled() 不弹错误框）。
    // creating = createProject 路径（锁冲突时不提供只读降级）。
    using OpenGate = std::function<bool( const QString &projectDir, bool creating,
                                         QString *error, bool *cancelled )>;
    void setOpenGate( const OpenGate &gate ) { m_openGate = gate; }
    bool lastOpenCancelled() const { return m_lastOpenCancelled; }

    // §37 projection hook: when set, writeProject() embeds the provider's
    // declared set into the .qgz (ManifestProjection custom property) so the
    // saved file describes all declarations, not just instantiated layers.
    // Wire e.g. to QgisLayerService::tryDeclared() / LayerManifest::readAll().
    // Provider must report read failure (false + error) — an empty set would
    // otherwise be indistinguishable from a legitimately empty manifest and
    // silently drop every declaration on write.
    void setDeclarationProvider(const std::function<bool(QVector<LayerDeclaration> *, QString *)> &provider);

  signals:
    // 即将替换/关闭当前工程（只在已有工程时发）：面板清工程作用域状态、
    // 在途任务取消——此刻旧工程的 QgsProject/catalog 仍完整可读。
    void projectAboutToClose();
    void projectClosed();
    void projectOpened(const QString &path);
    void projectWritten(const QString &path);
    void openActiveChanged(bool active);
    void openProgress(int percent, const QString &status);
    void openFinished(bool success);
    void openAborted(); // 取消接管；后台读写结束后才能释放工程锁
    void openWorkerFinished();
    void mapConfigurationChanged();

  private:
    void adoptBackgroundOpen(const std::shared_ptr<ProjectLoadState> &state, quint64 generation, int phase);
    bool runGate( const QString &projectDir, bool creating );
    void notifyAboutToClose();
    void failAfterClose();
    void applyMapConfiguration();

  QgsProject *m_project = nullptr;
  quint64 m_sessionId = 0;
  bool m_lastOpenCancelled = false;
  OpenGate m_openGate;
  QString m_path;
  QStringList m_errors;
  std::optional<PaleoGeoreference> m_georeference;
  PaleoProjectFile m_mapConfiguration;
  bool m_readOnly = false;
  std::function<bool(QVector<LayerDeclaration> *, QString *)> m_declarationProvider;
  bool m_opening = false;
  quint64 m_openGeneration = 0;
  QFuture<void> m_openFuture;
  std::shared_ptr<ProjectLoadState> m_pendingLoad;
  OpenPreparation m_openPreparation;
};
