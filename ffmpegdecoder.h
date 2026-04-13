#pragma once

#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <atomic>
#include <functional>
#include <vector>

#include "packetbuffer.h"

// FFmpeg — C API, нужен extern "C"
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
}

// ── Декодированный кадр ──────────────────────────────────────────────────────
// Содержит данные кадра в NV12: data[0] = Y плоскость, data[1] = UV плоскость.
// Время жизни данных — до следующего вызова releaseFunc.
struct DecodedFrame {
    const uint8_t* data[4] = {};   // [0]=Y, [1]=UV (NV12)
    int            linesize[4] = {};// [0]=stride Y, [1]=stride UV
    int            width    = 0;
    int            height   = 0;
    int            format   = 0;   // AVPixelFormat (AV_PIX_FMT_NV12)
    double         pts      = 0.0; // время кадра в секундах
    int64_t        frameIdx = 0;   // номер кадра (pts * fps)
    bool           isPrefetch = false; // true = кадр из фонового prefetch
};

// ── Команда для decode потока ─────────────────────────────────────────────────
enum class DecodeCmd {
    None,
    Open,          // открыть файл
    SeekAndDecode, // seek на PTS + декодировать кадр
    DecodeGOP,     // декодировать весь GOP от keyframe до targetPts
    DecodeNext,    // декодировать следующий кадр без seek (быстро)
    SyncPosition,  // seek + decode до target БЕЗ deliverFrame (быстрый ресинхрон)
    PrefetchGOP,   // фоновое упреждающее декодирование GOP (низкий приоритет)
    Stop
};

// ── Приоритет команды: чем выше — тем важнее ────────────────────────────────
inline int decodeCmdPriority(DecodeCmd cmd) {
    switch (cmd) {
    case DecodeCmd::Stop:          return 100;
    case DecodeCmd::Open:          return 90;
    case DecodeCmd::SyncPosition:  return 85;  // выше DecodeNext — sync должен завершиться первым
    case DecodeCmd::DecodeNext:    return 80;
    case DecodeCmd::SeekAndDecode: return 70;
    case DecodeCmd::DecodeGOP:     return 60;
    case DecodeCmd::PrefetchGOP:   return 10;  // самый низкий — прерывается
    default:                       return 0;
    }
}

struct DecodeCmdData {
    DecodeCmd   cmd       = DecodeCmd::None;
    QString     filePath;             // для Open
    double      targetPts = 0.0;      // для Seek/DecodeGOP — целевой PTS
    double      endPts    = 0.0;      // для PrefetchGOP — конец диапазона
};

// ── Callback для получения декодированных кадров ──────────────────────────────
// Вызывается из decode потока для каждого декодированного кадра.
// При DecodeGOP вызывается для ВСЕХ кадров в GOP (для заполнения кэша).
using FrameCallback = std::function<void(const DecodedFrame& frame)>;

// ── Декодер на FFmpeg + NVDEC с пакетами в RAM ───────────────────────────────
//
// Работает в отдельном потоке. При открытии файла загружает все видео-пакеты
// в RAM (PacketBuffer) и строит GOP-карту. Seek = бинарный поиск (мгновенно).
//
// Основные операции:
//   open(path)         — открыть, загрузить пакеты в RAM, инициализировать NVDEC
//   decodeGOP(pts)     — seek на keyframe + декодировать все кадры до pts
//   decodeNext()       — следующий кадр без seek (быстро)
//   prefetchGOP(s,e)   — фоновое декодирование диапазона (прерываемое)
//
// Формат кадров: NV12 (Y + UV), конвертация в RGB — в шейдере VideoWidget.
class FFmpegDecoder : public QThread
{
    Q_OBJECT

public:
    explicit FFmpegDecoder(QObject* parent = nullptr);
    ~FFmpegDecoder() override;

    // ── Управление (thread-safe, можно вызывать из main thread) ──────────────
    void openFile(const QString& path);
    void seekAndDecode(double pts);
    void decodeGOP(double pts);
    void decodeNext();             // следующий кадр без seek
    void syncPosition(double pts);  // ресинхрон декодера на PTS без доставки кадров
    void prefetchGOP(double startPts, double endPts); // фоновый prefetch диапазона
    void cancelPrefetch();         // отменить все ожидающие prefetch-команды
    void stopThread();

    // ── Callback для кадров (установить ДО запуска потока) ────────────────────
    void setFrameCallback(FrameCallback cb) { m_frameCallback = std::move(cb); }

    // ── Callback для проверки кэша (опционально) ─────────────────────────────
    // Если установлен, prefetch пропускает deliverFrame для кадров,
    // которые уже есть в кэше. Вызывается из decode потока.
    using CacheCheckFn = std::function<bool(int64_t frameIdx)>;
    void setCacheCheck(CacheCheckFn fn) { m_cacheCheck = std::move(fn); }

    // ── Свойства файла (валидны после сигнала fileOpened) ─────────────────────
    double duration()  const { return m_duration; }
    double fps()       const { return m_fps; }
    double lastFramePts() const { return m_packetBuffer.lastFramePts(); }
    int    videoWidth()  const { return m_width; }
    int    videoHeight() const { return m_height; }
    bool   isHWAccel() const { return m_hwAccel; }
    bool   isFullRange() const { return m_fullRange; }  // true = 0-255, false = 16-235

    // ── GOP карта (валидна после fileOpened) ──────────────────────────────────
    const PacketBuffer& packetBuffer() const { return m_packetBuffer; }
    int    gopCount()    const { return m_packetBuffer.gopCount(); }
    int    maxGopFrames() const { return m_packetBuffer.maxGopFrames(); }
    int    findGopByPts(double pts) const { return m_packetBuffer.findGopByPts(pts); }
    const GOPInfo& gop(int idx) const { return m_packetBuffer.gop(idx); }

signals:
    void fileOpened(bool success, const QString& error);
    void seekComplete(double pts);        // кадр декодирован и отправлен в callback
    void syncReady(double pts);           // ресинхрон завершён, декодер готов к decodeNext
    void gopDecoded(double startPts, double endPts, int frameCount);
    void prefetchGopDecoded(double startPts, double endPts, int frameCount);
    void nextDecoded(double pts);         // один кадр декодирован (decodeNext)
    void endOfStream();                   // конец потока — больше нет кадров
    void decoderError(const QString& msg);

protected:
    void run() override;

private:
    // ── Инициализация ────────────────────────────────────────────────────────
    bool doOpen(const QString& path);
    void doClose();
    bool initHWDecoder();
    bool initSWDecoder();

    // ── Декодирование ────────────────────────────────────────────────────────
    bool doSeekAndDecode(double pts);
    bool doDecodeGOP(double targetPts);
    bool doSyncPosition(double targetPts);     // seek + decode до target без deliverFrame
    bool doPrefetchGOP(double startPts, double endPts); // прерываемый prefetch диапазона
    bool doDecodeNext();               // следующий кадр без seek

    // ── Внутренние helpers ────────────────────────────────────────────────────
    bool decodeNextPacket(AVFrame* frame);       // декодировать следующий пакет из RAM
    bool seekToPacketIdx(int packetIdx);         // установить позицию чтения
    double framePts(const AVFrame* frame) const; // PTS кадра в секундах
    void deliverFrame(AVFrame* frame, bool isPrefetch = false);
    void flushDecoder();                        // сбросить буферы декодера после seek
    bool hasPendingHighPriority();              // есть ли команды важнее prefetch

    // ── Конвертация формата ──────────────────────────────────────────────────
    // Для HW кадров: скачиваем с GPU → NV12 (CPU), передаём как есть
    // Для SW кадров: yuv420p → NV12 (простое чередование U и V)
    bool transferHWFrame(AVFrame* hwFrame, AVFrame* swFrame);
    void convertToNV12(AVFrame* srcFrame);   // yuv420p → NV12 (в m_nv12UVBuffer)

    // ── Очередь команд (с приоритетами) ─────────────────────────────────────
    QMutex         m_cmdMutex;
    QWaitCondition m_cmdCond;
    QList<DecodeCmdData> m_cmdQueue;        // очередь: [0] = следующая на выполнение
    std::atomic<bool> m_running{true};
    std::atomic<bool> m_prefetchInterrupt{false}; // прерывание текущего prefetch

    // ── FFmpeg контекст ──────────────────────────────────────────────────────
    AVFormatContext*  m_fmtCtx    = nullptr;
    AVCodecContext*   m_codecCtx  = nullptr;
    AVBufferRef*      m_hwDevCtx  = nullptr;   // CUDA device для NVDEC
    int               m_videoIdx  = -1;        // индекс видеопотока

    // ── Кэш пакетов в RAM ───────────────────────────────────────────────────
    PacketBuffer      m_packetBuffer;
    int               m_readIdx = 0;           // текущая позиция чтения в m_packetBuffer

    // ── Буфер для SW fallback (yuv420p → NV12) ─────────────────────────────
    std::vector<uint8_t> m_nv12UVBuffer;   // чередующийся UV для SW декодера

    // ── Свойства файла ───────────────────────────────────────────────────────
    double m_duration = 0.0;
    double m_fps      = 0.0;
    int    m_width    = 0;
    int    m_height   = 0;
    bool   m_hwAccel  = false;
    bool   m_fullRange = false;   // color_range: full (0-255) или limited (16-235)

    // ── Позиция demuxer'а (для decodeNext) ───────────────────────────────────
    double m_lastDecodedPts  = -1.0;   // PTS последнего декодированного кадра
    bool   m_demuxerContinuous = false; // true если можно читать следующий без seek

    // ── Callbacks ─────────────────────────────────────────────────────────────
    FrameCallback m_frameCallback;
    CacheCheckFn  m_cacheCheck;     // проверка «кадр уже в кэше?»
};