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
    // #142 依赖锁：<zip 同目录>/<zip 基名>.requirements.lock（uv/pip-compile
    // --generate-hashes 产物）。存在则 pip 走 --require-hashes --only-binary；
    // 不存在则拒绝安装（PALEO_MAMCL_ALLOW_UNPINNED=1 显式放行旧行为）。
    QString lockFile() const;
    // #142 程序包完整性：内置包对编译期 PALEO_MAMCL_SHA256；外部包（环境变量/
    // setPackageZip）须由 PALEO_MAMCL_ZIP_SHA256 给出期望值，否则拒绝。
    QString expectedZipSha256() const;
    static QString sha256OfFile(const QString &path);
    // 依赖旗标 = 锁文件（无锁时为规范化 requirements）内容哈希，命中即跳过 pip。
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
    // 解包旗标 = 程序包 SHA-256（#142）。原子写 + 返回值校验：空/半截旗标
    // 会让下次重复解包（#731）。
    bool writeExtractMarker(QString *error) const;
    // #735 TOCTOU：sha 核对与解包重新打开文件之间 zip 可被替换——先拷一份
    // 自己的副本并再核一遍哈希，解包读的才是「已校验字节」。
    bool stageVerifiedZipCopy(QString *copyPath, QString *error);
    void discardVerifiedZipCopy();

    PythonEnvService *m_env;
    QString m_zipPath;
    QString m_zipSha256; // open() 校验通过的实际哈希，解包旗标内容
    QString m_verifiedZipCopy; // 本次解包用的已校验副本（用完即删）
    QString m_extractTmpDir;   // 解包临时目录（整棵 rename 到位后才写旗标）
    Stage m_stage = Idle;
};
