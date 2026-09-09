#pragma once
#include <QDialog>
#include <QHash>

#include "pages/p3_recipient_page.h"

class QLineEdit;
class QComboBox;

// P3 대상자 상세 모달. gui/wireframe/v1/p3_modal_tab1_basic_info_v2_photo.html(기본 정보),
// p3_modal_tab3_medication.html(복약) 대응.
//
// 시나리오서(docs/CareGuard_서비스_시나리오서_v2.docx §11.2) "웨어러블 장치를 사용하지
// 않는다"에 따라 p3_modal_tab2_wearable_threshold.html("웨어러블 · 임계값" 탭)은 구현하지
// 않는다 — 웨어러블 자체가 없으니 심박 임계값 설정이 성립하지 않는다. 안부 확인 기준 등
// 이와 유사한 전역 정책은 이미 P7 설정 § 임계값·알림 탭에 있다(대상자별이 아닌 시스템 전역).
//
// "저장" 클릭 시 이름·생년·성별·주소·지병·메모 + 보호자·복약 스케줄 전체를 saveRequested로
// 내보낸다 — 호출부(P3RecipientPage::saveRecipientUpdate)가 PUT /recipients/{id}로 실제
// 저장한다. 보호자·복약 스케줄의 행 단위 추가·삭제·수정은 이 다이얼로그 안(detail_)에서
// 먼저 반영되고, 최종 "저장"을 눌러야 서버에 실제로 올라간다. "대상자 비활성화"는 확인 후
// deactivateRequested를 내보내고 곧바로 닫는다(저장 버튼과 무관한 별도 동작). 사진 업로드는
// 여전히 안내 메시지로 대체한다.
class P3RecipientDetailDialog : public QDialog {
    Q_OBJECT
public:
    explicit P3RecipientDetailDialog(const RecipientDetail& detail, const QVector<OperatorOption>& operators,
                                      QWidget* parent = nullptr);

signals:
    void saveRequested(const RecipientDetail& detail);
    // "대상자 비활성화" 확인 후. 호출부(P3RecipientPage)가 POST /recipients/{id}/deactivate로
    // 실제 처리하고, 성공하면 카드를 목록에서 뺀다.
    void deactivateRequested(int recipientId);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QWidget* buildHeader();
    QWidget* buildBasicInfoTab();
    QWidget* buildMedicationTab();
    QWidget* buildFooter();

    QWidget* buildPhotoBlock();
    void populateGuardianTable(QWidget* box);
    void refreshGuardianTable();
    void addGuardian();
    void removeGuardian(int index);
    QWidget* buildTodayPillbox();
    void populateScheduleTable(QWidget* box);
    void refreshScheduleTable();
    void addSchedule();
    void removeSchedule(int index);
    QWidget* buildHistoryHeatmap();

    void showComingSoon(const QString& feature);
    void handleSaveClicked();

    RecipientDetail detail_;
    QVector<OperatorOption> operators_;  // "관리 담당자" 콤보 후보 — buildBasicInfoTab()에서 채운다
    QWidget* guardianTableBox_ = nullptr;
    int autoEditGuardianIndex_ = -1;  // addGuardian() 직후 이 행을 자동으로 수정 모드로 연다
    QWidget* scheduleTableBox_ = nullptr;
    int autoEditScheduleIndex_ = -1;  // addSchedule() 직후 이 행을 자동으로 수정 모드로 연다

    QLineEdit* nameEdit_ = nullptr;
    QLineEdit* birthYearEdit_ = nullptr;
    QComboBox* genderCombo_ = nullptr;
    QLineEdit* addressEdit_ = nullptr;
    QLineEdit* diseasesEdit_ = nullptr;
    QLineEdit* noteEdit_ = nullptr;
    QComboBox* robotCombo_ = nullptr;
    QComboBox* mapCombo_ = nullptr;  // 로봇 선택에 따라 자동 채워지는 표시 전용(제출 안 됨) — create dialog와 동일
    QComboBox* operatorCombo_ = nullptr;

    // eventFilter()의 IME 누수 방지용 — 위젯별 "이 시각까지는 새어 들어온 커밋을 삼킨다" 마감시각.
    QHash<QObject*, qint64> imeGuardDeadline_;
};
