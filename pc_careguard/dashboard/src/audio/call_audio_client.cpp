#include "audio/call_audio_client.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QIODevice>
#include <QMediaDevices>
#include <QWebSocket>

namespace {
constexpr int kSampleRate = 16000;
constexpr int kChannels = 1;
constexpr int kFrameMs = 20;
constexpr int kFrameSamples = kSampleRate * kFrameMs / 1000;  // 320
constexpr int kFrameBytes = kFrameSamples * 2;                // S16LE, 채널당 2바이트 * mono
constexpr int kOpusMaxPacketBytes = 4000;                     // opus_encode 출력 버퍼 상한(권장값)
constexpr int kPlaybackBufferBytes = kSampleRate * 2 * 100 / 1000;  // ~100ms 분량

QAudioFormat makeFormat() {
    QAudioFormat format;
    format.setSampleRate(kSampleRate);
    format.setChannelCount(kChannels);
    format.setSampleFormat(QAudioFormat::Int16);
    return format;
}
}  // namespace

CallAudioClient::CallAudioClient(QObject* parent) : QObject(parent) {}

CallAudioClient::~CallAudioClient() { stop(); }

QString CallAudioClient::wsUrlFor(const QString& serverUrl, const QString& robotCode,
                                   const QString& direction) const {
    QString url = QString(serverUrl).replace(QStringLiteral("https://"), QStringLiteral("wss://"))
                      .replace(QStringLiteral("http://"), QStringLiteral("ws://"));
    return QStringLiteral("%1/ws/dashboard/%2/audio/%3").arg(url, robotCode, direction);
}

void CallAudioClient::start(const QString& serverUrl, const QString& robotCode) {
    if (active_) return;
    active_ = true;

    int opusError = 0;
    encoder_ = opus_encoder_create(kSampleRate, kChannels, OPUS_APPLICATION_VOIP, &opusError);
    if (encoder_) opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(10));
    decoder_ = opus_decoder_create(kSampleRate, kChannels, &opusError);

    uplinkSocket_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(uplinkSocket_, &QWebSocket::binaryMessageReceived, this, &CallAudioClient::onUplinkBinaryMessage);
    connect(uplinkSocket_, &QWebSocket::disconnected, this, [this] {
        if (active_) emit audioDisconnected();
    });
    uplinkSocket_->open(QUrl(wsUrlFor(serverUrl, robotCode, QStringLiteral("uplink"))));

    downlinkSocket_ = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(downlinkSocket_, &QWebSocket::disconnected, this, [this] {
        if (active_) emit audioDisconnected();
    });
    downlinkSocket_->open(QUrl(wsUrlFor(serverUrl, robotCode, QStringLiteral("downlink"))));

    setupMicCapture();
    setupSpeakerPlayback();
}

void CallAudioClient::setupMicCapture() {
    micSource_ = new QAudioSource(QMediaDevices::defaultAudioInput(), makeFormat(), this);
    micDevice_ = micSource_->start();
    if (micDevice_ != nullptr) {
        connect(micDevice_, &QIODevice::readyRead, this, &CallAudioClient::onMicDataReady);
    }
}

void CallAudioClient::setupSpeakerPlayback() {
    speakerSink_ = new QAudioSink(QMediaDevices::defaultAudioOutput(), makeFormat(), this);
    speakerSink_->setBufferSize(kPlaybackBufferBytes);  // ~100ms 지터 버퍼 역할
    speakerDevice_ = speakerSink_->start();
}

void CallAudioClient::onMicDataReady() {
    if (micDevice_ == nullptr || encoder_ == nullptr) return;
    micPcmBuffer_.append(micDevice_->readAll());

    while (micPcmBuffer_.size() >= kFrameBytes) {
        const auto* pcm = reinterpret_cast<const opus_int16*>(micPcmBuffer_.constData());
        unsigned char encoded[kOpusMaxPacketBytes];
        const int encodedBytes = opus_encode(encoder_, pcm, kFrameSamples, encoded, kOpusMaxPacketBytes);
        micPcmBuffer_.remove(0, kFrameBytes);

        if (encodedBytes > 0 && downlinkSocket_ != nullptr &&
            downlinkSocket_->state() == QAbstractSocket::ConnectedState) {
            downlinkSocket_->sendBinaryMessage(
                QByteArray(reinterpret_cast<const char*>(encoded), encodedBytes));
        }
    }
}

void CallAudioClient::onUplinkBinaryMessage(const QByteArray& data) {
    if (decoder_ == nullptr || speakerDevice_ == nullptr) return;

    opus_int16 pcm[kFrameSamples * kChannels];
    const int decodedSamples = opus_decode(
        decoder_, reinterpret_cast<const unsigned char*>(data.constData()), data.size(), pcm, kFrameSamples, 0);
    if (decodedSamples <= 0) return;  // 손상된 프레임 — 이번 프레임만 건너뛴다

    speakerDevice_->write(reinterpret_cast<const char*>(pcm), decodedSamples * kChannels * 2);
}

void CallAudioClient::stop() {
    if (!active_) return;
    active_ = false;  // teardown 중 disconnected 시그널이 audioDisconnected를 다시 emit하지 않게 먼저 내린다
    teardown();
}

void CallAudioClient::teardown() {
    if (micSource_ != nullptr) {
        micSource_->stop();
        micSource_->deleteLater();
        micSource_ = nullptr;
    }
    micDevice_ = nullptr;

    if (speakerSink_ != nullptr) {
        speakerSink_->stop();
        speakerSink_->deleteLater();
        speakerSink_ = nullptr;
    }
    speakerDevice_ = nullptr;

    if (uplinkSocket_ != nullptr) {
        uplinkSocket_->close();
        uplinkSocket_->deleteLater();
        uplinkSocket_ = nullptr;
    }
    if (downlinkSocket_ != nullptr) {
        downlinkSocket_->close();
        downlinkSocket_->deleteLater();
        downlinkSocket_ = nullptr;
    }

    if (encoder_ != nullptr) {
        opus_encoder_destroy(encoder_);
        encoder_ = nullptr;
    }
    if (decoder_ != nullptr) {
        opus_decoder_destroy(decoder_);
        decoder_ = nullptr;
    }

    micPcmBuffer_.clear();
}
