#pragma once
#include <QAbstractTableModel>
#include <QDate>
#include <QVector>

// P2 이벤트 관제 테이블 모델. CareGuard_DESIGN_SYSTEM.md §4-3 규약을 따른다.
// 행 배경·좌측 바 등 시각 상태는 RowKindRole로 델리게이트(EventRowDelegate)에 전달하고,
// 실제 색상 계산은 델리게이트에서 tokens.h 상수로 한다 (모델은 상태 분류만 담당).
class EventTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column {
        ColCheck = 0,
        ColSeverity,
        ColEventId,
        ColTime,
        ColRobot,
        ColRecipient,
        ColType,
        ColSummary,
        ColStatus,
        ColAssignee,
        ColElapsed,
        ColumnCount
    };

    // 델리게이트가 행 배경/좌측 바를 고르는 기준. 우선순위는 그대로 정렬 버킷으로도 쓴다
    // (EventFilterProxyModel::lessThan).
    enum class RowKind { Normal = 2, Attention = 1, Emergency = 0 };

    enum Role {
        RowKindRole = Qt::UserRole + 1,
        ResolvedRole,
        IsEmergencyRole,
        DateRole,
    };

    enum class Severity { Info, Attention, Emergency };

    struct EventRow {
        Severity severity;
        QString eventId;
        QDate date;           // 기간 필터 대상. 화면 표시는 timeText를 그대로 쓴다.
        QString timeText;
        QString robotId;
        QString recipientName;
        QString type;
        QString summary;
        QString assignee;    // 비어있으면 "—"로 표시
        bool resolved = false;
        int elapsedSec = 0;  // resolved면 표시 안 함
        bool checked = false;
    };

    explicit EventTableModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    void setRows(const QVector<EventRow>& rows);

    // 상세 모달이 행 클릭 시점의 데이터를 그대로 스냅샷으로 받아 쓴다.
    EventRow rowAt(int row) const { return rows_.value(row); }

    // 상세 모달의 "RESOLVED 전환"에서 호출. eventId로 찾아 완료 처리한다.
    void resolveByEventId(const QString& eventId);

    // 미처리 행의 경과 시간을 1초 증가시킨다 (P2EventPage가 QTimer로 호출).
    void tickElapsed();

    // 체크된 행 중 EMERGENCY가 아니고 아직 미완료인 것의 eventId 목록. 일괄 완료 처리는
    // 서버에 완료 요청을 먼저 보내야 하므로(P2 이벤트 API 연동) 여기서는 모델을 바꾸지 않고
    // 대상만 골라준다 — 페이지가 이 목록으로 ApiClient::resolveEvent를 호출하고, 성공 응답이
    // 온 것만 resolveByEventId()로 반영한다.
    QVector<QString> checkedResolvableEventIds() const;
    // 체크된 행 중 EMERGENCY가 하나라도 있으면 true — 페이지가 "제외됨" 안내를 띄우는 데 쓴다.
    bool hasEmergencyChecked() const;

    void clearAllChecks();

    int checkedCount() const;
    int emergencyUnresolvedCount() const;
    int attentionUnresolvedCount() const;
    int totalCount() const { return rows_.size(); }
    int unresolvedCount() const;

signals:
    void countsChanged();

private:
    static RowKind rowKind(const EventRow& row);
    static QString formatElapsed(int sec);

    QVector<EventRow> rows_;
};
