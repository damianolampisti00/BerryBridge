#include "voiceplayer.hpp"
#include "oggopusdecoder.hpp"

#include <bb/multimedia/MediaPlayer>
#include <QDebug>
#include <QFile>
#include <QUrl>
#include <QtConcurrentRun>

using bb::multimedia::MediaPlayer;
using bb::multimedia::MediaState;
using bb::multimedia::MediaError;

VoicePlayer::VoicePlayer(QObject *parent) :
        QObject(parent),
        m_player(new MediaPlayer(this)),
        m_playing(false),
        m_preparing(false),
        m_position(0),
        m_duration(0)
{
    m_player->setStatusInterval(250); // smooth enough for the bubble's progress bar
    connect(m_player, SIGNAL(mediaStateChanged(bb::multimedia::MediaState::Type)),
            this, SLOT(onMediaStateChanged(bb::multimedia::MediaState::Type)));
    connect(m_player, SIGNAL(positionChanged(unsigned int)), this, SLOT(onPositionChanged(unsigned int)));
    connect(m_player, SIGNAL(durationChanged(unsigned int)), this, SLOT(onDurationChanged(unsigned int)));
    connect(m_player, SIGNAL(playbackCompleted()), this, SLOT(onPlaybackCompleted()));
    connect(m_player, SIGNAL(error(bb::multimedia::MediaError::Type, unsigned int)),
            this, SLOT(onError(bb::multimedia::MediaError::Type, unsigned int)));
    connect(&m_prepareWatcher, SIGNAL(finished()), this, SLOT(onPrepared()));
}

void VoicePlayer::toggle(const QString &messageId, const QString &localPath)
{
    if (messageId.isEmpty() || localPath.isEmpty()) return;
    if (messageId == m_activeId && !m_preparing) {
        if (m_playing) m_player->pause();
        else m_player->play();
        return;
    }
    if (messageId == m_preparingId) return; // already decoding it

    m_player->stop();
    reset();
    m_activeId = messageId;
    m_preparingId = messageId;
    m_preparing = true;
    emit stateChanged();
    emit progressChanged();

    QString path = localPath;
    if (path.startsWith("file://")) path = QUrl(path).toLocalFile();
    m_prepareWatcher.setFuture(QtConcurrent::run(&VoicePlayer::playablePath, path));
}

void VoicePlayer::stop()
{
    m_player->stop();
    reset();
    emit stateChanged();
    emit progressChanged();
}

// Pool thread. Ogg/Opus -> "<file>.wav" (decoded once, then reused);
// any other file is returned as is. Empty on a decode failure.
QString VoicePlayer::playablePath(const QString &path)
{
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly)) return QString();
    const QByteArray head = in.peek(64);
    if (!head.startsWith("OggS") || !head.contains("OpusHead")) return path;

    const QString wavPath = path + ".wav";
    if (QFile::exists(wavPath)) return wavPath;
    QByteArray wav;
    if (!OggOpusDecoder::decodeToWav(in.readAll(), &wav)) return QString();
    QFile out(wavPath);
    if (!out.open(QIODevice::WriteOnly) || out.write(wav) != wav.size()) {
        out.remove();
        return QString();
    }
    return wavPath;
}

void VoicePlayer::onPrepared()
{
    const QString path = m_prepareWatcher.result();
    const QString id = m_preparingId;
    m_preparingId.clear();
    if (id != m_activeId) return; // superseded by another message meanwhile
    m_preparing = false;
    if (path.isEmpty()) {
        reset();
        emit stateChanged();
        emit failed("Impossibile riprodurre questo audio");
        return;
    }
    emit stateChanged();
    playFile(path);
}

void VoicePlayer::playFile(const QString &path)
{
    m_player->setSourceUrl(QUrl::fromLocalFile(path));
    const MediaError::Type err = m_player->play();
    if (err != MediaError::None) {
        qWarning() << "[VOICE] play failed" << err << path;
        reset();
        emit stateChanged();
        emit failed("Impossibile riprodurre questo audio");
    }
}

void VoicePlayer::onMediaStateChanged(MediaState::Type state)
{
    const bool playing = (state == MediaState::Started);
    if (playing == m_playing) return;
    m_playing = playing;
    emit stateChanged();
}

void VoicePlayer::onPositionChanged(unsigned int position)
{
    m_position = int(position);
    emit progressChanged();
}

void VoicePlayer::onDurationChanged(unsigned int duration)
{
    m_duration = int(duration);
    emit progressChanged();
}

void VoicePlayer::onPlaybackCompleted()
{
    // Back to the start, inactive: the bubble shows "play" again.
    m_player->stop();
    reset();
    emit stateChanged();
    emit progressChanged();
}

void VoicePlayer::onError(MediaError::Type error, unsigned int position)
{
    Q_UNUSED(position);
    qWarning() << "[VOICE] media error" << error;
    reset();
    emit stateChanged();
    emit failed("Impossibile riprodurre questo audio");
}

void VoicePlayer::reset()
{
    m_activeId.clear();
    m_playing = false;
    m_preparing = false;
    m_position = 0;
    m_duration = 0;
}
