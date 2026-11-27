// 层：视图
#include "wellboresourcebutton.h"
#include "workflow/documentsession.h"
#include "workflow/wellboresourceconfirmation.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

namespace paleo::preview {
void addWellboreSourceButton(document::DocumentSession *session, const QString &versionId,
                             const QString &wellId, const QString &wellName, QWidget *host, QVBoxLayout *layout) {
    if (!session || session->readOnly()) return;
    const auto source = session->sourceForVersion(versionId);
    if (source.ref.modelStoreId != "area-model" || !source.fingerprint.valid()) return;
    auto *button = new QPushButton(QObject::tr("确认 LAS 所属井筒…"), host);
    layout->addWidget(button, 0, Qt::AlignLeft);
    const auto epoch = session->epoch();
    QObject::connect(button, &QPushButton::clicked, session, [=] {
        QPointer<QDialog> dialog = new QDialog(button);
        dialog->setWindowTitle(QObject::tr("确认 LAS 所属井筒"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        auto *form = new QFormLayout(dialog);
        form->addRow(QObject::tr("所属井"), new QLabel(wellName, dialog));
        auto *key = new QLineEdit(dialog);
        key->setPlaceholderText(QObject::tr("主井筒填 main，侧钻井筒填稳定编号"));
        auto *name = new QLineEdit(dialog);
        form->addRow(QObject::tr("井筒标识"), key);
        form->addRow(QObject::tr("井筒名称"), name);
        auto *issue = new QLabel(dialog);
        issue->setWordWrap(true);
        form->addRow(issue);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
        form->addRow(buttons);
        QObject::connect(buttons, &QDialogButtonBox::rejected, dialog.data(), &QDialog::reject);
        const auto revision = session->revision();
        QObject::connect(buttons, &QDialogButtonBox::accepted, session, [=] {
            if (key->text().trimmed().isEmpty() || name->text().trimmed().isEmpty()) {
                issue->setText(QObject::tr("请填写井筒标识和名称。"));
                return;
            }
            document::WellboreSourceConfirmation input{source.ref, versionId, wellId, key->text().trimmed(),
                                                        name->text().trimmed(), epoch, revision};
            const auto result = session->confirmWellboreSource(input);
            if (!dialog) return;
            if (!result.ok) {
                issue->setText(result.code == "wellbore_conflict" ? QObject::tr("该来源或井筒标识已有不同归属，请核对。") : result.error);
                return;
            }
            dialog->accept();
        });
        dialog->open();
    });
}
}
