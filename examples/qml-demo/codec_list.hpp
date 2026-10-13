// "av1,hevc,h264" and "opus,aac,pcm"-style codec lists (most preferred first)
// for the QML demo.
#pragma once

#include <rcsdk/rcsdk.h>

#include <QString>
#include <QStringList>

#include <algorithm>

namespace demo {

// Fills a codecs[] field; false for an unknown name or more than it holds.
template <class Codec, size_t N, class Name>
bool setList(Codec (&codecs)[N], const QString& list, Name codecOf) {
    std::fill(std::begin(codecs), std::end(codecs), Codec{});
    const QStringList names = list.split(u',', Qt::SkipEmptyParts);
    if (names.isEmpty() || names.size() > qsizetype(N)) {
        return false;
    }
    for (qsizetype i = 0; i < names.size(); ++i) {
        codecs[i] = codecOf(names[i].trimmed().toLower());
        if (!codecs[i]) {
            return false;
        }
    }
    return true;
}

inline rcs_codec videoCodec(const QString& name) {
    return name == u"h264" ? RCS_CODEC_H264 : name == u"hevc" ? RCS_CODEC_HEVC : name == u"av1" ? RCS_CODEC_AV1 : 0;
}

inline rcs_audio_codec audioCodec(const QString& name) {
    return name == u"opus" ? RCS_AUDIO_OPUS : name == u"aac" ? RCS_AUDIO_AAC : name == u"pcm" ? RCS_AUDIO_PCM : 0;
}

inline bool setCodecList(rcs_codec (&codecs)[RCS_MAX_CODECS], const QString& list) {
    return setList(codecs, list, videoCodec);
}

inline bool setAudioCodecList(rcs_audio_codec (&codecs)[RCS_MAX_AUDIO_CODECS], const QString& list) {
    return setList(codecs, list, audioCodec);
}

} // namespace demo
