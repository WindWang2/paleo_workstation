// 层：数据
#pragma once
#include <QObject>
#include <QString>

class QProcess;

// services/pythonenv.h — 内嵌 Python/venv 环境管理：MAMCL 等外部 Python 工具
// 的运行底座。只管「找解释器 / 建 venv / 装依赖 / 解 zip」四类原子操作，
// 全部异步（QProcess），编排状态机在 workflow 层（mamcltool）。
class PythonEnvService : public QObject
{
  Q_OBJECT
  public:
    explicit PythonEnvService(const QString &rootDir, QObject *parent = nullptr);
    ~PythonEnvService() override;

    QString rootDir() const { return m_rootDir; }
    QString venvDir(const QString &name) const;          // <root>/venv/<name>
    // venv 内 python 解释器路径；尚未创建时返回空串。
    QString pythonExecutable(const QString &name) const;
    bool venvReady(const QString &name) const;
    bool isBusy() const { return m_proc != nullptr; }

    // 基底解释器：PALEO_PYTHON 环境变量 > PATH 上的 python3/python。
    static QString findBasePython();

  public slots:
    // step 标签随 stepFinished 回传，供编排层对号入座。
    void createVenv(const QString &name);
    void installRequirements(const QString &name, const QString &requirementsPath);
    // 用基底解释器的 zipfile 模块解包（Qt 无内建 zip 解压）。
    void extractZip(const QString &zipPath, const QString &destDir);

  signals:
    void busyChanged(bool busy);
    void outputLine(const QString &line);
    void stepFinished(const QString &step, bool ok, const QString &message);

  private:
    void startStep(const QString &step, const QString &program, const QStringList &args);

    QString m_rootDir;
    QProcess *m_proc = nullptr;
    QString m_step;
};
