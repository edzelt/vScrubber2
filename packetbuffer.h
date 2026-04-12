#pragma once

#include <vector>
#include <cstdint>

// FFmpeg — C API
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

// ── Информация о GOP ────────────────────────────────────────────────────────
struct GOPInfo {
    int      startPacketIdx = 0;     // индекс keyframe пакета в m_packets
    int      endPacketIdx   = 0;     // индекс последнего пакета перед следующим keyframe
    double   startPts       = 0.0;   // PTS keyframe в секундах
    double   endPts         = 0.0;   // PTS последнего кадра в GOP
    int      frameCount     = 0;     // количество кадров в GOP
};

// ── Кэш видео-пакетов в RAM ─────────────────────────────────────────────────
//
// При открытии файла делает один проход av_read_frame и складывает все
// видео-пакеты в вектор. Попутно строит карту GOP (позиции keyframe).
//
// После загрузки:
//   - seek = бинарный поиск по вектору (мгновенно)
//   - чтение = итерация по вектору (из RAM, без disk I/O)
//   - GOP-карта доступна для умного prefetch
//
// Потребление RAM: ~15 МБит/с × 60 сек = ~110 МБ для 1 мин 4K HEVC.
// Для файлов до 2 ГБ — легко помещается в 48 ГБ RAM.
class PacketBuffer
{
public:
    PacketBuffer() = default;
    ~PacketBuffer() { clear(); }

    // Запрет копирования (владеет AVPacket*)
    PacketBuffer(const PacketBuffer&) = delete;
    PacketBuffer& operator=(const PacketBuffer&) = delete;

    // ── Загрузка ─────────────────────────────────────────────────────────────

    // Загрузить все видео-пакеты из файла в RAM.
    // Вызывать после avformat_open_input + avformat_find_stream_info.
    // videoStreamIdx — индекс видеопотока.
    // timeBase — time_base видеопотока (для конвертации PTS в секунды).
    // Возвращает true при успехе.
    bool load(AVFormatContext* fmtCtx, int videoStreamIdx, AVRational timeBase);

    // Освободить все пакеты и очистить структуры
    void clear();

    // ── Доступ к пакетам ─────────────────────────────────────────────────────

    // Получить пакет по индексу. Возвращает nullptr если вне диапазона.
    // Указатель валиден до вызова clear().
    AVPacket* packet(int idx) const;

    // Общее количество видео-пакетов
    int packetCount() const { return static_cast<int>(m_packets.size()); }

    // ── Навигация ────────────────────────────────────────────────────────────

    // Найти индекс пакета с PTS ≥ targetPts. Бинарный поиск.
    // Возвращает -1 если не найден.
    int findPacketByPts(double targetPts) const;

    // Найти индекс keyframe пакета с PTS ≤ targetPts (для seek).
    // Возвращает -1 если не найден.
    int findKeyframeBefore(double targetPts) const;

    // ── GOP-карта ────────────────────────────────────────────────────────────

    // Количество GOP
    int gopCount() const { return static_cast<int>(m_gops.size()); }

    // Получить информацию о GOP по индексу
    const GOPInfo& gop(int gopIdx) const { return m_gops[gopIdx]; }

    // Найти индекс GOP, содержащий данный PTS.
    // Возвращает -1 если не найден.
    int findGopByPts(double pts) const;

    // Найти индекс GOP, содержащий данный индекс пакета.
    int findGopByPacketIdx(int packetIdx) const;

    // ── Информация ───────────────────────────────────────────────────────────
    bool   isLoaded()       const { return m_loaded; }
    size_t totalBytes()     const { return m_totalBytes; }
    int    keyframeCount()  const { return static_cast<int>(m_gops.size()); }
    double maxGopDuration() const { return m_maxGopDuration; }
    int    maxGopFrames()   const { return m_maxGopFrames; }

    // ── Диагностика ──────────────────────────────────────────────────────────
    void validatePackets() const;

    // PTS пакета в секундах (для диагностики и навигации)
    double packetPts(int idx) const;

private:

    std::vector<AVPacket*> m_packets;        // все видео-пакеты
    std::vector<double>    m_packetPts;      // PTS каждого пакета в секундах (кэш)
    std::vector<GOPInfo>   m_gops;           // карта GOP
    AVRational             m_timeBase = {0, 1};

    size_t m_totalBytes      = 0;
    double m_maxGopDuration  = 0.0;
    int    m_maxGopFrames    = 0;
    bool   m_loaded          = false;
};