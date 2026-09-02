#include "pgm_image_loader.h"

#include <QFile>

namespace {

// 공백/개행으로 구분된 다음 토큰을 읽는다. '#'로 시작하는 줄은 주석으로 건너뛴다.
bool nextToken(QIODevice& dev, QByteArray& out) {
    out.clear();
    int ch;
    // 공백/주석 skip
    while (true) {
        char c;
        if (!dev.getChar(&c)) return false;
        ch = static_cast<unsigned char>(c);
        if (ch == '#') {
            char skip;
            while (dev.getChar(&skip) && skip != '\n') {
            }
            continue;
        }
        if (!QChar(ch).isSpace()) {
            out.append(c);
            break;
        }
    }
    // 토큰 본문
    while (true) {
        char c;
        if (!dev.getChar(&c)) break;
        if (QChar(static_cast<unsigned char>(c)).isSpace()) break;
        out.append(c);
    }
    return !out.isEmpty();
}

}  // namespace

QImage PgmImageLoader::load(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QImage();

    QByteArray magic;
    if (!nextToken(file, magic)) return QImage();
    const bool ascii = (magic == "P2");
    const bool binary = (magic == "P5");
    if (!ascii && !binary) return QImage();

    QByteArray widthTok, heightTok, maxvalTok;
    if (!nextToken(file, widthTok) || !nextToken(file, heightTok) || !nextToken(file, maxvalTok)) {
        return QImage();
    }
    bool ok = false;
    const int width = widthTok.toInt(&ok);
    if (!ok || width <= 0) return QImage();
    const int height = heightTok.toInt(&ok);
    if (!ok || height <= 0) return QImage();
    const int maxval = maxvalTok.toInt(&ok);
    if (!ok || maxval <= 0 || maxval > 65535) return QImage();

    QImage image(width, height, QImage::Format_Grayscale8);

    if (binary) {
        // 헤더 마지막 공백문자 하나는 이미 nextToken에서 소비됐으므로 바로 픽셀 데이터.
        const int bytesPerSample = maxval < 256 ? 1 : 2;
        const qint64 needed = static_cast<qint64>(width) * height * bytesPerSample;
        const QByteArray data = file.read(needed);
        if (data.size() < needed) return QImage();

        for (int y = 0; y < height; ++y) {
            uchar* row = image.scanLine(y);
            for (int x = 0; x < width; ++x) {
                const qint64 idx = (static_cast<qint64>(y) * width + x) * bytesPerSample;
                const int raw = bytesPerSample == 1
                                     ? static_cast<uchar>(data[idx])
                                     : ((static_cast<uchar>(data[idx]) << 8) | static_cast<uchar>(data[idx + 1]));
                row[x] = static_cast<uchar>((raw * 255) / maxval);
            }
        }
    } else {
        for (int y = 0; y < height; ++y) {
            uchar* row = image.scanLine(y);
            for (int x = 0; x < width; ++x) {
                QByteArray tok;
                if (!nextToken(file, tok)) return QImage();
                const int raw = tok.toInt(&ok);
                if (!ok) return QImage();
                row[x] = static_cast<uchar>((qBound(0, raw, maxval) * 255) / maxval);
            }
        }
    }

    return image;
}
