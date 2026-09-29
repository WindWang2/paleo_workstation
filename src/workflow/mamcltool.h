// 层：功能
#pragma once
#include <QObject>
#include <QString>

class PythonEnvService;

// workflow/mamcltool.h — MAMCL「地震多属性智能分析」外部 Python 程序编排：
// open() 一条链走到底——解包 vendor zip（按版本目录 + 旗标幂等）→ 建 venv
// （已建跳过）→ pip 装依赖（requirements 哈希旗标命中跳过）→ startDetached
// 启动 app.py。状态/结果经信号抛给壳，本类不碰控件。
class MamclTool : public QObject
{
  Q_OBJECT
  public:
    explicit MamclTool(PythonEnvService *env, QObject *parent = nullptr);

    void setPackageZip(const QString &zipPath);
    // PALEO_MAMCL_ZIP 环境变量 > setPackageZip > 编译期 PALEO_MAMCL_PACKAGE。
    QString packageZip() const;
    QString packageDir() const;        // <envRoot>/pkg/<zip 基名>
    QString appScript() const;         // packageDir()/app.py
    QString requirementsFile() const;  // packageDir()/requirements.txt
    // 规范化后的依赖清单（剥杂散 \r；openzgy 不在 PyPI——OpenZGY SDK 单独
    // 分发，剔除后 ZGY/层间模式由 runner 如实报缺，其余模式不受影响）。
    QByteArray normalizedRequirements() const;
    QString venvName() const { return QStringLiteral("mamcl"); }
    // 依赖旗标 = requirements.txt 内容哈希，命中即跳过 pip。
    QString depsMarkerPath() const;
    bool ready() const;
    bool isBusy() const { return m_stage != Idle; }

  public slots:
    void open();

  signals:
    void busyChanged(bool busy);
    void statusMessage(const QString &message);
    void launchFinished(bool ok, const QString &message);

  private:
    enum Stage { Idle, Extract, CreateVenv, InstallDeps, Launch };

    void advance();
    void onStepFinished(const QString &step, bool ok, const QString &message);
    void finish(bool ok, const QString &message);
    bool extractMarkerValid() const;
    void writeExtractMarker() const;

    PythonEnvService *m_env;
    QString m_zipPath;
    Stage m_stage = Idle;
};
