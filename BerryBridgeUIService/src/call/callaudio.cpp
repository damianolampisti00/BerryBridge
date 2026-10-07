#include "callaudio.hpp"

#include <QByteArray>
#include <QDebug>
#include <QFile>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/asoundlib.h>
#include <audio/audio_manager_device.h>
#include <audio/audio_manager_routing.h>

QAtomicInt g_callProbeTone(0);

namespace {

// Opens one direction of the call audio as configured: through the audio
// manager (so the system routes it like a call: earpiece/speaker/headset,
// and pauses media for it) unless the type is "none". Returns 0 and sets
// *error on failure; *audioman is the handle to free afterwards (or 0).
snd_pcm_t *openPcm(const CallAudioConfig &cfg, bool capture, unsigned int *audioman,
                   int *fragBytes, int *rate, int *frags, QString *error)
{
    const int mode = capture ? SND_PCM_OPEN_CAPTURE : SND_PCM_OPEN_PLAYBACK;
    const int channel = capture ? SND_PCM_CHANNEL_CAPTURE : SND_PCM_CHANNEL_PLAYBACK;
    const QString name = capture ? cfg.capture : cfg.playback;
    snd_pcm_t *pcm = 0;
    *audioman = 0;
    int rc;
    int card = -1, device = 0;

    if (cfg.type == "none") {
        rc = name.isEmpty() ? snd_pcm_open_preferred(&pcm, &card, &device, mode)
                            : snd_pcm_open_name(&pcm, name.toLatin1().constData(), mode);
    } else {
        const audio_manager_audio_type_t type =
                audio_manager_get_type_from_name(cfg.type.isEmpty() ? "voice" : cfg.type.toLatin1().constData());
        if (name.isEmpty()) {
            rc = audio_manager_snd_pcm_open_preferred(type, &pcm, audioman, &card, &device, mode);
        } else {
            QByteArray n = name.toLatin1();
            rc = audio_manager_snd_pcm_open_name(type, &pcm, audioman, n.data(), mode);
        }
        if (rc >= 0 && *audioman && !cfg.output.isEmpty()) {
            const audio_manager_device_t out = audio_manager_get_device_from_name(cfg.output.toLatin1().constData());
            const int r2 = audio_manager_set_handle_type(*audioman, type, out, AUDIO_DEVICE_UNCHANGED);
            if (r2 < 0) qWarning() << "[CALLAUDIO] set_handle_type" << cfg.output << "failed:" << r2;
        }
    }
    if (rc < 0) {
        *error = QString("open %1 '%2' type '%3': %4").arg(capture ? "capture" : "playback")
                 .arg(name.isEmpty() ? QString("preferred") : name).arg(cfg.type).arg(snd_strerror(rc));
        if (*audioman) { audio_manager_free_handle(*audioman); *audioman = 0; }
        return 0;
    }

    // read()/write() transfers, not mmap: mmap capture had 32-sample dropouts
    // on this phone (see VoiceCaptureThread in the UI).
    snd_pcm_plugin_set_disable(pcm, PLUGIN_MMAP);

    snd_pcm_channel_params_t params;
    memset(&params, 0, sizeof params);
    params.mode = SND_PCM_MODE_BLOCK;
    params.channel = channel;
    params.start_mode = capture ? SND_PCM_START_DATA : SND_PCM_START_FULL;
    params.stop_mode = SND_PCM_STOP_STOP;
    params.buf.block.frag_size = cfg.bytesPerFrag();
    params.buf.block.frags_min = 1;
    // Playback: a short device queue keeps the latency in OUR jitter buffer,
    // where it is measured and bounded.
    params.buf.block.frags_max = capture ? -1 : 3;
    params.format.interleave = 1;
    params.format.rate = cfg.rate;
    params.format.voices = 1;
    params.format.format = SND_PCM_SFMT_S16_LE;
    strncpy(params.sw_mixer_subchn_name, "Berry Bridge call", sizeof params.sw_mixer_subchn_name - 1);
    rc = snd_pcm_plugin_params(pcm, &params);
    if (rc < 0) {
        *error = QString("params %1: %2 (%3)").arg(capture ? "capture" : "playback").arg(snd_strerror(rc)).arg(params.why_failed);
    } else if ((rc = snd_pcm_plugin_prepare(pcm, channel)) < 0) {
        *error = QString("prepare %1: %2").arg(capture ? "capture" : "playback").arg(snd_strerror(rc));
    }
    if (!error->isEmpty()) {
        snd_pcm_close(pcm);
        if (*audioman) { audio_manager_free_handle(*audioman); *audioman = 0; }
        return 0;
    }

    snd_pcm_channel_setup_t setup;
    memset(&setup, 0, sizeof setup);
    setup.channel = channel;
    snd_pcm_plugin_setup(pcm, &setup);
    *fragBytes = setup.buf.block.frag_size > 0 ? setup.buf.block.frag_size : cfg.bytesPerFrag();
    *rate = setup.format.rate;
    *frags = setup.buf.block.frags;
    return pcm;
}

void recover(snd_pcm_t *pcm, int channel, QAtomicInt *counter)
{
    snd_pcm_channel_status_t status;
    memset(&status, 0, sizeof status);
    status.channel = channel;
    if (snd_pcm_plugin_status(pcm, &status) == 0
            && (status.status == SND_PCM_STATUS_READY || status.status == SND_PCM_STATUS_OVERRUN
                || status.status == SND_PCM_STATUS_UNDERRUN)) {
        if (status.status != SND_PCM_STATUS_READY && counter) counter->ref();
        snd_pcm_plugin_prepare(pcm, channel);
    }
}

} // namespace

// ---------------------------------------------------------------------------

CallAudioInput::CallAudioInput(const CallAudioConfig &cfg, AudioRing *sink, QObject *parent) :
        QThread(parent),
        probeOnSum(0), probeOnCount(0), probeOffSum(0), probeOffCount(0),
        m_cfg(cfg), m_sink(sink), m_stop(0)
{
}

void CallAudioInput::run()
{
    unsigned int audioman = 0;
    int frag = 0, rate = 0, frags = 0;
    snd_pcm_t *pcm = openPcm(m_cfg, true, &audioman, &frag, &rate, &frags, &m_error);
    if (!pcm) return;
    actualRate = rate;
    actualFrag = frag;
    actualFrags = frags;
    QFile dump(m_cfg.dumpPrefix + "capture.raw");
    if (!m_cfg.dumpPrefix.isEmpty()) dump.open(QIODevice::WriteOnly | QIODevice::Truncate);

    QByteArray buffer(frag, 0);
    const int fd = snd_pcm_file_descriptor(pcm, SND_PCM_CHANNEL_CAPTURE);
    while (!m_stop) {
        fd_set readFds;
        FD_ZERO(&readFds);
        FD_SET(fd, &readFds);
        struct timeval timeout = { 0, 200000 };
        const int ready = select(fd + 1, &readFds, 0, 0, &timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            m_error = QString("capture select: %1").arg(strerror(errno));
            break;
        }
        if (ready == 0) continue;
        const ssize_t n = snd_pcm_plugin_read(pcm, buffer.data(), frag);
        if (n < frag) recover(pcm, SND_PCM_CHANNEL_CAPTURE, &overruns);
        if (n <= 0) continue;
        bytesCaptured.fetchAndAddRelaxed(int(n));
        if (dump.isOpen()) dump.write(buffer.constData(), n);
        if (m_sink) m_sink->write(buffer.constData(), int(n));

        // Echo probe bookkeeping (cheap: one pass over a 20 ms block).
        const qint16 *s = reinterpret_cast<const qint16 *>(buffer.constData());
        qint64 sum = 0;
        for (int i = 0; i < int(n) / 2; ++i) sum += (qint64(s[i]) * s[i]) >> 4;
        QMutexLocker lock(&probeMutex);
        if (int(g_callProbeTone)) { probeOnSum += sum; probeOnCount += n / 2; }
        else { probeOffSum += sum; probeOffCount += n / 2; }
    }
    snd_pcm_plugin_flush(pcm, SND_PCM_CHANNEL_CAPTURE);
    snd_pcm_close(pcm);
    if (audioman) audio_manager_free_handle(audioman);
}

// ---------------------------------------------------------------------------

CallAudioOutput::CallAudioOutput(const CallAudioConfig &cfg, AudioRing *source, QObject *parent) :
        QThread(parent),
        m_cfg(cfg), m_source(source), m_stop(0)
{
}

void CallAudioOutput::run()
{
    unsigned int audioman = 0;
    int frag = 0, rate = 0, frags = 0;
    snd_pcm_t *pcm = openPcm(m_cfg, false, &audioman, &frag, &rate, &frags, &m_error);
    if (!pcm) return;
    actualRate = rate;
    actualFrag = frag;
    actualFrags = frags;
    QFile dump(m_cfg.dumpPrefix + "playback.raw");
    if (!m_cfg.dumpPrefix.isEmpty()) dump.open(QIODevice::WriteOnly | QIODevice::Truncate);

    QByteArray buffer(frag, 0);
    const int jitterBytes = qMax(frag, m_cfg.rate * 2 * m_cfg.jitterMs / 1000);
    const int fragMs = qMax(1, frag * 1000 / (m_cfg.rate * 2));
    bool prebuffering = true;
    quint32 noise = 12345;
    qint64 probeSample = 0;

    while (!m_stop) {
        if (m_source) {
            if (prebuffering) {
                // Build up the jitter buffer before (re)starting playback.
                if (!m_source->waitFor(jitterBytes, 50)) continue;
                prebuffering = false;
            }
            // Latency bound: more than the jitter target + 3 blocks queued
            // means the other side runs fast or arrived in a burst.
            while (m_source->level() > jitterBytes + 3 * frag) {
                m_source->skip(frag);
                dropped.ref();
            }
            if (m_source->waitFor(frag, fragMs)) {
                m_source->read(buffer.data(), frag);
            } else {
                const int got = m_source->read(buffer.data(), frag); // partial block, if any
                memset(buffer.data() + got, 0, frag - got);
                concealed.ref();
                if (m_source->level() == 0 && concealed % 25 == 0) prebuffering = true; // long gap: re-buffer
            }
        } else {
            // Echo probe: 1 s of white noise at -12 dBFS, 1 s of silence.
            qint16 *s = reinterpret_cast<qint16 *>(buffer.data());
            const bool on = ((probeSample / m_cfg.rate) % 2) == 0;
            for (int i = 0; i < frag / 2; ++i) {
                noise = noise * 1103515245u + 12345u;
                s[i] = on ? qint16(int((noise >> 16) & 0x3fff) - 0x2000) : 0;
            }
            probeSample += frag / 2;
            g_callProbeTone = on ? 1 : 0;
        }
        const ssize_t n = snd_pcm_plugin_write(pcm, buffer.constData(), frag);
        if (dump.isOpen() && n > 0) dump.write(buffer.constData(), n);
        if (n < frag) {
            recover(pcm, SND_PCM_CHANNEL_PLAYBACK, &underruns);
            if (n <= 0) continue;
        }
        bytesPlayed.fetchAndAddRelaxed(int(n));
    }
    g_callProbeTone = 0;
    snd_pcm_plugin_flush(pcm, SND_PCM_CHANNEL_PLAYBACK);
    snd_pcm_close(pcm);
    if (audioman) audio_manager_free_handle(audioman);
}
