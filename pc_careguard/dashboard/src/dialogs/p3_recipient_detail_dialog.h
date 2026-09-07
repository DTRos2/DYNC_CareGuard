#pragma once
#include <QDialog>
#include <QHash>

#include "pages/p3_recipient_page.h"

class QLineEdit;

// P3 대상자 상세 모달. gui/wireframe/v1/p3_modal_tab1_basic_info_v2_photo.html(기본 정보),
// p3_modal_tab3_medication.html(복약) 대응.
//
// 시나리오서(docs/CareGuard_서비스_시나리오서_v2.docx §11.2) "웨어러블 장치를 사용하지
// 않는다"에 따라 p3_modal_tab2_wearable_threshold.html("웨어러블 · 임계값" 탭)은 구현하지
// 않는다 — 웨어러블 자체가 없으니 심박 임계값 설정이 성립하지 않는다. 안부 확인 기준 등
// 이와 유사한 전역 정책은 이미 P7 설정 § 임계값·알림 탭에 있다(대상자별이 아닌 시스템 전역).
//
// recipient·guardian·medication_schedule/log 조회·저장 API가 아직 없어
// mockRecipientDetail()의 스냅샷으로 화면을 구성한다. 저장 시 이름·주소만 상위 페이지의
// 카드에 실제로 반영되고(basicInfoSaved), 보호자 추가·삭제·수정과 복약 스케줄 추가·수정은
// detail_.guardians/schedules를 직접 고쳐 이 모달 안에서만 반영한다(재조회 API가 없어
// 다이얼로그를 닫으면 사라짐). 그 외(스케줄 삭제, 사진 업로드, 대상자 비활성화)는 여전히
// 안내 메시지로 대체한다.
class P3RecipientDetailDialog : public QDialog {
    Q_OBJECT
public:
    explicit P3RecipientDetailDialog(const RecipientDetail& detail, QWidget* parent = nullptr);

signals:
    void basicInfoSaved(const QString& name, const QString& address);

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
    QWidget* guardianTableBox_ = nullptr;
    int autoEditGuardianIndex_ = -1;  // addGuardian() 직후 이 행을 자동으로 수정 모드로 연다
    QWidget* scheduleTableBox_ = nullptr;
    int autoEditScheduleIndex_ = -1;  // addSchedule() 직후 이 행을 자동으로 수정 모드로 연다

    QLineEdit* nameEdit_ = nullptr;
    QLineEdit* birthYearEdit_ = nullptr;
    QLineEdit* genderEdit_ = nullptr;
    QLineEdit* addressEdit_ = nullptr;
    QLineEdit* diseasesEdit_ = nullptr;
    QLineEdit* noteEdit_ = nullptr;

    // eventFilter()의 IME 누수 방지용 — 위젯별 "이 시각까지는 새어 들어온 커밋을 삼킨다" 마감시각.
    QHash<QObject*, qint64> imeGuardDeadline_;
};
