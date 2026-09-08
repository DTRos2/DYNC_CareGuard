#pragma once
#include <QDate>
#include <QMap>
#include <QString>
#include <QVector>
#include <QWidget>

#include "theme/tokens.h"

class QLabel;
class QPushButton;
class QButtonGroup;
class QGridLayout;
class QFrame;
class QResizeEvent;
class QTimer;
class LiveSearchLineEdit;
class ApiClient;
class WsClient;
class QJsonObject;

// P3 대상자 관리. gui/wireframe/v1/p3_recipient_management_wireframe.html,
// CareGuard_DESIGN_SYSTEM.md §5-2(대상자 카드)·§6(idx 2)·§7-1/7-2(상태 판정·퀵액션) 기준.
//
// 시나리오서(docs/CareGuard_서비스_시나리오서_v2.docx §11.2)는 "웨어러블 장치를 사용하지
// 않는다. 심박 등 생체신호를 수집하거나 이를 근거로 위험을 판정하는 기능은 서비스 범위에서
// 제외한다"라고 명시한다 — 와이어프레임·디자인 시스템 문서의 심박 대형 숫자·스파크라인·
// 웨어러블 착용/배터리 표시는 전부 반영하지 않는다. 상태 판정은 care_event(안부 확인·복약·
// 로봇 통신)만 근거로 삼는다 (P2 이벤트 상세 모달에서 이미 같은 방침으로 정리됨).
//
// 대상자 등록 API가 아직 없어 인메모리 mock 데이터로 동작한다 (P2/P4와 동일한 방침).

enum class MedSlotState { Done, InProgress, Pending, Missed };

// TODO(team): recipient 테이블·care_event 조회 API가 생기면 이 struct와 mockRecipients()는
// 그쪽 모델로 옮기고 여기서는 삭제한다.
struct RecipientCard {
    QString code;       // "C-001"
    QString name;
    QString robotCode;  // "R-01"
    QString address;    // "행복빌라 302"
    Tokens::Level level;
    QString statusSummary;  // 상태 요약 한 줄 (§5-2, 심박 블록을 대체)
    MedSlotState meds[3] = {MedSlotState::Pending, MedSlotState::Pending, MedSlotState::Pending};
};

// 와이어프레임 그대로의 mock 데이터. 실 연동 전까지 GUI 검증용으로만 사용한다.
QVector<RecipientCard> mockRecipients();

// ---------- 상세 모달(gui/wireframe/v1/p3_modal_*.html) 전용 확장 데이터 ----------
// 시나리오서 §11.2에 따라 "웨어러블·임계값" 탭(p3_modal_tab2_wearable_threshold.html)은
// 구현하지 않는다 — 웨어러블 장치를 쓰지 않아 심박 임계값 설정 자체가 성립하지 않는다.

// guardian 테이블 대응. priority=0은 편집 불가 고정 행(119·112)을 의미한다.
struct GuardianContact {
    int priority = 0;
    QString name;
    QString relation;
    QString phone;
    QString notifyChannel;  // "문자 + 전화" 등 표시용
    bool isEmergencyFixed = false;
};

// medication_schedule 테이블 대응(slot_no/drug_name/dose_time/days_of_week).
// daysLabel은 days_of_week 비트마스크의 표시용 문자열이다(§10).
struct MedicationScheduleItem {
    int slotNo = 1;  // 1=아침 2=점심 3=저녁
    QString drugName;
    QString doseTime;  // "08:00"
    QString daysLabel;  // "매일" / "월·수·금"
};

// medication_log 결과를 하루 단위 수령률로 집계한 표시용 값.
struct MedicationDayRecord {
    QDate date;
    double adherence = -1.0;  // 0.0~1.0. -1 = 스케줄 없음/미래(빈 칸)
};

// TODO(team): recipient·guardian·medication_schedule/log 조회 API가 생기면 이 struct들과
// mockRecipientDetail()은 그쪽 모델로 옮기고 여기서는 삭제한다.
struct RecipientDetail {
    RecipientCard card;
    QString gender;     // "남" / "여"
    int birthYear = 0;  // 0 = 미상
    QString diseases;
    QString note;
    QVector<GuardianContact> guardians;
    QVector<MedicationScheduleItem> schedules;
    QVector<MedicationDayRecord> history;  // 최근 28일, 월요일 시작
    QString medAlertText;  // 미수령 등 안내 배너 본문. 비어 있으면 배너를 숨긴다
    int mapWaypointCount = 0;  // 담당 로봇의 배정 맵 WP 개수. 맵 이름은 card.address로 표시(§10)
};

RecipientDetail mockRecipientDetail(const RecipientCard& card);

class P3RecipientPage : public QWidget {
    Q_OBJECT
public:
    explicit P3RecipientPage(QWidget* parent = nullptr);

signals:
    // 긴급 카드의 "이벤트 확인" 클릭. MainWindow가 P2 이벤트 페이지로 전환 + 필터링에 연결한다.
    void eventCheckRequested(const QString& recipientName);

protected:
    // 창 크기가 바뀔 때마다 카드 그리드 열 수를 다시 계산해 반응형으로 재배치한다 (P4와 동일 패턴).
    void resizeEvent(QResizeEvent* event) override;

private:
    enum class SortMode { Risk, Name, Robot };

    QWidget* buildHeaderRow();
    void rebuildGrid();
    int computeColumnCount() const;
    QVector<int> visibleIndexesSorted() const;
    void openDetail(int recipientIndex);
    void registerRecipient();
    // ATTENTION 카드 "복약 재안내" 클릭. POST /missions로 실제 mission을 만들고 로봇에
    // 다운링크를 보낸다 — medication_scheduler가 없어 스케줄을 거치지 않는 즉시 실행.
    void requestMedicationReannounce(const QString& robotCode, const QString& recipientName);
    // /ws/dashboard가 push하는 robot_status(로봇 연결/해제)를 받아 OFFLINE 카드를 자동으로
    // 갱신한다 — "재연결 시도" 버튼 없이 로봇이 실제로 재연결되면 카드가 저절로 바뀐다.
    void handleWsMessage(const QJsonObject& message);

    ApiClient* apiClient_ = nullptr;
    WsClient* wsClient_ = nullptr;
    QVector<RecipientCard> recipients_;
    // "대상자 등록"으로 만든 대상자의 guardians/schedules 등 상세 데이터. mockRecipientDetail()은
    // C-001~004 외 코드를 모르므로(TODO: API 붙으면 이 지도는 통째로 삭제), 여기 있으면 이걸
    // 우선 쓴다(openDetail() 참고).
    QMap<QString, RecipientDetail> customDetails_;

    QWidget* gridPage_ = nullptr;
    QGridLayout* grid_ = nullptr;
    int columns_ = 2;  // resizeEvent()가 창 폭에 맞춰 갱신 — 초기값은 첫 rebuild 전 임시값

    LiveSearchLineEdit* searchEdit_ = nullptr;
    QButtonGroup* sortGroup_ = nullptr;
    SortMode sortMode_ = SortMode::Risk;  // P3 기본 정렬 (§6 페이지별 구현 주의점)

    // EMERGENCY 카드 배경 펄스 (§9 모션, Tokens::Motion::PULSE_PERIOD_MS). rebuildGrid()가
    // 매번 다시 채운다 — 그리드를 새로 만들 때마다 이전 카드는 삭제되므로.
    QVector<QFrame*> emergencyCards_;
    QTimer* pulseTimer_ = nullptr;
    bool pulseOn_ = true;
};
