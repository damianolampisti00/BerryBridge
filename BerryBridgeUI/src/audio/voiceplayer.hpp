#ifndef VOICEPLAYER_HPP_
#define VOICEPLAYER_HPP_

#include <QObject>
#include <QString>
#include <QFutureWatcher>
#include <bb/multimedia/MediaState>
#include <bb/multimedia/MediaError>

namespace bb { namespace multimedia { class MediaPlayer; } }

// In-chat playback of audio/voice messages, exposed to QML as "voicePlayer".
// The upstream app handed every audio file to the system media previewer,
// which can't play Ogg/Opus at all -- the format of WhatsApp/Signal voice
// notes. Those are decoded to a WAV next to the downloaded file (once, with
// BBport's OggOpusDecoder, on a pool thread) and played here with
// bb::multimedia::MediaPlayer; anything else (m4a, mp3, aac...) plays as is.
class VoicePlayer : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString activeId READ activeId NOTIFY stateChanged)   // message being played/prepared
    Q_PROPERTY(bool playing READ playing NOTIFY stateChanged)
    Q_PROPERTY(bool preparing READ preparing NOTIFY stateChanged)    // decoding before the first play
    Q_PROPERTY(int position READ position NOTIFY progressChanged)    // ms
    Q_PROPERTY(int duration READ duration NOTIFY progressChanged)    // ms, 0 until known
public:
    explicit VoicePlayer(QObject *parent = 0);

    QString activeId() const { return m_activeId; }
    bool playing() const { return m_playing; }
    bool preparing() const { return m_preparing; }
    int position() const { return m_position; }
    int duration() const { return m_duration; }

    // Play this message's file; the same message again pauses/resumes it.
    Q_INVOKABLE void toggle(const QString &messageId, const QString &localPath);
    Q_INVOKABLE void stop();

signals:
    void stateChanged();
    void progressChanged();
    void failed(const QString &message);

private slots:
    void onPrepared();
    void onMediaStateChanged(bb::multimedia::MediaState::Type state);
    void onPositionChanged(unsigned int position);
    void onDurationChanged(unsigned int duration);
    void onPlaybackCompleted();
    void onError(bb::multimedia::MediaError::Type error, unsigned int position);

private:
    static QString playablePath(const QString &path);
    void playFile(const QString &path);
    void reset();

    bb::multimedia::MediaPlayer *m_player;
    QFutureWatcher<QString> m_prepareWatcher;
    QString m_activeId;
    QString m_preparingId;
    bool m_playing;
    bool m_preparing;
    int m_position;
    int m_duration;
};

#endif /* VOICEPLAYER_HPP_ */
