#pragma once
#include <QDialog>
#include <QHash>

#include "pages/p3_recipient_page.h"

class QLineEdit;
class QComboBox;
class QLabel;

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
    // readOnly=true면(guardian 로그인) 편집·추가·삭제·저장·비활성화 버튼을 전부 빼고 조회만
    // 가능하게 연다 — 요청: guardian은 대상자 정보를 읽기만 할 수 있어야 한다.
    explicit P3RecipientDetailDialog(const RecipientDetail& detail, const QVector<OperatorOption>& operators,
                                      const QVector<RobotOption>& robots, const QVector<MapOption>& maps,
                                      bool readOnly = false, QWidget* parent = nullptr);

signals:
    void saveRequested(const RecipientDetail& detail);
    // "대상자 비활성화" 확인 후. 호출부(P3RecipientPage)가 POST /recipients/{id}/deactivate로
    // 실제 처리하고, 성공하면 카드를 목록에서 뺀다.
    void deactivateRequested(int recipientId);

public:
    // 이 모달이 열려 있는 동안 다른 곳(다른 PC의 P3, 또는 이 창의 P1)에서 같은 대상자
    // 정보를 저장해 WS recipient_changed가 왔을 때, 호출부(P3RecipientPage)가 GET
    // /recipients/{id}로 새로 받아온 값을 여기로 넘긴다. 보호자·복약 스케줄 행을 지금
    // 막 추가/수정 중이면(💾 저장 전, autoEditGuardianIndex_/autoEditScheduleIndex_) 그
    // 입력을 덮어써 잃어버리게 되므로 이번 갱신은 건너뛴다 — 모달을 닫았다 다시 열면
    // 최신값이 보인다. 최근 28일 복약 이력 히트맵(buildHistoryHeatmap())은 생성 시 한
    // 번만 그려지는 위젯이라 여기서는 갱신하지 않는다(닫았다 다시 열어야 최신 이력이 보임).
    void applyRemoteUpdate(const RecipientDetail& fresh);

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
    // p3_recipient_create_dialog.cpp::confirmSharedAssignment와 같은 모양·같은 이유 — 로봇 한
    // 대를 여러 대상자가 같이 담당할 수 있어(2026-09-15 정정) 추가 여부만 확인한다.
    bool confirmSharedAssignment(const QString& targetLabel, const QString& recipientName);

    RecipientDetail detail_;
    bool readOnly_ = false;
    QVector<OperatorOption> operators_;  // "관리 담당자" 콤보 후보 — buildBasicInfoTab()에서 채운다
    QWidget* guardianTableBox_ = nullptr;
    int autoEditGuardianIndex_ = -1;  // addGuardian() 직후 이 행을 자동으로 수정 모드로 연다
    QWidget* scheduleTableBox_ = nullptr;
    int autoEditScheduleIndex_ = -1;  // addSchedule() 직후 이 행을 자동으로 수정 모드로 연다

    // 헤더의 이름·"코드 · 로봇 · 주소" 표시 — applyRemoteUpdate()가 폼 입력칸과 같이 갱신한다.
    QLabel* headerNameLabel_ = nullptr;
    QLabel* headerIdInfoLabel_ = nullptr;

    QLineEdit* nameEdit_ = nullptr;
    QLineEdit* birthYearEdit_ = nullptr;
    QComboBox* genderCombo_ = nullptr;
    QLineEdit* addressEdit_ = nullptr;
    QLineEdit* phoneEdit_ = nullptr;  // 대상자 본인 연락처(선택 입력, RecipientDetail::phone)
    QLineEdit* diseasesEdit_ = nullptr;
    QLineEdit* noteEdit_ = nullptr;
    QComboBox* robotCombo_ = nullptr;
    QComboBox* mapCombo_ = nullptr;  // 표시 전용(제출 안 됨, robotCode만 실제로 저장됨) — create dialog와 동일
    QComboBox* operatorCombo_ = nullptr;
    QVector<RobotOption> robots_;
    QVector<MapOption> maps_;
    // create dialog와 같은 이유의 재진입 가드(robotCombo_<->mapCombo_ 양방향 동기화).
    bool syncingRobotMap_ = false;
    // create dialog와 같은 이유(추가 확인 취소 시 되돌릴 마지막 확정 인덱스). 여기는 애초에
    // 대상자 본인 로봇/맵이 미리 선택돼 있을 수 있어(buildBasicInfoTab() 참고) 0으로 초기화하지
    // 않고, 그 초기 선택을 반영한 뒤 실제 값으로 채운다.
    int lastRobotIndex_ = 0;
    int lastMapIndex_ = 0;
    // applyRemoteUpdate()가 다른 세션에서 이미 반영된 robotCode로 robotCombo_를 맞출 때는
    // 사용자가 고른 게 아니므로 추가 확인 모달을 건너뛴다(그 밑의 맵 자동 동기화는 그대로
    // 실행되어야 하므로 syncingRobotMap_과는 별도 플래그).
    bool suppressSharedAssignConfirm_ = false;

    // eventFilter()의 IME 누수 방지용 — 위젯별 "이 시각까지는 새어 들어온 커밋을 삼킨다" 마감시각.
    QHash<QObject*, qint64> imeGuardDeadline_;
};
