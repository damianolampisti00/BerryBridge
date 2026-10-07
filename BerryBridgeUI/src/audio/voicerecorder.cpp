#include "voicerecorder.hpp"
#include "oggopusencoder.hpp"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QMutexLocker>
#include <QtConcurrentRun>

#include <errno.h>
#include <string.h>
#include <sys/select.h>
#include <sys/asoundlib.h>
#include <audio/audio_manager_routing.h>

namespace {
const int kSampleRate = 48000;     // what OggOpusEncoder expects (Opus's native rate)
const int kBytesPerSecond = kSampleRate * 2; // 16-bit mono
const int kMaxSeconds = 5 * 60;    // ~29 MB of PCM; longer recordings are cut there
const int kMinMillis = 700;        // anything shorter was a mis-tap, not a message
const char *const kVoiceDir = "/accounts/1000/shared/misc/BerryBridge/audio";
}

// ---------------------------------------------------------------------------
// VoiceCaptureThread

VoiceCaptureThread::VoiceCaptureThread(QObject *parent) :
        QThread(parent),
        m_stop(false)
{
}

void VoiceCaptureThread::requestStop()
{
    QMutexLocker lock(&m_mutex);
    m_stop = true;
}

bool VoiceCaptureThread::stopRequested()
{
    QMutexLocker lock(&m_mutex);
    return m_stop;
}

void VoiceCaptureThread::run()
{
    snd_pcm_t *pcm = 0;
    unsigned int audioman = 0;
    int card = -1, device = 0;
    // Through the audio manager as a VOICE_RECORDING: routed like the system
    // recorder (built-in mic, or the headset's when one is plugged in).
    int rc = audio_manager_snd_pcm_open_preferred(AUDIO_TYPE_VOICE_RECORDING, &pcm, &audioman,
                                                  &card, &device, SND_PCM_OPEN_CAPTURE);
    if (rc < 0) {
        audioman = 0;
        rc = snd_pcm_open_preferred(&pcm, &card, &device, SND_PCM_OPEN_CAPTURE);
    }
    if (rc < 0) {
        m_error = QString("Microfono non disponibile (%1)").arg(snd_strerror(rc));
        return;
    }

    snd_pcm_channel_info_t info;
    memset(&info, 0, sizeof info);
    info.channel = SND_PCM_CHANNEL_CAPTURE;
    snd_pcm_plugin_info(pcm, &info);

    // No mmap transfers. With them the recording had isolated 32-sample
    // blocks (the driver's period, 0.67 ms) turned to near-silence in the
    // middle of speech -- ~100 audible clicks in 3.5 s, all at multiples of 32
    // samples, with no overrun reported (raw capture analysed 2026-10-05): the
    // classic DMA-buffer read race. read()-based transfer, as in QNX's own
    // wave.c capture sample, must be chosen before snd_pcm_plugin_params().
    snd_pcm_plugin_set_disable(pcm, PLUGIN_MMAP);

    // The plugin layer converts from whatever the hardware runs at to exactly
    // this format, so the encoder always gets 48 kHz mono S16LE.
    snd_pcm_channel_params_t params;
    memset(&params, 0, sizeof params);
    params.mode = SND_PCM_MODE_BLOCK;
    params.channel = SND_PCM_CHANNEL_CAPTURE;
    params.start_mode = SND_PCM_START_DATA;
    params.stop_mode = SND_PCM_STOP_STOP;
    params.buf.block.frag_size = info.max_fragment_size;
    params.buf.block.frags_max = -1;
    params.buf.block.frags_min = 1;
    params.format.interleave = 1;
    params.format.rate = kSampleRate;
    params.format.voices = 1;
    params.format.format = SND_PCM_SFMT_S16_LE;
    strncpy(params.sw_mixer_subchn_name, "Berry Bridge voice", sizeof params.sw_mixer_subchn_name - 1);

    rc = snd_pcm_plugin_params(pcm, &params);
    if (rc < 0) {
        m_error = QString("Configurazione microfono non riuscita (%1, %2)").arg(snd_strerror(rc)).arg(params.why_failed);
    } else if ((rc = snd_pcm_plugin_prepare(pcm, SND_PCM_CHANNEL_CAPTURE)) < 0) {
        m_error = QString("Avvio microfono non riuscito (%1)").arg(snd_strerror(rc));
    }

    if (m_error.isEmpty()) {
        snd_pcm_channel_setup_t setup;
        memset(&setup, 0, sizeof setup);
        setup.channel = SND_PCM_CHANNEL_CAPTURE;
        snd_pcm_plugin_setup(pcm, &setup);
        const int fragSize = setup.buf.block.frag_size > 0 ? setup.buf.block.frag_size : 4096;
        if (setup.format.rate != kSampleRate || setup.format.voices != 1) {
            qWarning() << "[VOICE] capture runs at" << setup.format.rate << "Hz," << setup.format.voices << "ch";
        }

        QByteArray buffer(fragSize, 0);
        const int fd = snd_pcm_file_descriptor(pcm, SND_PCM_CHANNEL_CAPTURE);
        const int maxBytes = kMaxSeconds * kBytesPerSecond;
        m_pcm.reserve(30 * kBytesPerSecond);

        while (!stopRequested() && m_pcm.size() < maxBytes) {
            // Short select() timeouts so a stop request is noticed promptly
            // even if the device delivers nothing.
            fd_set readFds;
            FD_ZERO(&readFds);
            FD_SET(fd, &readFds);
            struct timeval timeout = { 0, 200000 };
            const int ready = select(fd + 1, &readFds, 0, 0, &timeout);
            if (ready < 0) {
                if (errno == EINTR) continue;
                m_error = QString("Lettura microfono interrotta (%1)").arg(strerror(errno));
                break;
            }
            if (ready == 0) continue;

            const ssize_t n = snd_pcm_plugin_read(pcm, buffer.data(), fragSize);
            if (n < fragSize) {
                // Overrun (we fell behind) or not started yet: re-prepare and carry on.
                snd_pcm_channel_status_t status;
                memset(&status, 0, sizeof status);
                status.channel = SND_PCM_CHANNEL_CAPTURE;
                if (snd_pcm_plugin_status(pcm, &status) == 0
                        && (status.status == SND_PCM_STATUS_READY || status.status == SND_PCM_STATUS_OVERRUN)) {
                    snd_pcm_plugin_prepare(pcm, SND_PCM_CHANNEL_CAPTURE);
                }
            }
            if (n > 0) m_pcm.append(buffer.constData(), int(n));
        }
        snd_pcm_plugin_flush(pcm, SND_PCM_CHANNEL_CAPTURE);
    }

    snd_pcm_close(pcm);
    if (audioman) audio_manager_free_handle(audioman);
}

// ---------------------------------------------------------------------------
// VoiceRecorder

VoiceRecorder::VoiceRecorder(QObject *parent) :
        QObject(parent),
        m_capture(0),
        m_encodeAfterStop(false),
        m_busy(false)
{
    m_clock.invalidate(); // Qt 4's QElapsedTimer has no constructor
    m_tick.setInterval(250);
    connect(&m_tick, SIGNAL(timeout()), this, SLOT(onTick()));
    connect(&m_encodeWatcher, SIGNAL(finished()), this, SLOT(onEncoded()));
}

VoiceRecorder::~VoiceRecorder()
{
    if (m_capture) {
        m_capture->requestStop();
        m_capture->wait(2000);
    }
}

int VoiceRecorder::elapsedSeconds() const
{
    return m_clock.isValid() ? int(m_clock.elapsed() / 1000) : 0;
}

void VoiceRecorder::start()
{
    if (m_capture || m_busy) return;
    m_capture = new VoiceCaptureThread(this);
    connect(m_capture, SIGNAL(finished()), this, SLOT(onCaptureFinished()));
    m_encodeAfterStop = false;
    m_capture->start(QThread::TimeCriticalPriority); // starving it = overruns = audible clicks
    m_clock.start();
    m_tick.start();
    emit recordingChanged();
    emit elapsedChanged();
}

void VoiceRecorder::stopAndEncode()
{
    if (!m_capture) return;
    m_encodeAfterStop = true;
    m_capture->requestStop();
}

void VoiceRecorder::cancel()
{
    if (!m_capture) return;
    m_encodeAfterStop = false;
    m_capture->requestStop();
}

void VoiceRecorder::onTick()
{
    emit elapsedChanged();
}

void VoiceRecorder::onCaptureFinished()
{
    VoiceCaptureThread *capture = m_capture;
    m_capture = 0;
    m_tick.stop();
    const qint64 millis = m_clock.isValid() ? m_clock.elapsed() : 0;
    m_clock.invalidate();
    const QByteArray pcm = capture->pcm();
    const QString error = capture->error();
    capture->deleteLater();
    emit recordingChanged();
    emit elapsedChanged();

    if (!error.isEmpty() && pcm.isEmpty()) {
        qWarning() << "[VOICE]" << error;
        emit failed(error);
        return;
    }
    if (!m_encodeAfterStop) return; // cancelled
    if (millis < kMinMillis || pcm.size() < kBytesPerSecond / 2) {
        emit failed("Vocale troppo breve");
        return;
    }
    setBusy(true);
    m_encodeWatcher.setFuture(QtConcurrent::run(&VoiceRecorder::encode, pcm));
}

EncodedVoice VoiceRecorder::encode(const QByteArray &pcm)
{
    // Pool thread: libopus at 48 kHz takes a moment for a long message.
    EncodedVoice out;
    QByteArray ogg;
    if (!OggOpusEncoder::encodeFromPcm(pcm, 1, &ogg)) {
        out.error = "Codifica Opus non riuscita";
        return out;
    }
    QDir().mkpath(kVoiceDir);
    out.path = QString("%1/voice_%2.ogg").arg(kVoiceDir)
            .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
    QFile file(out.path);
    if (!file.open(QIODevice::WriteOnly) || file.write(ogg) != ogg.size()) {
        out.error = "Salvataggio del vocale non riuscito";
        return out;
    }
    out.durationSec = double(pcm.size()) / kBytesPerSecond;
    out.ok = true;
    return out;
}

void VoiceRecorder::onEncoded()
{
    const EncodedVoice result = m_encodeWatcher.result();
    setBusy(false);
    if (result.ok) emit finished("file://" + result.path, result.durationSec);
    else emit failed(result.error);
}

void VoiceRecorder::setBusy(bool busy)
{
    if (busy == m_busy) return;
    m_busy = busy;
    emit busyChanged();
}
