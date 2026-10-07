#ifndef CALLAUDIO_HPP_
#define CALLAUDIO_HPP_

#include <QAtomicInt>
#include <QString>
#include <QThread>

#include "audioring.hpp"

struct snd_pcm;

// Real-time call audio on the phone, separate from the voice-message
// recorder/player of the UI (those record a whole message into memory; a
// call streams small blocks both ways). Format: S16LE mono at `rate`.
struct CallAudioConfig
{
    CallAudioConfig() : rate(16000), fragMs(20), jitterMs(80) {}
    int rate;
    int fragMs;      // capture/playback block size
    int jitterMs;    // playback pre-buffer (and the latency it tolerates)
    QString type;    // audio manager type name ("voice", "video_chat", "voice_recording"...); "none" = no audio manager
    QString capture; // PCM device name ("voicec", "voice_procc"...); empty = preferred
    QString playback;// PCM device name ("voicep", "voice_procp"...); empty = preferred
    QString output;  // preferred output device ("handset", "speaker"...); empty = the type's default
    QString dumpPrefix; // non-empty: raw PCM of what is captured/played goes to <prefix>capture.raw/playback.raw
    int bytesPerFrag() const { return rate * 2 * fragMs / 1000; }
};

// Shared between the threads for the echo probe: 1 while the playback side
// is writing the probe signal, 0 while it writes silence.
extern QAtomicInt g_callProbeTone;

class CallAudioInput : public QThread
{
    Q_OBJECT
public:
    // `sink` may be 0 (measure only).
    CallAudioInput(const CallAudioConfig &cfg, AudioRing *sink, QObject *parent = 0);
    void requestStop() { m_stop = 1; }
    QString error() const { return m_error; }

    // Stats, read by the controller while running.
    QAtomicInt bytesCaptured;
    QAtomicInt overruns;
    QAtomicInt actualRate;
    QAtomicInt actualFrag;
    QAtomicInt actualFrags; // fragments the device queue holds
    // Echo probe: mean square of the captured signal while the probe tone
    // plays vs while it is silent (sum and sample count, in 1/16 units).
    qint64 probeOnSum, probeOnCount, probeOffSum, probeOffCount;
    QMutex probeMutex;

protected:
    void run();

private:
    CallAudioConfig m_cfg;
    AudioRing *m_sink;
    QAtomicInt m_stop;
    QString m_error;
};

class CallAudioOutput : public QThread
{
    Q_OBJECT
public:
    // `source` may be 0: plays the echo-probe signal instead (1 s noise
    // bursts, 1 s silence).
    CallAudioOutput(const CallAudioConfig &cfg, AudioRing *source, QObject *parent = 0);
    void requestStop() { m_stop = 1; if (m_source) m_source->wakeReaders(); }
    QString error() const { return m_error; }

    QAtomicInt bytesPlayed;
    QAtomicInt underruns;     // device ran dry (it stopped)
    QAtomicInt concealed;     // blocks of silence written because no audio had arrived
    QAtomicInt dropped;       // blocks discarded to keep the latency bounded
    QAtomicInt actualRate;
    QAtomicInt actualFrag;
    QAtomicInt actualFrags;

protected:
    void run();

private:
    CallAudioConfig m_cfg;
    AudioRing *m_source;
    QAtomicInt m_stop;
    QString m_error;
};

#endif /* CALLAUDIO_HPP_ */
