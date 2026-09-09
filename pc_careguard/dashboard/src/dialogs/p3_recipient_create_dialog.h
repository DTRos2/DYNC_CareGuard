#pragma once
#include <QDialog>
#include <QHash>

#include "pages/p3_recipient_page.h"

class QLineEdit;
class QComboBox;
class QLabel;
class QPushButton;

// P3 대상자 등록 모달. gui/wireframe/v1/p3_recipient_create_modal.html 그대로 구현.
//
// recipient 등록 API가 아직 없어(P2/P4/P3 상세 모달과 동일한 방침) exec()이 Accepted로
// 끝나면 호출자가 result()로 완성된 RecipientCard+RecipientDetail을 가져가 인메모리에
// 반영한다. 로봇·맵은 "맵·로봇 매핑 페이지에서 등록된 항목만 선택 가능"이라는 와이어프레임
// 문구대로, 이미 배정된 로봇 하나당 맵 하나가 고정으로 딸려오는 이 앱의 기존 mock 관례
// (R-01~R-04 ↔ 행복빌라 302/은빛아파트 101/솔마을 7호/동산주택 2층)를 그대로 따른다 —
// 별도의 로봇/맵 카탈로그 데이터가 아직 없어서다.
class P3RecipientCreateDialog : public QDialog {
    Q_OBJECT
public:
    explicit P3RecipientCreateDialog(const QString& nextCode, const QVector<OperatorOption>& operators,
                                      QWidget* parent = nullptr);

    // exec() == Accepted 이후에만 유효하다.
    RecipientCard resultCard() const { return detail_.card; }
    RecipientDetail resultDetail() const { return detail_; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QWidget* buildPhotoBlock();
    QWidget* buildGuardianSection();
    void refreshGuardianTable();
    void addGuardian();
    void removeGuardian(int index);

    QWidget* buildScheduleSection();
    void refreshScheduleTable();
    void addSchedule();
    void removeSchedule(int index);

    void refreshValidation();
    void handleRegisterClicked();

    RecipientDetail detail_;  // guardians/schedules는 여기 바로 누적, card는 등록 클릭 시 채운다

    QLineEdit* nameEdit_ = nullptr;
    QLineEdit* birthYearEdit_ = nullptr;
    QComboBox* genderCombo_ = nullptr;
    QLineEdit* addressEdit_ = nullptr;
    QLineEdit* diseasesEdit_ = nullptr;
    QLineEdit* noteEdit_ = nullptr;
    QComboBox* robotCombo_ = nullptr;
    QComboBox* mapCombo_ = nullptr;
    QComboBox* operatorCombo_ = nullptr;

    QWidget* guardianTableBox_ = nullptr;
    int autoEditGuardianIndex_ = -1;

    QWidget* scheduleTableBox_ = nullptr;
    int autoEditScheduleIndex_ = -1;

    QLabel* requiredCountLabel_ = nullptr;
    QPushButton* registerBtn_ = nullptr;

    QString nextCode_;

    // eventFilter()의 IME 누수 방지용 — 위젯별 "이 시각까지는 새어 들어온 커밋을 삼킨다" 마감시각.
    QHash<QObject*, qint64> imeGuardDeadline_;
};
