#pragma once
#include <QByteArray>
#include <QObject>
#include <QString>

#include <opus/opus.h>

class QWebSocket;
class QAudioSource;
class QAudioSink;
class QIODevice;

// 이벤트 상세 모달 "스피커 통화" — 대시보드 쪽 마이크 캡처/스피커 재생 + Opus 인코딩/
// 디코딩 + 오디오 전용 WebSocket 2개(업링크 수신, 다운링크 송신)를 한 객체로 묶는다.
// 기존 JSON 채널(WsClient)과는 완전히 분리돼 있다 — 통화 시작/종료 신호는
// ApiClient::startCall/endCall(REST)이 담당하고, 이 클래스는 그 신호를 보낸 뒤 열고
// 닫는 순수 오디오 파이프일 뿐이다.
//
// 서버 쪽(app/ws/audio_relay.py) 엔드포인트:
//   /ws/dashboard/{robot_code}/audio/uplink   — 로봇 마이크 소리를 여기서 받는다(수신 전용).
//   /ws/dashboard/{robot_code}/audio/downlink — 이 대시보드 마이크 소리를 여기로 보낸다(송신 전용).
//
// 코덱은 LIMO(careguard_hmi/audio_capture_node.py, audio_downlink_player.py)와 반드시
// 같아야 한다: Opus, 16kHz, mono, 20ms 프레임(320 샘플/프레임).
//
// 오디오 처리(인코딩/디코딩)는 QAudioSource/QAudioSink의 readyRead 콜백 안에서 바로
// 처리한다 — Opus 20ms 프레임 인코딩/디코딩은 마이크로초 단위라 별도 QThread 없이도
// UI 이벤트 루프를 눈에 띄게 막지 않는다(spec이 허용한 "경량 콜백" 방식).
//
// 재생 쪽 지터 버퍼는 별도 큐를 직접 구현하지 않고 QAudioSink::setBufferSize()로
// ~100ms 분량을 잡아 QAudioSink 내부 버퍼가 그 역할을 하게 한다 — 디코딩된 PCM을
// 받는 즉시 그 버퍼에 write()만 하면, 실제 재생 페이싱은 오디오 장치 자체가 맞춘다
// (LIMO 쪽처럼 raw process pipe를 직접 페이싱해야 하는 상황이 아니다).
//
// 빌드/실행 미검증 — libopus-dev 설치, 실 마이크/스피커 장치에서 확인 필요.
class CallAudioClient : public QObject {
    Q_OBJECT
public:
    explicit CallAudioClient(QObject* parent = nullptr);
    ~CallAudioClient() override;

    // serverUrl은 ApiClient/WsClient와 같은 형식(http(s)://host:port). 이미 통화 중이면
    // 아무 것도 하지 않는다.
    void start(const QString& serverUrl, const QString& robotCode);
    void stop();

    bool isActive() const { return active_; }

signals:
    // 오디오 소켓 중 하나라도 연결이 끊기면 emit — 모달이 이걸 보고 "통화 종료"
    // 버튼을 눌린 것처럼 되돌릴 수 있다. 정상적인 stop() 호출 시에는 emit하지 않는다.
    void audioDisconnected();

private:
    void setupMicCapture();
    void setupSpeakerPlayback();
    void teardown();

    void onMicDataReady();
    void onUplinkBinaryMessage(const QByteArray& data);

    QString wsUrlFor(const QString& serverUrl, const QString& robotCode, const QString& direction) const;

    bool active_ = false;

    QWebSocket* uplinkSocket_ = nullptr;    // 수신: 로봇 마이크 -> 이 대시보드 스피커
    QWebSocket* downlinkSocket_ = nullptr;  // 송신: 이 대시보드 마이크 -> 로봇 스피커

    QAudioSource* micSource_ = nullptr;
    QIODevice* micDevice_ = nullptr;
    QAudioSink* speakerSink_ = nullptr;
    QIODevice* speakerDevice_ = nullptr;

    OpusEncoder* encoder_ = nullptr;
    OpusDecoder* decoder_ = nullptr;

    QByteArray micPcmBuffer_;  // 마이크에서 들어온 바이트를 20ms(640바이트) 단위로 자르기 전 임시 버퍼
};
