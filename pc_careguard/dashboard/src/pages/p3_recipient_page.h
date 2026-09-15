#pragma once
#include <QDate>
#include <QMap>
#include <QString>
#include <QStringList>
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
class QLineEdit;
class P3RecipientDetailDialog;

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
    // mockRecipients()의 4개 카드는 이 필드를 안 채워 0으로 남는다 — 상세 조회 시
    // id<=0이면 openDetail()이 mockRecipientDetail()로 폴백한다(실 DB에 없는 mock 행이라서).
    int id = 0;  // recipient.id. 0 = 서버에 없는 mock 카드
    int operatorId = 0;    // 담당 관제담당자 계정 id. 0 = 미배정
    QString operatorName;  // 미배정이면 빈 문자열 — 카드·상세 모달 표시용
    // 서버가 care_event로 판정한 위험도(Normal/Attention/Emergency만, Offline은 안 옴) —
    // level과 분리해서 따로 들고 있어야 한다. WS robot_status로 로봇이 재연결됐을 때 level을
    // Offline에서 되돌릴 값이 필요한데, 예전엔 무조건 Normal로 되돌려서 실제로는 위험 상태였던
    // 대상자도 재연결 순간 카드가 정상으로 잘못 표시되는 문제가 있었다(handleWsMessage 참고).
    // 새 필드를 struct 끝에 두는 이유: mockRecipients() 등 위치 기반 중괄호 초기화 호출부가
    // 이미 있어서, 중간에 끼우면 그 호출부들의 인자 순서가 전부 밀려 컴파일이 깨진다.
    Tokens::Level riskLevel = Tokens::Level::Normal;
};

// 대상자 등록/상세 모달의 "관리 담당자" 콤보 후보 1명. ApiClient::OperatorInfo의 화면 모델
// 버전 — 다이얼로그가 net/api_client.h에 직접 의존하지 않도록 얇게 감싼다(RecipientCard가
// ApiClient::RecipientInfo를 감싸는 것과 같은 이유).
struct OperatorOption {
    int id = 0;
    QString name;
};

// 대상자 등록/상세 모달의 "담당 로봇" 콤보 후보 1대 — ApiClient::RobotInfo의 화면 모델
// 버전(OperatorOption과 같은 이유). deregistered_at IS NULL인(등록 해제 안 된) 로봇 전부가
// 대상이다 — 예전엔 이 목록 자체가 없어 R-01~R-04만 하드코딩돼 있었다("R-05 이상은 콤보에
// 안 보인다" 리포트, 2026-09-15).
struct RobotOption {
    int id = 0;
    QString code;
    // 이 로봇을 이미 담당 중인 다른 대상자 이름. 미배정이면 빈 문자열 — P3RecipientPage가
    // recipients_(로드된 카드 목록)를 훑어 채운다. 상세 모달에서는 "지금 수정 중인 그 대상자
    // 자신의 로봇"은 재배정이 아니므로 빈 문자열로 둔다(요청: 이미 배정된 로봇/맵을 콤보에서
    // 표시하고, 고르면 "기존 대상자 배정이 초기화된다"는 확인 모달을 띄워달라, 2026-09-15).
    QString assignedRecipientName;
};

// 대상자 등록/상세 모달의 "담당 맵" 콤보 후보 1개 — ApiClient::MapInfo 중 화면에 필요한
// 필드만 남긴 버전. GET /maps/active가 내려주는 "로봇에 배정된 활성 맵"만 대상이라, 로봇이
// 정해지면 이 목록에서 robotCode가 같은 항목을 찾아 자동으로 맞춰줄 수 있다(반대 방향도
// 마찬가지 — 로봇을 몰라도 맵 이름으로 먼저 고르면 robotCombo_가 따라간다, 2026-09-15 요청).
// 실제로 서버에 저장되는 값은 robotCode뿐이라(RecipientCreateParams 참고), 이 맵 선택은
// "그 로봇이 맞는지 눈으로 확인하는" 용도지 별도로 서버에 보내는 필드가 아니다.
struct MapOption {
    int id = 0;
    QString robotCode;
    QString name;
    // ApiClient::MapInfo::recipientName 그대로 — RobotOption::assignedRecipientName과 같은
    // 이유·같은 자기 자신 제외 규칙(2026-09-15).
    QString assignedRecipientName;
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
    // guardian.notify_enabled(boolean)와 1:1 — 예전엔 "문자 + 전화" 같은 자유 텍스트였는데
    // DB엔 그걸 담을 칸이 없어(§project_p3_recipient_db_persistence) 저장 시 아무튼 boolean으로
    // 뭉개졌다. 아예 입력 단계부터 체크박스로 바꿔 DB 타입과 맞춘다.
    bool notifyEnabled = true;
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

// 복약 시각 자유 입력을 "HH:mm"으로 정규화한다. "9"/"09"/"9:30"/"930"/"0930"처럼 콜론·자릿수
// 형식을 몰라도 되게 다양한 표기를 받아들인다(요청: 시각 입력에 유연성을 달라). 못 알아보면
// 빈 문자열 — 호출부가 이걸로 저장 시점 유효성 검사를 한다. 생성/상세 모달 둘 다 공유한다.
QString normalizeDoseTime(const QString& raw);

// 보호자 연락처를 "010-1234-5678"류 하이픈 형식으로 점진적으로 정규화한다(요청: 입력하는
// 순간부터 하이픈이 보이도록). 자릿수가 안 늘어도(완성되지 않아도) 그때까지 입력한 만큼
// 자연스럽게 그룹을 나눠 보여준다 — QLineEdit::textEdited에 연결해 실시간으로도 쓸 수 있다.
QString formatGuardianPhone(const QString& raw);

// phoneEdit에 입력하는 즉시 formatGuardianPhone()으로 하이픈을 넣어 보여준다. 커서 위치도
// "그 앞에 숫자가 몇 개 있었는지" 기준으로 보정해, 하이픈이 새로 끼어들어도 커서가 엉뚱한
// 곳으로 튀지 않게 한다. 생성/상세 모달의 보호자 연락처 입력칸이 공유해서 쓴다.
void installLivePhoneFormatting(QLineEdit* phoneEdit);

class P3RecipientPage : public QWidget {
    Q_OBJECT
public:
    explicit P3RecipientPage(QWidget* parent = nullptr);

    // MainWindow가 앱 전체에서 공유하는 WsClient 하나를 내려준다 — 페이지마다 자기
    // WsClient를 따로 만들면 /ws/dashboard 소켓이 페이지 수만큼 중복으로 열린다.
    void setWsClient(WsClient* client);

    // role=guardian이면 userId(careguard_user.id)를 넘겨 자신이 연결된 대상자만 받도록
    // GET /recipients?guardian_user_id=를 쓴다. role이 그 외면 0을 넘겨 전체 목록으로 되돌린다.
    // MainWindow::setUserRole()가 로그인 직후 호출한다.
    void setGuardianUserId(int userId);

signals:
    // 긴급 카드의 "이벤트 확인" 클릭. MainWindow가 P2 이벤트 페이지로 전환 + 필터링에 연결한다.
    void eventCheckRequested(const QString& recipientName);
    // guardian 필터 적용 후(또는 해제 후) 대상자 이름 목록. MainWindow가 P2EventPage의
    // 이벤트 필터에도 같은 이름 목록을 넘겨, 보호자가 자신의 대상자 이벤트만 보게 한다.
    void guardianRecipientNamesChanged(const QStringList& names);

protected:
    // 창 크기가 바뀔 때마다 카드 그리드 열 수를 다시 계산해 반응형으로 재배치한다 (P4와 동일 패턴).
    void resizeEvent(QResizeEvent* event) override;

private:
    enum class SortMode { Risk, Name, Robot };

    QWidget* buildHeaderRow();
    void rebuildGrid();
    int computeColumnCount() const;
    QVector<int> visibleIndexesSorted() const;
    // GET /recipients. 실패하면(서버 미기동 등) mockRecipients()로 폴백해 화면은 계속 동작하게 한다.
    void loadRecipientsFromServer();
    void openDetail(int recipientIndex);
    void registerRecipient();
    // P3RecipientDetailDialog::saveRequested — 상세 모달 "저장" 클릭. PUT /recipients/{id}로
    // 기본정보·보호자·복약 스케줄을 실제로 갱신한다.
    void saveRecipientUpdate(const RecipientDetail& detail);
    // P3RecipientDetailDialog::deactivateRequested — 상세 모달 "대상자 비활성화" 확인 클릭.
    // POST /recipients/{id}/deactivate로 실제 처리하고, 성공하면 카드를 목록에서 뺀다.
    void deactivateRecipient(int recipientId);
    // ATTENTION 카드 "복약 재안내" 클릭. POST /missions로 실제 mission을 만들고 로봇에
    // 다운링크를 보낸다 — medication_scheduler가 없어 스케줄을 거치지 않는 즉시 실행.
    void requestMedicationReannounce(const QString& robotCode, const QString& recipientName);
    // /ws/dashboard가 push하는 robot_status(로봇 연결/해제)·recipient_changed(대상자 정보
    // 저장)를 받아 OFFLINE 카드/카드 목록을 자동으로 갱신한다 — "재연결 시도" 버튼 없이
    // 로봇이 실제로 재연결되거나, 다른 PC가 대상자 정보를 저장하면 카드가 저절로 바뀐다.
    void handleWsMessage(const QJsonObject& message);
    // recipient_changed가 왔을 때, 지금 그 대상자의 상세 모달이 열려 있으면(openDetailDialog_)
    // GET /recipients/{id}로 새로 받아와 그 모달도 새로고침 없이 갱신한다 — 안 하면 모달을
    // 열 때 받아온 스냅샷 그대로 남아, 닫았다 다시 열기 전까지 다른 PC의 수정(보호자·복약
    // 스케줄 등, 목록 API엔 없는 필드)이 안 보였다.
    void refreshOpenDetailDialog(int recipientId);

    ApiClient* apiClient_ = nullptr;
    WsClient* wsClient_ = nullptr;
    QVector<RecipientCard> recipients_;
    int guardianUserId_ = 0;  // 0 = 필터 없음(admin/operator)

    // 현재 열려 있는 상세 모달과 그 대상자 id — 없으면 -1/nullptr(다이얼로그는
    // WA_DeleteOnClose라 닫히면 destroyed 시그널에서 이 둘을 되돌린다).
    P3RecipientDetailDialog* openDetailDialog_ = nullptr;
    int openDetailRecipientId_ = -1;

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
