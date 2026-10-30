#include <QtTest>
#define main referenceChecks
#include "../vendor/Data_service/tests/stratigraphy_template_guidance_tests.cpp"
#undef main
class ReferenceRegression : public QObject {
 Q_OBJECT
private slots:
 void zipCompatibility() { QCOMPARE(referenceChecks(), 0); }
};
QTEST_GUILESS_MAIN(ReferenceRegression)
#include "tst_stratigraphy_template_guidance.moc"
