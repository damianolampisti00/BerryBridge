#ifndef VOICERECORDER_HPP_
#define VOICERECORDER_HPP_

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QByteArray>
#include <QString>
#include <QTimer>
#include <QElapsedTimer>
#include <QFutureWatcher>

// Voice messages without BerryCore. bb::multimedia::AudioRecorder only ever
// writes AAC-in-MP4, and turning that into the Ogg/Opus every messaging
// network renders as a real voice note needs an AAC decoder BB10 doesn't
// expose (BBport borrowed BerryCore's ffmpeg for it). So the microphone is
// read directly as 48 kHz mono PCM through QNX's own audio stack (libasound,
// opened via BB10's audio manager as a voice recording), and that PCM goes
// straight into OggOpusEncoder (libopus, vendored from BBport).

// One recording's PCM, captured on its own thread (blocking reads).
class VoiceCaptureThread : public QThread
{
    Q_OBJECT
public:
    explicit VoiceCaptureThread(QObject *parent = 0);
    void requestStop();
    // Valid once the thread has finished.
    QByteArray pcm() const { return m_pcm; }
    QString error() const { return m_error; }

protected:
    virtual void run();

private:
    bool stopRequested();
    QMutex m_mutex;
    bool m_stop;
    QByteArray m_pcm;
    QString m_error;
};

struct EncodedVoice {
    EncodedVoice() : ok(false), durationSec(0) {}
    bool ok;
    QString path;
    double durationSec;
    QString error;
};

// Exposed to QML as "voiceRecorder": start() -> stopAndEncode() ->
// finished(fileUrl, seconds) or failed(message); cancel() drops it.
class VoiceRecorder : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(int elapsedSeconds READ elapsedSeconds NOTIFY elapsedChanged)
public:
    explicit VoiceRecorder(QObject *parent = 0);
    virtual ~VoiceRecorder();

    bool recording() const { return m_capture != 0; }
    bool busy() const { return m_busy; } // encoding after stopAndEncode()
    int elapsedSeconds() const;

    Q_INVOKABLE void start();
    Q_INVOKABLE void stopAndEncode();
    Q_INVOKABLE void cancel();

signals:
    void recordingChanged();
    void busyChanged();
    void elapsedChanged();
    void finished(const QString &fileUrl, double durationSeconds);
    void failed(const QString &message);

private slots:
    void onTick();
    void onCaptureFinished();
    void onEncoded();

private:
    static EncodedVoice encode(const QByteArray &pcm);
    void setBusy(bool busy);

    VoiceCaptureThread *m_capture;
    bool m_encodeAfterStop;
    bool m_busy;
    QTimer m_tick;
    QElapsedTimer m_clock;
    QFutureWatcher<EncodedVoice> m_encodeWatcher;
};

#endif /* VOICERECORDER_HPP_ */
