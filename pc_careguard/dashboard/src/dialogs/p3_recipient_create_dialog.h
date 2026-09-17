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
// exec()이 Accepted로 끝나면 호출자(P3RecipientPage::registerRecipient)가 result()로
// 완성된 RecipientCard+RecipientDetail을 가져가 POST /recipients로 실제 등록한다.
// 로봇·맵 콤보 후보는 호출자가 모달을 열기 전에 GET /robots·GET /maps/active로 받아와
// robots/maps 인자로 넘긴다(2026-09-15부터 — 예전엔 R-01~R-04 4대만 하드코딩돼 있어
// 그 이상 로봇이 콤보에 안 보였다). 서버에는 robotCode만 저장되고(map은 로봇에 이미
// 1:1로 딸려있는 값이라 대상자 레코드에 따로 저장하지 않는다) 맵 콤보는 "그 로봇이
// 맞는지 눈으로 확인하는" 용도 — 로봇/맵 어느 쪽을 먼저 골라도 나머지가 자동으로
// 맞춰진다(robotCombo_<->mapCombo_ 양방향 동기화).
class P3RecipientCreateDialog : public QDialog {
    Q_OBJECT
public:
    explicit P3RecipientCreateDialog(const QString& nextCode, const QVector<OperatorOption>& operators,
                                      const QVector<RobotOption>& robots, const QVector<MapOption>& maps,
                                      QWidget* parent = nullptr);

    // exec() == Accepted 이후에만 유효하다.
    RecipientCard resultCard() const { return detail_.card; }
    RecipientDetail resultDetail() const { return detail_; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // robots_[index-1]/maps_[index-1]가 이미 다른 대상자 배정 중(assignedRecipientName 비어있지
    // 않음)이면 "그 대상자에 더해 이 대상자도 추가할지" 확인 모달을 띄운다. 확인하면 true —
    // 로봇 한 대를 여러 대상자가 같이 담당할 수 있어(2026-09-15 정정 — 처음엔 "한 로봇=한
    // 대상자"로 잘못 가정해 재배정 경고 문구를 썼었다) DB는 기존 담당자를 그대로 둔다. 로봇·맵
    // 콤보 둘 다 같은 문구를 쓰므로 공유 헬퍼로 뺐다.
    bool confirmSharedAssignment(const QString& targetLabel, const QString& recipientName);

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
    QLineEdit* phoneEdit_ = nullptr;  // 대상자 본인 연락처(선택 입력, RecipientDetail::phone)
    QLineEdit* diseasesEdit_ = nullptr;
    QLineEdit* noteEdit_ = nullptr;
    QComboBox* robotCombo_ = nullptr;
    QComboBox* mapCombo_ = nullptr;
    QComboBox* operatorCombo_ = nullptr;
    QVector<RobotOption> robots_;
    QVector<MapOption> maps_;
    // robotCombo_<->mapCombo_ 양방향 동기화(하나를 고르면 다른 쪽도 맞춰줌) 도중
    // currentIndexChanged가 서로를 다시 부르는 무한루프를 막는 재진입 가드.
    bool syncingRobotMap_ = false;
    // 마지막으로 "확정된"(추가 확인을 통과했거나 애초에 확인이 필요 없었던) 콤보 인덱스 —
    // 사용자가 이미 배정된 로봇/맵을 골랐다가 확인 모달에서 취소하면 여기로 되돌린다. 둘 다
    // 콤보가 placeholder("로봇 선택"/"맵 선택")만 있는 index 0에서 시작하므로 기본값 0.
    int lastRobotIndex_ = 0;
    int lastMapIndex_ = 0;

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
