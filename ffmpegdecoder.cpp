#include "ffmpegdecoder.h"
#include <QDebug>
#include <cmath>

// ── Callback для выбора HW pixel format (NVDEC) ──────────────────────────────
static AVPixelFormat getHWFormat(AVCodecContext* /*ctx*/,
                                 const AVPixelFormat* pixFmts)
{
    // Ищем CUDA формат в списке поддерживаемых
    for (const AVPixelFormat* p = pixFmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_CUDA)
            return AV_PIX_FMT_CUDA;
    }
    qWarning() << "FFmpegDecoder: CUDA формат недоступен, fallback на SW";
    return pixFmts[0];
}

// ─────────────────────────────────────────────────────────────────────────────
FFmpegDecoder::FFmpegDecoder(QObject* parent)
    : QThread(parent)
{}

FFmpegDecoder::~FFmpegDecoder()
{
    stopThread();
}

// ── Команды из main thread (thread-safe) ─────────────────────────────────────

// Вставить команду в очередь с учётом приоритета.
// Высокоприоритетные (Seek, GOP, Next) вытесняют prefetch.
static void enqueueCmd(QList<DecodeCmdData>& queue, DecodeCmdData cmd,
                       std::atomic<bool>& prefetchInterrupt)
{
    int prio = decodeCmdPriority(cmd.cmd);

    // Если пришла высокоприоритетная команда — удаляем все prefetch из очереди
    if (prio > decodeCmdPriority(DecodeCmd::PrefetchGOP)) {
        for (int i = queue.size() - 1; i >= 0; --i) {
            if (queue[i].cmd == DecodeCmd::PrefetchGOP)
                queue.removeAt(i);
        }
        // Прерываем текущий prefetch если он выполняется
        prefetchInterrupt.store(true, std::memory_order_release);
    }

    // Для seek/GOP/sync — заменяем предыдущую однотипную команду (дедупликация)
    if (cmd.cmd == DecodeCmd::SeekAndDecode || cmd.cmd == DecodeCmd::DecodeGOP
        || cmd.cmd == DecodeCmd::SyncPosition) {
        for (int i = 0; i < queue.size(); ++i) {
            if (queue[i].cmd == cmd.cmd) {
                queue[i] = cmd;
                return;
            }
        }
    }

    // Для PrefetchGOP — не дублируем перекрывающиеся диапазоны
    if (cmd.cmd == DecodeCmd::PrefetchGOP) {
        for (const auto& c : std::as_const(queue)) {
            if (c.cmd == DecodeCmd::PrefetchGOP &&
                std::abs(c.targetPts - cmd.targetPts) < 0.001 &&
                std::abs(c.endPts - cmd.endPts) < 0.001)
                return;
        }
    }

    // Вставляем по приоритету (стабильная сортировка — важные впереди)
    int insertPos = 0;
    for (; insertPos < queue.size(); ++insertPos) {
        if (decodeCmdPriority(queue[insertPos].cmd) < prio)
            break;
    }
    queue.insert(insertPos, cmd);
}

void FFmpegDecoder::openFile(const QString& path)
{
    QMutexLocker lk(&m_cmdMutex);
    DecodeCmdData cmd{ DecodeCmd::Open, path, 0.0 };
    enqueueCmd(m_cmdQueue, cmd, m_prefetchInterrupt);
    m_cmdCond.wakeOne();
}

void FFmpegDecoder::seekAndDecode(double pts)
{
    QMutexLocker lk(&m_cmdMutex);
    DecodeCmdData cmd{ DecodeCmd::SeekAndDecode, {}, pts };
    enqueueCmd(m_cmdQueue, cmd, m_prefetchInterrupt);
    m_cmdCond.wakeOne();
}

void FFmpegDecoder::decodeGOP(double pts)
{
    QMutexLocker lk(&m_cmdMutex);
    DecodeCmdData cmd{ DecodeCmd::DecodeGOP, {}, pts };
    enqueueCmd(m_cmdQueue, cmd, m_prefetchInterrupt);
    m_cmdCond.wakeOne();
}

void FFmpegDecoder::decodeNext()
{
    QMutexLocker lk(&m_cmdMutex);
    DecodeCmdData cmd{ DecodeCmd::DecodeNext, {}, 0.0 };
    enqueueCmd(m_cmdQueue, cmd, m_prefetchInterrupt);
    m_cmdCond.wakeOne();
}

void FFmpegDecoder::syncPosition(double pts)
{
    QMutexLocker lk(&m_cmdMutex);
    DecodeCmdData cmd{ DecodeCmd::SyncPosition, {}, pts };
    enqueueCmd(m_cmdQueue, cmd, m_prefetchInterrupt);
    m_cmdCond.wakeOne();
}

void FFmpegDecoder::prefetchGOP(double startPts, double endPts)
{
    QMutexLocker lk(&m_cmdMutex);
    DecodeCmdData cmd{ DecodeCmd::PrefetchGOP, {}, startPts, endPts };
    enqueueCmd(m_cmdQueue, cmd, m_prefetchInterrupt);
    m_cmdCond.wakeOne();
}

void FFmpegDecoder::cancelPrefetch()
{
    QMutexLocker lk(&m_cmdMutex);
    // Удаляем все PrefetchGOP из очереди
    for (int i = m_cmdQueue.size() - 1; i >= 0; --i) {
        if (m_cmdQueue[i].cmd == DecodeCmd::PrefetchGOP)
            m_cmdQueue.removeAt(i);
    }
    // Прерываем текущий выполняющийся prefetch
    m_prefetchInterrupt.store(true, std::memory_order_release);
}

void FFmpegDecoder::stopThread()
{
    m_running = false;
    {
        QMutexLocker lk(&m_cmdMutex);
        m_cmdQueue.clear();
        m_cmdQueue.append({ DecodeCmd::Stop, {}, 0.0 });
        m_prefetchInterrupt.store(true, std::memory_order_release);
        m_cmdCond.wakeOne();
    }
    if (isRunning()) wait(5000);
    doClose();
}

// ── Главный цикл потока ──────────────────────────────────────────────────────
void FFmpegDecoder::run()
{

    while (m_running) {
        DecodeCmdData cmd;
        {
            QMutexLocker lk(&m_cmdMutex);
            while (m_cmdQueue.isEmpty() && m_running)
                m_cmdCond.wait(&m_cmdMutex, 50);

            if (m_cmdQueue.isEmpty()) continue;
            cmd = m_cmdQueue.takeFirst();
        }

        if (!m_running || cmd.cmd == DecodeCmd::Stop) break;
        if (cmd.cmd == DecodeCmd::None) continue;

        switch (cmd.cmd) {
        case DecodeCmd::Open: {
            bool ok = doOpen(cmd.filePath);
            emit fileOpened(ok, ok ? QString() : "Не удалось открыть файл");
            break;
        }
        case DecodeCmd::SeekAndDecode:
            doSeekAndDecode(cmd.targetPts);
            break;
        case DecodeCmd::DecodeGOP:
            doDecodeGOP(cmd.targetPts);
            break;
        case DecodeCmd::DecodeNext:
            doDecodeNext();
            break;
        case DecodeCmd::SyncPosition:
            doSyncPosition(cmd.targetPts);
            break;
        case DecodeCmd::PrefetchGOP:
            // Сбрасываем флаг прерывания перед началом
            m_prefetchInterrupt.store(false, std::memory_order_release);
            doPrefetchGOP(cmd.targetPts, cmd.endPts);
            break;
        default:
            break;
        }
    }

}

// ─────────────────────────────────────────────────────────────────────────────
// ОТКРЫТИЕ ФАЙЛА
// ─────────────────────────────────────────────────────────────────────────────

bool FFmpegDecoder::doOpen(const QString& path)
{
    // Закрываем предыдущий файл если был
    doClose();

    QByteArray pathUtf8 = path.toUtf8();

    // ── Открываем контейнер ──────────────────────────────────────────────────
    if (avformat_open_input(&m_fmtCtx, pathUtf8.constData(), nullptr, nullptr) < 0) {
        qWarning() << "FFmpegDecoder: не удалось открыть" << path;
        return false;
    }

    if (avformat_find_stream_info(m_fmtCtx, nullptr) < 0) {
        qWarning() << "FFmpegDecoder: не удалось найти потоки";
        doClose();
        return false;
    }

    // ── Ищем видеопоток ──────────────────────────────────────────────────────
    m_videoIdx = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_VIDEO,
                                     -1, -1, nullptr, 0);
    if (m_videoIdx < 0) {
        qWarning() << "FFmpegDecoder: видеопоток не найден";
        doClose();
        return false;
    }

    AVStream* vstream = m_fmtCtx->streams[m_videoIdx];
    AVCodecParameters* par = vstream->codecpar;

    m_width    = par->width;
    m_height   = par->height;
    m_duration = (m_fmtCtx->duration > 0)
                     ? m_fmtCtx->duration / (double)AV_TIME_BASE
                     : 0.0;
    m_fullRange = (par->color_range == AVCOL_RANGE_JPEG);

    // FPS из потока
    if (vstream->avg_frame_rate.den > 0 && vstream->avg_frame_rate.num > 0)
        m_fps = av_q2d(vstream->avg_frame_rate);
    else if (vstream->r_frame_rate.den > 0 && vstream->r_frame_rate.num > 0)
        m_fps = av_q2d(vstream->r_frame_rate);
    else
        m_fps = 25.0;

    qDebug() << "FFmpegDecoder:" << m_width << "x" << m_height
             << "@" << m_fps << "fps, duration:" << m_duration << "s"
             << "codec:" << avcodec_get_name(par->codec_id)
             << "range:" << (m_fullRange ? "full" : "limited");

    // ── Инициализируем декодер — сначала HW, потом SW fallback ───────────────
    if (!initHWDecoder()) {
        qDebug() << "FFmpegDecoder: NVDEC недоступен, используем SW декодер";
        if (!initSWDecoder()) {
            qWarning() << "FFmpegDecoder: не удалось создать декодер";
            doClose();
            return false;
        }
    }

    // ── Загружаем все видео-пакеты в RAM ─────────────────────────────────────
    AVStream* vstreamForPB = m_fmtCtx->streams[m_videoIdx];
    if (!m_packetBuffer.load(m_fmtCtx, m_videoIdx, vstreamForPB->time_base)) {
        qWarning() << "FFmpegDecoder: не удалось загрузить пакеты в RAM";
        doClose();
        return false;
    }
    m_readIdx = 0;

    return true;
}

// ── Инициализация HW декодера (NVDEC через CUDA) ─────────────────────────────
bool FFmpegDecoder::initHWDecoder()
{
    AVStream* vstream = m_fmtCtx->streams[m_videoIdx];
    AVCodecParameters* par = vstream->codecpar;

    // Ищем декодер с поддержкой CUDA
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) return false;

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) return false;

    avcodec_parameters_to_context(m_codecCtx, par);

    // Создаём CUDA device
    int rc = av_hwdevice_ctx_create(&m_hwDevCtx, AV_HWDEVICE_TYPE_CUDA,
                                    nullptr, nullptr, 0);
    if (rc < 0) {
        qDebug() << "FFmpegDecoder: CUDA device не создан, rc=" << rc;
        avcodec_free_context(&m_codecCtx);
        return false;
    }

    m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDevCtx);
    m_codecCtx->get_format    = getHWFormat;

    // Многопоточное декодирование
    m_codecCtx->thread_count = 0;   // авто

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        qDebug() << "FFmpegDecoder: не удалось открыть HW декодер";
        avcodec_free_context(&m_codecCtx);
        av_buffer_unref(&m_hwDevCtx);
        return false;
    }

    m_hwAccel = true;
    qDebug() << "FFmpegDecoder: NVDEC активирован";
    return true;
}

// ── Инициализация SW декодера (fallback) ─────────────────────────────────────
bool FFmpegDecoder::initSWDecoder()
{
    AVStream* vstream = m_fmtCtx->streams[m_videoIdx];
    AVCodecParameters* par = vstream->codecpar;

    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) return false;

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) return false;

    avcodec_parameters_to_context(m_codecCtx, par);
    m_codecCtx->thread_count = 0;

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        avcodec_free_context(&m_codecCtx);
        return false;
    }

    m_hwAccel = false;
    qDebug() << "FFmpegDecoder: SW декодер активирован";
    return true;
}

void FFmpegDecoder::doClose()
{
    m_packetBuffer.clear();
    m_readIdx = 0;
    if (m_codecCtx) { avcodec_free_context(&m_codecCtx); }
    if (m_hwDevCtx) { av_buffer_unref(&m_hwDevCtx); }
    if (m_fmtCtx)   { avformat_close_input(&m_fmtCtx); }
    m_nv12UVBuffer.clear();
    m_videoIdx = -1;
    m_hwAccel  = false;
    m_lastDecodedPts    = -1.0;
    m_demuxerContinuous = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// ДЕКОДИРОВАНИЕ
// ─────────────────────────────────────────────────────────────────────────────

// ── Seek + декодировать один кадр (для jog/покадровой навигации) ──────────────
// Ищет кадр с PTS ближайшим к target. Доставляет только его.
bool FFmpegDecoder::doSeekAndDecode(double pts)
{
    if (!m_fmtCtx || !m_codecCtx) return false;

    int keyPktIdx = m_packetBuffer.findKeyframeBefore(pts);
    if (!seekToPacketIdx(keyPktIdx)) return false;
    flushDecoder();

    AVFrame* frame = av_frame_alloc();
    AVFrame* bestFrame = av_frame_alloc();
    if (!frame || !bestFrame) {
        av_frame_free(&frame);
        av_frame_free(&bestFrame);
        return false;
    }

    double bestPts = -1.0;
    double bestDiff = 1e9;
    int framesAfterTarget = 0;

    while (decodeNextPacket(frame)) {
        double fpts = framePts(frame);

        if (fpts >= 0.0) {
            double diff = std::abs(fpts - pts);
            if (diff < bestDiff) {
                bestDiff = diff;
                bestPts = fpts;
                av_frame_unref(bestFrame);
                av_frame_move_ref(bestFrame, frame);
            } else {
                av_frame_unref(frame);
            }

            if (fpts >= pts)
                framesAfterTarget++;

            if (framesAfterTarget >= 3) break;
            if (bestDiff < 0.001) break;
        } else {
            av_frame_unref(frame);
        }
    }

    bool found = false;
    if (bestPts >= 0.0) {
        m_lastDecodedPts = bestPts;
        m_demuxerContinuous = true;
        deliverFrame(bestFrame);
        emit seekComplete(bestPts);
        found = true;
    }

    av_frame_free(&frame);
    av_frame_free(&bestFrame);
    return found;
}

// ── Декодировать GOP: от keyframe до targetPts, доставляя ВСЕ кадры ─────────
// Ключевая операция для быстрого реверса: один seek декодирует ~30-90
// кадров, и все попадают в GPU-кэш.
bool FFmpegDecoder::doDecodeGOP(double targetPts)
{
    if (!m_fmtCtx || !m_codecCtx) return false;

    int keyPktIdx = m_packetBuffer.findKeyframeBefore(targetPts);
    if (!seekToPacketIdx(keyPktIdx)) return false;
    flushDecoder();

    AVFrame* frame = av_frame_alloc();
    if (!frame) return false;

    int count = 0;
    double startPts = -1.0;
    double endPts   = -1.0;

    while (decodeNextPacket(frame)) {
        double fpts = framePts(frame);

        if (startPts < 0.0) startPts = fpts;
        endPts = fpts;

        m_lastDecodedPts = fpts;
        deliverFrame(frame);
        count++;
        av_frame_unref(frame);

        if (fpts >= targetPts - 0.5 / m_fps)
            break;

        if (count > 300) break;
    }

    av_frame_free(&frame);
    m_demuxerContinuous = (count > 0);
    emit gopDecoded(startPts, endPts, count);
    return count > 0;
}

// ── Проверка наличия высокоприоритетных команд в очереди ─────────────────────
bool FFmpegDecoder::hasPendingHighPriority()
{
    QMutexLocker lk(&m_cmdMutex);
    for (const auto& c : std::as_const(m_cmdQueue)) {
        if (decodeCmdPriority(c.cmd) > decodeCmdPriority(DecodeCmd::PrefetchGOP))
            return true;
    }
    return false;
}

// ── Упреждающее декодирование диапазона (прерываемое) ─────────────────────────
// Seek на keyframe ≤ startPts, декодируем все кадры до endPts.
// Кадры, уже имеющиеся в кэше, декодируются (неизбежно — нужны как reference),
// но deliverFrame для них пропускается — экономим конвертацию RGBA и upload.
// ВАЖНО: prefetch НЕ обновляет m_lastDecodedPts и m_demuxerContinuous —
// эти поля используются для отслеживания позиции пользовательских команд.
// Прерывается при поступлении высокоприоритетной команды.
bool FFmpegDecoder::doPrefetchGOP(double startPts, double endPts)
{
    if (!m_fmtCtx || !m_codecCtx) return false;

    // Проверка прерывания перед началом seek
    if (m_prefetchInterrupt.load(std::memory_order_acquire)) {
        return false;
    }

    // Сохраняем позицию demuxer — восстановим после prefetch
    double savedLastPts = m_lastDecodedPts;

    // Нормализуем диапазон
    if (startPts > endPts) std::swap(startPts, endPts);

    if (!seekToPacketIdx(m_packetBuffer.findKeyframeBefore(startPts))) return false;
    flushDecoder();

    AVFrame* frame = av_frame_alloc();
    if (!frame) return false;

    int delivered = 0;   // кадры, отправленные в кэш (новые)
    int skipped   = 0;   // кадры, пропущенные (уже в кэше)
    double actualStart = -1.0;
    double actualEnd   = -1.0;

    while (decodeNextPacket(frame)) {
        // Проверяем прерывание после каждого декодированного кадра
        if (m_prefetchInterrupt.load(std::memory_order_acquire)) {
            av_frame_unref(frame);
            break;
        }

        double fpts = framePts(frame);
        int64_t fIdx = static_cast<int64_t>(std::round(fpts * m_fps));

        if (actualStart < 0.0) actualStart = fpts;
        actualEnd = fpts;

        // Пропускаем deliverFrame если кадр уже в кэше
        bool inCache = m_cacheCheck && m_cacheCheck(fIdx);
        if (!inCache) {
            deliverFrame(frame, true);  // isPrefetch = true
            delivered++;
        } else {
            skipped++;
        }

        av_frame_unref(frame);

        // Достигли конца запрошенного диапазона
        if (fpts >= endPts - 0.5 / m_fps)
            break;

        // Защита от бесконечного цикла (макс. 600 кадров — с учётом skip)
        if (delivered + skipped > 600) break;
    }

    av_frame_free(&frame);

    // Восстанавливаем состояние demuxer — prefetch не влияет на позицию
    // пользовательских команд. demuxer сейчас в невалидной позиции,
    // следующая пользовательская команда сделает свой seek + flush.
    m_lastDecodedPts    = savedLastPts;
    m_demuxerContinuous = false;  // всегда false — demuxer сбит prefetch-ом

    // Flush декодера после prefetch — чтобы следующая команда
    // не получила «хвосты» от prefetch reference frames
    flushDecoder();

    if (delivered > 0) {
        emit prefetchGopDecoded(actualStart, actualEnd, delivered);
    }

    return delivered > 0;
}

// ── Ресинхрон декодера на PTS без доставки кадров ─────────────────────────────
// Seek на keyframe, decode до target — но НЕ вызывает deliverFrame.
// Только ставит m_readIdx и m_demuxerContinuous = true.
// Используется для старта continuous play с произвольной позиции.
// Значительно быстрее seekAndDecode — не тратит время на HW transfer и копирование.
bool FFmpegDecoder::doSyncPosition(double targetPts)
{
    if (!m_fmtCtx || !m_codecCtx) return false;

    int keyPktIdx = m_packetBuffer.findKeyframeBefore(targetPts);
    if (!seekToPacketIdx(keyPktIdx)) return false;
    flushDecoder();

    AVFrame* frame = av_frame_alloc();
    if (!frame) return false;

    double bestPts = -1.0;
    int framesAfterTarget = 0;

    while (decodeNextPacket(frame)) {
        double fpts = framePts(frame);
        av_frame_unref(frame);

        if (fpts >= 0.0) {
            bestPts = fpts;

            if (fpts >= targetPts)
                framesAfterTarget++;

            // Точное попадание или 3 кадра после target — reorder buffer исчерпан
            if (std::abs(fpts - targetPts) < 0.001 || framesAfterTarget >= 3)
                break;
        }
    }

    av_frame_free(&frame);

    if (bestPts >= 0.0) {
        m_lastDecodedPts = bestPts;
        m_demuxerContinuous = true;
        emit syncReady(bestPts);
        return true;
    }

    m_demuxerContinuous = false;
    return false;
}

// ── Декодировать один следующий кадр без seek (быстро!) ───────────────────────
// Работает только если demuxer в непрерывной позиции после предыдущего decode.
// При ошибке — fallback на seek+decode.
bool FFmpegDecoder::doDecodeNext()
{
    if (!m_fmtCtx || !m_codecCtx) return false;

    // Если demuxer не в непрерывной позиции — нельзя просто читать дальше
    if (!m_demuxerContinuous) {
        double targetPts = m_lastDecodedPts + 1.0 / m_fps;
        bool ok = doSeekAndDecode(targetPts);
        if (!ok) {
            emit endOfStream();
        }
        return ok;
    }

    AVFrame* frame = av_frame_alloc();
    if (!frame) return false;

    bool ok = false;
    if (decodeNextPacket(frame)) {
        double fpts = framePts(frame);
        if (fpts >= 0.0) {
            m_lastDecodedPts = fpts;
            deliverFrame(frame);
            emit nextDecoded(fpts);
            ok = true;
        }
    }

    if (!ok) {
        m_demuxerContinuous = false;
        emit endOfStream();
    }

    av_frame_free(&frame);
    return ok;
}

// ── Декодирование следующего пакета из RAM ───────────────────────────────────
// Читает пакеты из PacketBuffer (RAM) вместо av_read_frame (диск).
// ВАЖНО: корректно обрабатываем EAGAIN от avcodec_send_packet —
// при EAGAIN пакет НЕ принят, нужно сначала receive_frame, потом повторить send.
bool FFmpegDecoder::decodeNextPacket(AVFrame* frame)
{
    while (true) {
        // Сначала пытаемся забрать готовый кадр из декодера
        int rc = avcodec_receive_frame(m_codecCtx, frame);
        if (rc == 0) {
            return true;  // кадр готов
        }
        if (rc != AVERROR(EAGAIN)) {
            // Ошибка или конец потока
            return false;
        }

        // EAGAIN — декодеру нужно больше данных, отправляем следующий пакет
        if (m_readIdx >= m_packetBuffer.packetCount()) {
            // Конец буфера — flush оставшихся кадров из декодера
            avcodec_send_packet(m_codecCtx, nullptr);
            // Пробуем забрать последние кадры
            rc = avcodec_receive_frame(m_codecCtx, frame);
            return (rc == 0);
        }

        AVPacket* pkt = m_packetBuffer.packet(m_readIdx);
        if (!pkt) {
            m_readIdx++;
            continue;
        }

        rc = avcodec_send_packet(m_codecCtx, pkt);
        if (rc == 0) {
            // Пакет принят — двигаем индекс
            m_readIdx++;
        } else if (rc == AVERROR(EAGAIN)) {
            // Декодер переполнен — нужно забрать кадр, НЕ двигаем readIdx!
            // На следующей итерации цикла receive_frame заберёт кадр,
            // потом повторим send этого же пакета
            rc = avcodec_receive_frame(m_codecCtx, frame);
            if (rc == 0) {
                return true;  // кадр готов, пакет отправим в следующем вызове
            }
            // Если и тут EAGAIN — что-то совсем не так, пропускаем
            m_readIdx++;
        } else {
            // Реальная ошибка отправки — пропускаем пакет
            m_readIdx++;
        }
    }
}

// ── Установка позиции чтения в PacketBuffer ─────────────────────────────────
// Мгновенно — просто индекс в массиве RAM, без disk I/O.
bool FFmpegDecoder::seekToPacketIdx(int packetIdx)
{
    if (packetIdx < 0 || packetIdx >= m_packetBuffer.packetCount()) {
        qWarning() << "FFmpegDecoder: seekToPacketIdx вне диапазона:" << packetIdx;
        m_demuxerContinuous = false;
        return false;
    }
    m_readIdx = packetIdx;
    m_demuxerContinuous = false;
    return true;
}

// ── Сброс буферов декодера после seek ────────────────────────────────────────
void FFmpegDecoder::flushDecoder()
{
    if (m_codecCtx)
        avcodec_flush_buffers(m_codecCtx);
}

// ── PTS кадра в секундах ─────────────────────────────────────────────────────
double FFmpegDecoder::framePts(const AVFrame* frame) const
{
    AVStream* vstream = m_fmtCtx->streams[m_videoIdx];
    int64_t pts = frame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) pts = frame->pts;
    if (pts == AV_NOPTS_VALUE) return -1.0;
    return pts * av_q2d(vstream->time_base);
}

// ── Доставка кадра в callback (NV12) ─────────────────────────────────────────
void FFmpegDecoder::deliverFrame(AVFrame* frame, bool isPrefetch)
{
    if (!m_frameCallback) return;

    AVFrame* swFrame = nullptr;

    // Если HW кадр — скачиваем с GPU в системную память (NV12)
    if (frame->format == AV_PIX_FMT_CUDA) {
        swFrame = av_frame_alloc();
        if (!transferHWFrame(frame, swFrame)) {
            av_frame_free(&swFrame);
            return;
        }
        frame = swFrame;   // дальше работаем с SW копией (NV12)
    }

    // Определяем формат — NV12 или yuv420p
    AVPixelFormat fmt = static_cast<AVPixelFormat>(frame->format);

    DecodedFrame df;
    df.width   = m_width;
    df.height  = m_height;
    df.pts     = framePts(frame);
    df.frameIdx = static_cast<int64_t>(std::round(df.pts * m_fps));

    if (fmt == AV_PIX_FMT_NV12) {
        // NV12: Y в data[0], чередующийся UV в data[1] — передаём как есть
        df.data[0]     = frame->data[0];
        df.linesize[0] = frame->linesize[0];
        df.data[1]     = frame->data[1];
        df.linesize[1] = frame->linesize[1];
        df.format      = AV_PIX_FMT_NV12;
    } else if (fmt == AV_PIX_FMT_YUV420P) {
        // yuv420p: U и V в отдельных плоскостях — конвертируем в NV12
        convertToNV12(frame);
        df.data[0]     = frame->data[0];          // Y как есть
        df.linesize[0] = frame->linesize[0];
        df.data[1]     = m_nv12UVBuffer.data();    // чередующийся UV
        df.linesize[1] = m_width;                  // stride UV = width (2 байта на пару)
        df.format      = AV_PIX_FMT_NV12;
    } else {
        qWarning() << "FFmpegDecoder: неподдерживаемый формат" << fmt;
        if (swFrame) av_frame_free(&swFrame);
        return;
    }

    df.isPrefetch = isPrefetch;
    m_frameCallback(df);

    if (swFrame) av_frame_free(&swFrame);
}

// ── Скачивание HW кадра с GPU ────────────────────────────────────────────────
bool FFmpegDecoder::transferHWFrame(AVFrame* hwFrame, AVFrame* swFrame)
{
    // av_hwframe_transfer_data копирует данные с GPU (CUDA) в CPU (NV12)
    int rc = av_hwframe_transfer_data(swFrame, hwFrame, 0);
    if (rc < 0) {
        qWarning() << "FFmpegDecoder: HW transfer failed, rc=" << rc;
        return false;
    }
    swFrame->pts                  = hwFrame->pts;
    swFrame->best_effort_timestamp = hwFrame->best_effort_timestamp;
    return true;
}

// ── Конвертация yuv420p → NV12 (чередование U и V) ──────────────────────────
// yuv420p: data[0]=Y, data[1]=U, data[2]=V (отдельные плоскости)
// NV12:    data[0]=Y, data[1]=UVUVUV... (чередующийся)
// Конвертация дешёвая: просто чередуем байты U и V.
void FFmpegDecoder::convertToNV12(AVFrame* srcFrame)
{
    int uvW = m_width / 2;
    int uvH = m_height / 2;
    size_t uvSize = uvW * uvH * 2;   // 2 байта на пиксель (U + V)

    if (m_nv12UVBuffer.size() < uvSize)
        m_nv12UVBuffer.resize(uvSize);

    const uint8_t* uPlane = srcFrame->data[1];
    const uint8_t* vPlane = srcFrame->data[2];
    int uStride = srcFrame->linesize[1];
    int vStride = srcFrame->linesize[2];

    uint8_t* dst = m_nv12UVBuffer.data();

    for (int y = 0; y < uvH; ++y) {
        const uint8_t* uRow = uPlane + y * uStride;
        const uint8_t* vRow = vPlane + y * vStride;
        for (int x = 0; x < uvW; ++x) {
            *dst++ = uRow[x];
            *dst++ = vRow[x];
        }
    }
}