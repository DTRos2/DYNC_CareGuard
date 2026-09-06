#pragma once
#include <QByteArray>
#include <QImage>
#include <QString>

class QIODevice;

// ROS map_server 스타일 .pgm(Netpbm grayscale) 파일을 QImage로 읽는다.
// P2(ASCII) / P5(binary) 두 서브포맷을 모두 지원한다. 실패 시 null QImage 반환.
namespace PgmImageLoader {
QImage load(const QString& path);
// 파일 대신 이미 메모리에 있는 바이트(예: 서버에서 다운로드한 응답 본문)를 읽는다.
QImage loadFromData(const QByteArray& data);
QImage load(QIODevice& device);
}
