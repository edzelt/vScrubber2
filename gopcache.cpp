#include "gopcache.h"
#include "ffmpegdecoder.h"   // DecodedFrame
#include <QDebug>
#include <cstring>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// ИНИЦИАЛИЗАЦИЯ GL
// ─────────────────────────────────────────────────────────────────────────────

void GopCache::initGL()
{
    initializeOpenGLFunctions();
    m_glReady = true;
}

void GopCache::destroyGL()
{
    if (!m_glReady) return;

    if (m_dispTexY)  { glDeleteTextures(1, &m_dispTexY);  m_dispTexY  = 0; }
    if (m_dispTexUV) { glDeleteTextures(1, &m_dispTexUV); m_dispTexUV = 0; }

    for (int i = 0; i < PBO_COUNT; ++i) {
        if (m_pbo[i]) { glDeleteBuffers(1, &m_pbo[i]); m_pbo[i] = 0; }
    }
    if (m_pboFence) { glDeleteSync(m_pboFence); m_pboFence = nullptr; }

    m_dispFrameIdx = -1;
    m_pboSize = 0;
    m_glReady = false;
}

void GopCache::setFrameSize(int w, int h)
{
    if (w == m_frameW && h == m_frameH) return;
    m_frameW = w;
    m_frameH = h;

    if (!m_glReady) return;

    // Пересоздаём display-текстуры
    if (m_dispTexY)  glDeleteTextures(1, &m_dispTexY);
    if (m_dispTexUV) glDeleteTextures(1, &m_dispTexUV);

    m_dispTexY  = createTex(this, GL_R8,  w,   h,   GL_RED, GL_UNSIGNED_BYTE);
    m_dispTexUV = createTex(this, GL_RG8, w/2, h/2, GL_RG,  GL_UNSIGNED_BYTE);
    m_dispFrameIdx = -1;

    // Пересоздаём PBO
    size_t frameBytes = static_cast<size_t>(w) * h * 3 / 2;
    m_pboSize = frameBytes;

    for (int i = 0; i < PBO_COUNT; ++i) {
        if (m_pbo[i]) glDeleteBuffers(1, &m_pbo[i]);
        glGenBuffers(1, &m_pbo[i]);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pbo[i]);
        glBufferData(GL_PIXEL_UNPACK_BUFFER, frameBytes, nullptr, GL_STREAM_DRAW);
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    if (m_pboFence) { glDeleteSync(m_pboFence); m_pboFence = nullptr; }
    m_pboIndex = 0;
}

void GopCache::setMemoryLimit(size_t bytes)
{
    m_maxBytes = bytes;
}

GLuint GopCache::createTex(QOpenGLFunctions_4_5_Core* gl,
                           GLenum intFmt, int w, int h, GLenum fmt, GLenum type)
{
    GLuint tex = 0;
    gl->glGenTextures(1, &tex);
    gl->glBindTexture(GL_TEXTURE_2D, tex);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, intFmt, w, h, 0, fmt, type, nullptr);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return tex;
}

void GopCache::clear()
{
    QMutexLocker lk(&m_mutex);
    m_slots.clear();
    m_usedBytes = 0;
    m_dispFrameIdx = -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// ЗАПИСЬ КАДРА
// ─────────────────────────────────────────────────────────────────────────────

bool GopCache::storeFrame(const DecodedFrame& frame, int gopIdx,
                          int64_t gopFirstIdx, int64_t gopLastIdx)
{
    if (!frame.data[0] || !frame.data[1]) return false;
    if (frame.width != m_frameW || frame.height != m_frameH) return false;

    QMutexLocker lk(&m_mutex);

    // Ищем или создаём слот для этого GOP
    GopSlot* slot = findSlot(gopIdx);

    if (!slot) {
        // Создаём новый слот. Вставляем в правильную позицию (по gopIdx)
        GopSlot newSlot;
        newSlot.gopIdx        = gopIdx;
        newSlot.firstFrameIdx = gopFirstIdx;
        newSlot.lastFrameIdx  = gopLastIdx;
        newSlot.expectedCount = static_cast<int>(gopLastIdx - gopFirstIdx + 1);

        // Резервируем место для NV12 данных
        size_t frameBytes = static_cast<size_t>(m_frameW) * m_frameH * 3 / 2;
        size_t gopBytes   = frameBytes * newSlot.expectedCount;

        // Проверяем лимит RAM — если не хватает, вытесняем
        // (mutex уже захвачен, evict тоже работает под mutex — тут без рекурсии)
        while (m_usedBytes + gopBytes > m_maxBytes && !m_slots.empty()) {
            // Удаляем самый дальний от нового GOP
            int frontDist = std::abs(m_slots.front().gopIdx - gopIdx);
            int backDist  = std::abs(m_slots.back().gopIdx - gopIdx);
            if (frontDist >= backDist) {
                m_usedBytes -= m_slots.front().totalBytes();
                m_slots.pop_front();
            } else {
                m_usedBytes -= m_slots.back().totalBytes();
                m_slots.pop_back();
            }
        }

        // Резервируем память
        newSlot.nv12Data.reserve(gopBytes);

        // Вставляем в отсортированную позицию
        auto it = std::lower_bound(m_slots.begin(), m_slots.end(), gopIdx,
                                   [](const GopSlot& s, int idx) { return s.gopIdx < idx; });
        auto inserted = m_slots.insert(it, std::move(newSlot));
        slot = &(*inserted);
    }

    // Кадр уже есть?
    if (slot->idxToFrame.contains(frame.frameIdx))
        return true;

    // Копируем NV12 данные: Y + UV подряд
    size_t ySize  = static_cast<size_t>(frame.linesize[0]) * frame.height;
    size_t uvSize = static_cast<size_t>(frame.linesize[1]) * (frame.height / 2);
    size_t offset = slot->nv12Data.size();

    slot->nv12Data.resize(offset + ySize + uvSize);
    std::memcpy(slot->nv12Data.data() + offset, frame.data[0], ySize);
    std::memcpy(slot->nv12Data.data() + offset + ySize, frame.data[1], uvSize);

    // Записываем описание кадра
    FrameEntry entry;
    entry.frameIdx  = frame.frameIdx;
    entry.dataOffset = offset;
    entry.yStride   = frame.linesize[0];
    entry.uvStride  = frame.linesize[1];

    int frameSlotIdx = static_cast<int>(slot->frames.size());
    slot->frames.push_back(entry);
    slot->idxToFrame[frame.frameIdx] = frameSlotIdx;

    m_usedBytes += ySize + uvSize;

    // Проверяем полноту
    if (static_cast<int>(slot->frames.size()) >= slot->expectedCount)
        slot->complete = true;

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ЧТЕНИЕ КАДРА
// ─────────────────────────────────────────────────────────────────────────────

bool GopCache::contains(int64_t frameIdx) const
{
    QMutexLocker lk(&m_mutex);
    // Проверяем с допуском ±2 для защиты от ошибок округления PTS→frameIdx
    for (int d = 0; d <= 2; ++d) {
        if (findFrame(frameIdx + d)) return true;
        if (d > 0 && findFrame(frameIdx - d)) return true;
    }

    // Диагностика: если кадр в диапазоне какого-то GOP но не найден
    for (const auto& slot : m_slots) {
        if (frameIdx >= slot.firstFrameIdx && frameIdx <= slot.lastFrameIdx) {
            qDebug() << "GopCache::contains MISS idx=" << frameIdx
                     << "GOP" << slot.gopIdx
                     << "[" << slot.firstFrameIdx << ".." << slot.lastFrameIdx << "]"
                     << "stored=" << slot.frames.size()
                     << "/" << slot.expectedCount
                     << "complete=" << slot.complete;
            if (!slot.frames.empty())
                qDebug() << "  actual range: first=" << slot.frames.front().frameIdx
                         << "last=" << slot.frames.back().frameIdx;
        }
    }
    return false;
}

int64_t GopCache::findNearest(int64_t targetIdx, int tolerance) const
{
    QMutexLocker lk(&m_mutex);
    // Точное совпадение
    if (findFrame(targetIdx)) return targetIdx;
    // Поиск в окрестности ±tolerance
    for (int d = 1; d <= tolerance; ++d) {
        if (findFrame(targetIdx + d)) return targetIdx + d;
        if (findFrame(targetIdx - d)) return targetIdx - d;
    }
    return -1;
}

NV12Textures GopCache::displayFrame(int64_t frameIdx)
{
    if (!m_glReady || !m_dispTexY) return {};

    // Кадр уже в текстурах?
    if (frameIdx == m_dispFrameIdx)
        return { m_dispTexY, m_dispTexUV };

    QMutexLocker lk(&m_mutex);

    // Ищем кадр с допуском ±1 (ошибки округления PTS→frameIdx)
    const GopSlot* slot = nullptr;
    const FrameEntry* entry = findFrame(frameIdx, &slot);
    if (!entry) entry = findFrame(frameIdx - 1, &slot);
    if (!entry) entry = findFrame(frameIdx + 1, &slot);
    if (!entry || !slot) return {};

    // Указатели на NV12 данные
    const uint8_t* yData  = slot->nv12Data.data() + entry->dataOffset;
    const uint8_t* uvData = yData + static_cast<size_t>(entry->yStride) * m_frameH;

    lk.unlock();  // данные скопируем, mutex больше не нужен

    // Upload в display-текстуры через PBO
    uploadToPBO(yData, entry->yStride, uvData, entry->uvStride);
    m_dispFrameIdx = frameIdx;

    return { m_dispTexY, m_dispTexUV };
}

// ─────────────────────────────────────────────────────────────────────────────
// PBO UPLOAD — CPU RAM → GPU текстуры
// ─────────────────────────────────────────────────────────────────────────────

void GopCache::uploadToPBO(const uint8_t* yData, int yStride,
                           const uint8_t* uvData, int uvStride)
{
    // Ждём предыдущий transfer
    if (m_pboFence) {
        glClientWaitSync(m_pboFence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000);
        glDeleteSync(m_pboFence);
        m_pboFence = nullptr;
    }

    GLuint pbo = m_pbo[m_pboIndex];
    if (!pbo) return;

    size_t ySize  = static_cast<size_t>(yStride) * m_frameH;
    size_t uvSize = static_cast<size_t>(uvStride) * (m_frameH / 2);

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, m_pboSize, nullptr, GL_STREAM_DRAW);

    void* mapped = glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
    if (!mapped) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        // Fallback без PBO
        glBindTexture(GL_TEXTURE_2D, m_dispTexY);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, yStride);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_frameW, m_frameH,
                        GL_RED, GL_UNSIGNED_BYTE, yData);
        glBindTexture(GL_TEXTURE_2D, m_dispTexUV);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, uvStride / 2);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_frameW / 2, m_frameH / 2,
                        GL_RG, GL_UNSIGNED_BYTE, uvData);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        return;
    }

    uint8_t* dst = static_cast<uint8_t*>(mapped);
    std::memcpy(dst, yData, ySize);
    std::memcpy(dst + ySize, uvData, uvSize);
    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);

    // Y
    glBindTexture(GL_TEXTURE_2D, m_dispTexY);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, yStride);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_frameW, m_frameH,
                    GL_RED, GL_UNSIGNED_BYTE, reinterpret_cast<void*>(0));

    // UV
    glBindTexture(GL_TEXTURE_2D, m_dispTexUV);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, uvStride / 2);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_frameW / 2, m_frameH / 2,
                    GL_RG, GL_UNSIGNED_BYTE, reinterpret_cast<void*>(ySize));

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    m_pboFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    m_pboIndex = (m_pboIndex + 1) % PBO_COUNT;
}

// ─────────────────────────────────────────────────────────────────────────────
// ВЫТЕСНЕНИЕ
// ─────────────────────────────────────────────────────────────────────────────

void GopCache::evictForSpace(size_t needed, int direction, int targetGopIdx)
{
    QMutexLocker lk(&m_mutex);

    while (m_usedBytes + needed > m_maxBytes && !m_slots.empty()) {
        if (direction > 0) {
            // Убираем самые ранние (хвост)
            m_usedBytes -= m_slots.front().totalBytes();
            m_slots.pop_front();
        } else if (direction < 0) {
            // Убираем самые поздние (голову)
            m_usedBytes -= m_slots.back().totalBytes();
            m_slots.pop_back();
        } else {
            // Убираем самый дальний от target
            int frontDist = (targetGopIdx >= 0)
                                ? std::abs(m_slots.front().gopIdx - targetGopIdx) : 0;
            int backDist  = (targetGopIdx >= 0)
                               ? std::abs(m_slots.back().gopIdx - targetGopIdx) : 0;
            if (frontDist >= backDist) {
                m_usedBytes -= m_slots.front().totalBytes();
                m_slots.pop_front();
            } else {
                m_usedBytes -= m_slots.back().totalBytes();
                m_slots.pop_back();
            }
        }
    }
}

void GopCache::evictAfter(int gopIdx)
{
    QMutexLocker lk(&m_mutex);
    while (!m_slots.empty() && m_slots.back().gopIdx > gopIdx) {
        m_usedBytes -= m_slots.back().totalBytes();
        m_slots.pop_back();
    }
}

void GopCache::evictBefore(int gopIdx)
{
    QMutexLocker lk(&m_mutex);
    while (!m_slots.empty() && m_slots.front().gopIdx < gopIdx) {
        m_usedBytes -= m_slots.front().totalBytes();
        m_slots.pop_front();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// СОСТОЯНИЕ
// ─────────────────────────────────────────────────────────────────────────────

int GopCache::slotCount() const
{
    QMutexLocker lk(&m_mutex);
    return static_cast<int>(m_slots.size());
}

size_t GopCache::usedBytes() const
{
    QMutexLocker lk(&m_mutex);
    return m_usedBytes;
}

size_t GopCache::freeBytes() const
{
    QMutexLocker lk(&m_mutex);
    return (m_maxBytes > m_usedBytes) ? m_maxBytes - m_usedBytes : 0;
}

bool GopCache::hasGop(int gopIdx) const
{
    QMutexLocker lk(&m_mutex);
    return findSlot(gopIdx) != nullptr;
}

bool GopCache::isGopComplete(int gopIdx) const
{
    QMutexLocker lk(&m_mutex);
    const GopSlot* slot = findSlot(gopIdx);
    return slot && slot->complete;
}

void GopCache::markGopComplete(int gopIdx)
{
    QMutexLocker lk(&m_mutex);
    GopSlot* slot = findSlot(gopIdx);
    if (slot) slot->complete = true;
}

int GopCache::earliestGopIdx() const
{
    QMutexLocker lk(&m_mutex);
    return m_slots.empty() ? -1 : m_slots.front().gopIdx;
}

int GopCache::latestGopIdx() const
{
    QMutexLocker lk(&m_mutex);
    return m_slots.empty() ? -1 : m_slots.back().gopIdx;
}

// ─────────────────────────────────────────────────────────────────────────────
// ВНУТРЕННИЕ HELPERS
// ─────────────────────────────────────────────────────────────────────────────

GopCache::GopSlot* GopCache::findSlot(int gopIdx)
{
    // mutex уже захвачен вызывающим
    // Бинарный поиск — слоты отсортированы по gopIdx
    auto it = std::lower_bound(m_slots.begin(), m_slots.end(), gopIdx,
                               [](const GopSlot& s, int idx) { return s.gopIdx < idx; });
    if (it != m_slots.end() && it->gopIdx == gopIdx)
        return &(*it);
    return nullptr;
}

const GopCache::GopSlot* GopCache::findSlot(int gopIdx) const
{
    auto it = std::lower_bound(m_slots.begin(), m_slots.end(), gopIdx,
                               [](const GopSlot& s, int idx) { return s.gopIdx < idx; });
    if (it != m_slots.end() && it->gopIdx == gopIdx)
        return &(*it);
    return nullptr;
}

const GopCache::FrameEntry* GopCache::findFrame(int64_t frameIdx,
                                                const GopSlot** outSlot) const
{
    // mutex уже захвачен вызывающим
    // Ищем во всех слотах (обычно 2-8 слотов, быстро)
    for (const auto& slot : m_slots) {
        auto it = slot.idxToFrame.find(frameIdx);
        if (it != slot.idxToFrame.end()) {
            if (outSlot) *outSlot = &slot;
            return &slot.frames[it.value()];
        }
    }
    return nullptr;
}