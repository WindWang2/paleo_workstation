// 层：测试壳
// G4-W1/W6：operation 私有命名空间纯数据契约。生成器与历史形态
// （op-<uuid>、裸 UUID attempt、attempt-<uuid> fixture）必须原样通过；
// 点段、分隔符、绝对形式、控制字符、大小写/花括号变体一律拒绝且不归一。
#include <QtTest>
#include <QUuid>
#include "metadata/operationnamespace.h"

using namespace paleo::capability::operation_namespace;

class AgentOperationNamespace : public QObject {
  Q_OBJECT
private slots:
  void canonicalFormsMatchEveryGenerator();
  void unsafeOperationIdsAreRejected();
  void attemptFormsKeepBothLegacies();
  void prefixMembershipIsSegmentExact();
private:
  static QString op() { return QStringLiteral("op-") + QUuid::createUuid().toString(QUuid::WithoutBraces); }
  static QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
};

void AgentOperationNamespace::canonicalFormsMatchEveryGenerator() {
  const QString id = op();
  QVERIFY(isCanonicalOperationId(id));
  QCOMPARE(id.size(), 39);
  QVERIFY(isCanonicalAttemptId(uuid()));
  QVERIFY(isCanonicalAttemptId(QStringLiteral("attempt-") + uuid()));
  QVERIFY(isCanonicalResultLeaf(QStringLiteral("result-") + uuid()));
  QCOMPARE(privateOperationPrefix(id), QStringLiteral(".paleo/capability-ops/") + id + '/');
}

void AgentOperationNamespace::unsafeOperationIdsAreRejected() {
  const QString canonical = op();
  // 前缀关系：op-a 不因字符串前缀认领 op-ab。
  QVERIFY(!isCanonicalOperationId(canonical + QLatin1Char('x')));
  QVERIFY(!isCanonicalOperationId(canonical.left(canonical.size() - 1)));
  // 点段/分隔符/绝对形式/别名与控制字符。
  QVERIFY(!isCanonicalOperationId(QStringLiteral("op-a/../b")));
  QVERIFY(!isCanonicalOperationId(QStringLiteral("/tmp/") + canonical));
  QVERIFY(!isCanonicalOperationId(QStringLiteral("op-a/b")));
  QVERIFY(!isCanonicalOperationId(QStringLiteral("op-\\a")));
  QVERIFY(!isCanonicalOperationId(QStringLiteral("op-a.")));
  QVERIFY(!isCanonicalOperationId(QStringLiteral("op-.")));
  QVERIFY(!isCanonicalOperationId(QStringLiteral("op-a") + QChar(0x0A)));
  QVERIFY(!isCanonicalOperationId(QString::fromUtf8("op-中文")));
  // 不 trim、不改大小写、不剥花括号——变体不作为同一身份继续。
  QVERIFY(!isCanonicalOperationId(QStringLiteral(" op-a") + uuid()));
  QVERIFY(!isCanonicalOperationId((QStringLiteral("op-") + uuid()).toUpper()));
  QVERIFY(!isCanonicalOperationId(QStringLiteral("op-{") + uuid() + QLatin1Char('}')));
  QVERIFY(!isCanonicalOperationId(QStringLiteral("urn:uuid:") + uuid()));
  QVERIFY(!isCanonicalOperationId(QString()));
  QVERIFY(!isCanonicalOperationId(QString(300, QLatin1Char('a'))));
  QVERIFY(privateOperationPrefix(QStringLiteral("op-a/../b")).isEmpty());
}

void AgentOperationNamespace::attemptFormsKeepBothLegacies() {
  // 生产裸 UUID 与已发布 attempt-<uuid> fixture 都保留；其余不认。
  QVERIFY(!isCanonicalAttemptId(QStringLiteral("attempt-") + uuid() + QLatin1Char('x')));
  QVERIFY(!isCanonicalAttemptId(uuid().toUpper()));
  QVERIFY(!isCanonicalAttemptId(QStringLiteral("../pwn")));
  QVERIFY(!isCanonicalAttemptId(QStringLiteral("a/b")));
  QVERIFY(!isCanonicalAttemptId(QStringLiteral("attempt-../x")));
  QVERIFY(!isCanonicalAttemptId(QString()));
  QVERIFY(!isCanonicalResultLeaf(uuid()));
  QVERIFY(!isCanonicalResultLeaf(QStringLiteral("result-../x")));
  // review R1 回归：缺连字符/连字符位被 hex 占据的 36 位串不是规范身份。
  QString dashless = uuid();
  dashless.replace(8, 1, QLatin1Char('0'));
  QVERIFY(!isCanonicalAttemptId(dashless));
  QVERIFY(!isCanonicalOperationId(QStringLiteral("op-") + dashless));
}

void AgentOperationNamespace::prefixMembershipIsSegmentExact() {
  const QString a = op();
  QString b = a;
  b[b.size() - 1] = b[b.size() - 1] == QLatin1Char('a') ? QLatin1Char('b') : QLatin1Char('a');
  const QString prefix = QStringLiteral(".paleo/capability-ops/");
  QVERIFY(isInPrivateOperationNamespace(prefix + a + QStringLiteral("/att0/vals.bin"), a));
  // op-a 不得凭字符串前缀认领 op-ab：段边界由 '/' 强制。
  QVERIFY(!isInPrivateOperationNamespace(prefix + a + QLatin1Char('x') + QStringLiteral("/att/vals.bin"), a));
  QVERIFY(!isInPrivateOperationNamespace(prefix + b + QStringLiteral("/att/vals.bin"), a));
  // 命名空间外与身份不合规的 owner。
  QVERIFY(!isInPrivateOperationNamespace(prefix + a, a));            // 无段边界斜杠
  QVERIFY(!isInPrivateOperationNamespace(QStringLiteral("artifacts/derived/x"), a));
  QVERIFY(!isInPrivateOperationNamespace(prefix + a + QStringLiteral("/f.bin"), QStringLiteral("op-a/../b")));
  QVERIFY(!isInPrivateOperationNamespace(prefix, a));
}

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  AgentOperationNamespace test;
  return QTest::qExec(&test, argc, argv);
}
#include "tst_agent_operation_namespace.moc"
