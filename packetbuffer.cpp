#include "packetbuffer.h"
#include <QDebug>
#include <algorithm>
#include <cmath>

// ── Загрузка всех видео-пакетов в RAM ───────────────────────────────────────
bool PacketBuffer::load(AVFormatContext* fmtCtx, int videoStreamIdx,
                        AVRational timeBase)
{
    clear();
    m_timeBase = timeBase;

    // Перематываем на начало файла
    av_seek_frame(fmtCtx, videoStreamIdx, 0, AVSEEK_FLAG_BACKWARD);

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return false;

    int currentGopStart = -1;

    while (av_read_frame(fmtCtx, pkt) >= 0) {
        // Пропускаем не-видео пакеты
        if (pkt->stream_index != videoStreamIdx) {
            av_packet_unref(pkt);
            continue;
        }

        // Клонируем пакет — владеем данными
        AVPacket* clone = av_packet_clone(pkt);
        av_packet_unref(pkt);

        if (!clone) continue;

        int idx = static_cast<int>(m_packets.size());

        // PTS в секундах
        double pts = (clone->pts != AV_NOPTS_VALUE)
                         ? clone->pts * av_q2d(m_timeBase)
                         : (clone->dts != AV_NOPTS_VALUE)
                               ? clone->dts * av_q2d(m_timeBase)
                               : 0.0;

        m_packets.push_back(clone);
        m_packetPts.push_back(pts);
        m_totalBytes += clone->size;

        // Определяем keyframe — начало нового GOP
        if (clone->flags & AV_PKT_FLAG_KEY) {
            // Закрываем предыдущий GOP
            if (currentGopStart >= 0 && !m_gops.empty()) {
                GOPInfo& prev = m_gops.back();
                prev.endPacketIdx = idx - 1;
                prev.endPts       = m_packetPts[idx - 1];
                prev.frameCount   = prev.endPacketIdx - prev.startPacketIdx + 1;

                double gopDur = prev.endPts - prev.startPts;
                if (gopDur > m_maxGopDuration) m_maxGopDuration = gopDur;
                if (prev.frameCount > m_maxGopFrames) m_maxGopFrames = prev.frameCount;
            }

            // Начинаем новый GOP
            GOPInfo gop;
            gop.startPacketIdx = idx;
            gop.startPts       = pts;
            m_gops.push_back(gop);
            currentGopStart = idx;
        }
    }

    av_packet_free(&pkt);

    // Закрываем последний GOP
    if (!m_gops.empty() && !m_packets.empty()) {
        GOPInfo& last = m_gops.back();
        last.endPacketIdx = static_cast<int>(m_packets.size()) - 1;
        last.endPts       = m_packetPts.back();
        last.frameCount   = last.endPacketIdx - last.startPacketIdx + 1;

        double gopDur = last.endPts - last.startPts;
        if (gopDur > m_maxGopDuration) m_maxGopDuration = gopDur;
        if (last.frameCount > m_maxGopFrames) m_maxGopFrames = last.frameCount;
    }

    // Перематываем обратно на начало для дальнейшего использования
    av_seek_frame(fmtCtx, videoStreamIdx, 0, AVSEEK_FLAG_BACKWARD);

    m_loaded = true;

    qDebug() << "PacketBuffer: загружено" << m_packets.size() << "пакетов,"
             << m_gops.size() << "GOP,"
             << m_totalBytes / (1024 * 1024) << "МБ RAM,"
             << "макс. GOP:" << m_maxGopFrames << "кадров /"
             << QString::number(m_maxGopDuration, 'f', 2) << "сек";

    // ── Диагностика: проверяем целостность PTS ──────────────────────────────
    validatePackets();

    return true;
}

void PacketBuffer::clear()
{
    for (AVPacket* pkt : m_packets)
        av_packet_free(&pkt);
    m_packets.clear();
    m_packetPts.clear();
    m_gops.clear();
    m_totalBytes = 0;
    m_maxGopDuration = 0.0;
    m_maxGopFrames = 0;
    m_loaded = false;
}

// ── Доступ к пакетам ─────────────────────────────────────────────────────────

AVPacket* PacketBuffer::packet(int idx) const
{
    if (idx < 0 || idx >= static_cast<int>(m_packets.size()))
        return nullptr;
    return m_packets[idx];
}

double PacketBuffer::packetPts(int idx) const
{
    if (idx < 0 || idx >= static_cast<int>(m_packetPts.size()))
        return -1.0;
    return m_packetPts[idx];
}

// ── Навигация: бинарный поиск по PTS ─────────────────────────────────────────

int PacketBuffer::findPacketByPts(double targetPts) const
{
    if (m_packetPts.empty()) return -1;

    // lower_bound: первый элемент ≥ targetPts
    auto it = std::lower_bound(m_packetPts.begin(), m_packetPts.end(), targetPts);
    if (it == m_packetPts.end()) return static_cast<int>(m_packetPts.size()) - 1;
    return static_cast<int>(it - m_packetPts.begin());
}

int PacketBuffer::findKeyframeBefore(double targetPts) const
{
    if (m_gops.empty()) return -1;

    // Бинарный поиск по GOP: ищем последний GOP с startPts ≤ targetPts
    int lo = 0, hi = static_cast<int>(m_gops.size()) - 1;
    int result = 0;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (m_gops[mid].startPts <= targetPts) {
            result = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    return m_gops[result].startPacketIdx;
}

// ── GOP-карта ────────────────────────────────────────────────────────────────

int PacketBuffer::findGopByPts(double pts) const
{
    if (m_gops.empty()) return -1;

    // Бинарный поиск: последний GOP с startPts ≤ pts
    int lo = 0, hi = static_cast<int>(m_gops.size()) - 1;
    int result = 0;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (m_gops[mid].startPts <= pts) {
            result = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    return result;
}

int PacketBuffer::findGopByPacketIdx(int packetIdx) const
{
    if (m_gops.empty()) return -1;

    for (int i = static_cast<int>(m_gops.size()) - 1; i >= 0; --i) {
        if (m_gops[i].startPacketIdx <= packetIdx)
            return i;
    }
    return 0;
}

// ── Диагностика: проверка целостности загруженных пакетов ─────────────────────
void PacketBuffer::validatePackets() const
{
    if (m_packetPts.size() < 2) return;

    // Считаем ожидаемый интервал из первых кадров
    // Сортируем PTS чтобы найти медианный интервал (пакеты могут быть в decode order)
    std::vector<double> sortedPts = m_packetPts;
    std::sort(sortedPts.begin(), sortedPts.end());

    // Медианный интервал из первых 100 кадров
    std::vector<double> intervals;
    int sampleSize = std::min(100, static_cast<int>(sortedPts.size()) - 1);
    for (int i = 0; i < sampleSize; ++i) {
        double dt = sortedPts[i + 1] - sortedPts[i];
        if (dt > 0.0001)  // игнорируем нулевые/дублированные
            intervals.push_back(dt);
    }

    if (intervals.empty()) {
        qWarning() << "PacketBuffer ДИАГНОСТИКА: не удалось определить интервал кадров";
        return;
    }

    std::sort(intervals.begin(), intervals.end());
    double medianInterval = intervals[intervals.size() / 2];
    double gapThreshold = medianInterval * 2.5;  // разрыв = больше 2.5x от нормы

    qDebug() << "PacketBuffer ДИАГНОСТИКА: медианный интервал PTS ="
             << QString::number(medianInterval * 1000.0, 'f', 2) << "мс"
             << "(~" << QString::number(1.0 / medianInterval, 'f', 1) << "fps)";

    // Проверяем разрывы в отсортированных PTS
    int gapCount = 0;
    int duplicateCount = 0;
    double prevPts = sortedPts[0];

    for (size_t i = 1; i < sortedPts.size(); ++i) {
        double dt = sortedPts[i] - prevPts;

        if (dt < 0.0001 && dt >= 0.0) {
            duplicateCount++;
            if (duplicateCount <= 5) {
                qDebug() << "  ДУБЛИКАТ PTS:" << QString::number(sortedPts[i], 'f', 4)
                         << "с (пакеты" << i-1 << "и" << i << ")";
            }
        }
        else if (dt > gapThreshold) {
            gapCount++;
            double missingFrames = dt / medianInterval - 1.0;
            if (gapCount <= 20) {
                qWarning() << "  РАЗРЫВ PTS:" << QString::number(prevPts, 'f', 4)
                           << "->" << QString::number(sortedPts[i], 'f', 4)
                           << "с (Δ" << QString::number(dt * 1000.0, 'f', 1)
                           << "мс, ~" << static_cast<int>(missingFrames)
                           << "пропущенных кадров)";
            }
        }
        prevPts = sortedPts[i];
    }

    // Также проверяем порядок пакетов (decode order vs presentation order)
    int outOfOrderCount = 0;
    for (size_t i = 1; i < m_packetPts.size(); ++i) {
        if (m_packetPts[i] < m_packetPts[i-1] - 0.0001)
            outOfOrderCount++;
    }

    qDebug() << "PacketBuffer ДИАГНОСТИКА итого:" << m_packets.size() << "пакетов,"
             << duplicateCount << "дубликатов PTS,"
             << gapCount << "разрывов,"
             << outOfOrderCount << "пакетов не в порядке PTS (B-frames)";

    // Проверяем GOP-карту
    for (size_t i = 0; i < m_gops.size(); ++i) {
        const GOPInfo& g = m_gops[i];
        if (g.frameCount <= 0) {
            qWarning() << "  GOP" << i << ": пустой! startPkt=" << g.startPacketIdx
                       << "endPkt=" << g.endPacketIdx;
        }
        if (i > 0 && g.startPacketIdx != m_gops[i-1].endPacketIdx + 1) {
            qWarning() << "  GOP" << i << ": разрыв индексов пакетов!"
                       << "пред. endPkt=" << m_gops[i-1].endPacketIdx
                       << "текущ. startPkt=" << g.startPacketIdx;
        }
    }
}