#include "thumbnailgenerator.h"
#include <algorithm>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

ThumbnailGenerator::ThumbnailGenerator(QObject* parent)
    : QThread(parent)
{
}

ThumbnailGenerator::~ThumbnailGenerator()
{
    stopThread();
}

void ThumbnailGenerator::openFile(const QString& path)
{
    QMutexLocker lk(&m_mutex);
    m_abort.store(true);

    {
        QMutexLocker clk(&m_cacheMutex);
        m_cache.clear();
    }
    m_totalKeyframes.store(0);
    m_generatedCount.store(0);
    m_ready.store(false);

    m_pendingPath = path;
    m_cond.wakeOne();
}

void ThumbnailGenerator::stopThread()
{
    m_running.store(false);
    m_abort.store(true);
    {
        QMutexLocker lk(&m_mutex);
        m_cond.wakeOne();
    }
    if (isRunning()) wait(3000);
}

QImage ThumbnailGenerator::thumbnailAt(double pts) const
{
    QMutexLocker lk(&m_cacheMutex);
    if (m_cache.isEmpty()) return {};

    int64_t key = static_cast<int64_t>(pts * 1000.0);
    auto it = m_cache.lowerBound(key);

    if (it == m_cache.end()) {
        --it;
        return it.value();
    }
    if (it == m_cache.begin()) {
        return it.value();
    }

    auto prev = it - 1;
    if (key - prev.key() <= it.key() - key)
        return prev.value();
    else
        return it.value();
}

// ── Главный цикл ────────────────────────────────────────────────────────────
void ThumbnailGenerator::run()
{
    while (m_running.load()) {
        QString path;
        {
            QMutexLocker lk(&m_mutex);
            while (m_pendingPath.isEmpty() && m_running.load())
                m_cond.wait(&m_mutex, 100);

            if (!m_running.load()) break;
            path = m_pendingPath;
            m_pendingPath.clear();
        }

        if (path.isEmpty()) continue;

        m_abort.store(false);
        generateThumbnails(path);
    }
}

// ── Генерация превью из файла ────────────────────────────────────────────────
void ThumbnailGenerator::generateThumbnails(const QString& path)
{
    QByteArray pathUtf8 = path.toUtf8();

    // ── Открываем файл ───────────────────────────────────────────────────────
    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, pathUtf8.constData(), nullptr, nullptr) < 0)
        return;

    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return;
    }

    int videoIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoIdx < 0) {
        avformat_close_input(&fmtCtx);
        return;
    }

    AVStream* vstream = fmtCtx->streams[videoIdx];
    AVCodecParameters* par = vstream->codecpar;

    // ── SW декодер (не HW — превью не критичны по скорости) ──────────────────
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        avformat_close_input(&fmtCtx);
        return;
    }

    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codecCtx, par);
    codecCtx->thread_count = 2;

    // Декодируем только keyframes — пропускаем не-key пакеты
    codecCtx->skip_frame = AVDISCARD_NONKEY;

    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return;
    }

    // ── Подсчёт keyframes (примерный — по длительности и типичному GOP) ──────
    // Точный подсчёт требует полного прохода — делаем приблизительно
    double duration = (fmtCtx->duration > 0)
                          ? fmtCtx->duration / (double)AV_TIME_BASE : 0.0;
    double fps = 25.0;
    if (vstream->avg_frame_rate.den > 0 && vstream->avg_frame_rate.num > 0)
        fps = av_q2d(vstream->avg_frame_rate);
    int estimatedKF = std::max(1, static_cast<int>(duration * fps / 125.0));
    m_totalKeyframes.store(estimatedKF);
    emit generationStarted(estimatedKF);

    // ── Scaler: исходный размер → 160×90 ─────────────────────────────────────
    // Определяем размер с сохранением пропорций
    int srcW = par->width;
    int srcH = par->height;
    int dstW = THUMB_W;
    int dstH = THUMB_H;

    if (srcW > 0 && srcH > 0) {
        double aspect = static_cast<double>(srcW) / srcH;
        if (aspect > static_cast<double>(THUMB_W) / THUMB_H) {
            dstH = std::max(1, static_cast<int>(THUMB_W / aspect));
        } else {
            dstW = std::max(1, static_cast<int>(THUMB_H * aspect));
        }
    }

    SwsContext* swsCtx = nullptr;  // создадим при первом кадре

    // ── Читаем и декодируем keyframes ────────────────────────────────────────
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int count = 0;

    while (av_read_frame(fmtCtx, pkt) >= 0) {
        if (m_abort.load()) {
            av_packet_unref(pkt);
            break;
        }

        if (pkt->stream_index != videoIdx) {
            av_packet_unref(pkt);
            continue;
        }

        // Отправляем пакет
        int rc = avcodec_send_packet(codecCtx, pkt);
        av_packet_unref(pkt);
        if (rc < 0) continue;

        // Забираем кадры
        while (avcodec_receive_frame(codecCtx, frame) == 0) {
            if (m_abort.load()) break;

            // PTS в секундах
            double pts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                             ? frame->best_effort_timestamp * av_q2d(vstream->time_base)
                             : (frame->pts != AV_NOPTS_VALUE)
                                   ? frame->pts * av_q2d(vstream->time_base)
                                   : 0.0;

            // Создаём scaler при первом кадре (теперь знаем формат)
            if (!swsCtx) {
                swsCtx = sws_getContext(
                    frame->width, frame->height,
                    static_cast<AVPixelFormat>(frame->format),
                    dstW, dstH, AV_PIX_FMT_RGB24,
                    SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (!swsCtx) {
                    av_frame_unref(frame);
                    goto cleanup;
                }
            }

            // Масштабируем в RGB24
            {
                QImage img(dstW, dstH, QImage::Format_RGB888);
                uint8_t* dstData[1] = { img.bits() };
                int dstLinesize[1] = { static_cast<int>(img.bytesPerLine()) };

                sws_scale(swsCtx, frame->data, frame->linesize,
                          0, frame->height, dstData, dstLinesize);

                // Сохраняем в кэш
                int64_t key = static_cast<int64_t>(pts * 1000.0);
                {
                    QMutexLocker clk(&m_cacheMutex);
                    m_cache.insert(key, img.copy());  // глубокая копия
                }

                count++;
                m_generatedCount.store(count);

                if (count % 10 == 0)
                    emit generationProgress(count, m_totalKeyframes.load());
            }

            av_frame_unref(frame);
        }
    }

    // Flush декодера
    avcodec_send_packet(codecCtx, nullptr);
    while (avcodec_receive_frame(codecCtx, frame) == 0) {
        if (m_abort.load()) break;

        double pts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                         ? frame->best_effort_timestamp * av_q2d(vstream->time_base)
                         : 0.0;

        if (swsCtx) {
            QImage img(dstW, dstH, QImage::Format_RGB888);
            uint8_t* dstData[1] = { img.bits() };
            int dstLinesize[1] = { static_cast<int>(img.bytesPerLine()) };

            sws_scale(swsCtx, frame->data, frame->linesize,
                      0, frame->height, dstData, dstLinesize);

            int64_t key = static_cast<int64_t>(pts * 1000.0);
            {
                QMutexLocker clk(&m_cacheMutex);
                m_cache.insert(key, img.copy());
            }
            count++;
            m_generatedCount.store(count);
        }
        av_frame_unref(frame);
    }

cleanup:
    av_frame_free(&frame);
    av_packet_free(&pkt);
    if (swsCtx) sws_freeContext(swsCtx);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&fmtCtx);

    m_totalKeyframes.store(count);
    m_ready.store(!m_abort.load());

    if (!m_abort.load())
        emit generationFinished();
}