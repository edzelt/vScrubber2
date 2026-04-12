#include "frameringbuffer.h"
#include "ffmpegdecoder.h"   // для DecodedFrame
#include <QDebug>
#include <algorithm>

void FrameRingBuffer::init(int maxFrames)
{
    initializeOpenGLFunctions();
    m_maxFrames   = maxFrames;
    m_initialized = true;
    qDebug() << "FrameRingBuffer: инициализирован, макс. кадров:" << maxFrames;
}

void FrameRingBuffer::destroy()
{
    if (!m_initialized) return;

    QMutexLocker lk(&m_mutex);
    for (auto& slot : m_slots) {
        if (slot.texY)  { glDeleteTextures(1, &slot.texY);  slot.texY  = 0; }
        if (slot.texUV) { glDeleteTextures(1, &slot.texUV); slot.texUV = 0; }
    }
    m_slots.clear();
    m_idxToSlot.clear();
    m_initialized = false;
}

void FrameRingBuffer::clear()
{
    QMutexLocker lk(&m_mutex);
    m_idxToSlot.clear();
    for (auto& slot : m_slots)
        slot.frameIdx = -1;
    m_ageCounter = 0;
}

void FrameRingBuffer::setFrameSize(int w, int h)
{
    if (w == m_frameW && h == m_frameH) return;

    QMutexLocker lk(&m_mutex);
    qDebug() << "FrameRingBuffer: размер кадра" << w << "x" << h
             << "NV12, VRAM на кадр:" << (w * h * 3 / 2) / 1024 << "КБ";
    m_frameW = w;
    m_frameH = h;

    // Пересоздаём все текстуры
    for (auto& slot : m_slots) {
        if (slot.texY)  { glDeleteTextures(1, &slot.texY);  slot.texY  = 0; }
        if (slot.texUV) { glDeleteTextures(1, &slot.texUV); slot.texUV = 0; }
    }
    m_slots.clear();
    m_idxToSlot.clear();
    m_ageCounter = 0;
    m_slots.resize(m_maxFrames);
}

// ── Создание одной текстуры с параметрами ────────────────────────────────────
static GLuint createTexture(QOpenGLFunctions_4_5_Core* gl,
                            GLenum internalFmt, int w, int h,
                            GLenum fmt, GLenum type)
{
    GLuint tex = 0;
    gl->glGenTextures(1, &tex);
    gl->glBindTexture(GL_TEXTURE_2D, tex);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, w, h, 0,
                     fmt, type, nullptr);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return tex;
}

// ── Сохранение кадра (CPU NV12 → GL текстуры Y + UV) ─────────────────────────
void FrameRingBuffer::store(const DecodedFrame& frame)
{
    if (!m_initialized || m_slots.isEmpty()) return;
    if (frame.width != m_frameW || frame.height != m_frameH) return;
    if (!frame.data[0] || !frame.data[1]) return;

    QMutexLocker lk(&m_mutex);

    int64_t idx = frame.frameIdx;

    // Уже в кэше — обновляем age
    auto it = m_idxToSlot.find(idx);
    if (it != m_idxToSlot.end()) {
        m_slots[it.value()].age = ++m_ageCounter;
        return;
    }

    int slotIdx = allocSlot(idx);
    Slot& slot = m_slots[slotIdx];

    // Ленивая аллокация текстур
    if (!slot.texY) {
        // Y: полный размер, один канал (GL_R8)
        slot.texY = createTexture(this, GL_R8, m_frameW, m_frameH,
                                  GL_RED, GL_UNSIGNED_BYTE);
        // UV: половинный размер, два канала чередующихся (GL_RG8)
        slot.texUV = createTexture(this, GL_RG8, m_frameW / 2, m_frameH / 2,
                                   GL_RG, GL_UNSIGNED_BYTE);
    }

    // Загружаем Y плоскость
    glBindTexture(GL_TEXTURE_2D, slot.texY);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.linesize[0]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_frameW, m_frameH,
                    GL_RED, GL_UNSIGNED_BYTE, frame.data[0]);

    // Загружаем UV плоскость (чередующийся U,V — NV12)
    // linesize[1] в байтах, каждый «пиксель» UV = 2 байта (RG)
    glBindTexture(GL_TEXTURE_2D, slot.texUV);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.linesize[1] / 2);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_frameW / 2, m_frameH / 2,
                    GL_RG, GL_UNSIGNED_BYTE, frame.data[1]);

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    slot.frameIdx = idx;
    slot.age      = ++m_ageCounter;
    m_idxToSlot[idx] = slotIdx;
}

// ── Поиск текстур по индексу кадра ───────────────────────────────────────────
NV12Textures FrameRingBuffer::lookup(int64_t frameIdx)
{
    QMutexLocker lk(&m_mutex);
    auto it = m_idxToSlot.find(frameIdx);
    if (it == m_idxToSlot.end()) return {};

    // LRU touch: обновляем age чтобы защитить от вытеснения
    Slot& slot = m_slots[it.value()];
    slot.age = ++m_ageCounter;
    return { slot.texY, slot.texUV };
}

NV12Textures FrameRingBuffer::lookup(double pts, double fps)
{
    if (fps <= 0.0) return {};
    int64_t idx = static_cast<int64_t>(std::round(pts * fps));
    return lookup(idx);
}

// ── Быстрая проверка наличия (без LRU touch) ────────────────────────────────
bool FrameRingBuffer::contains(int64_t frameIdx) const
{
    QMutexLocker lk(&m_mutex);
    return m_idxToSlot.contains(frameIdx);
}

int FrameRingBuffer::count() const
{
    QMutexLocker lk(&m_mutex);
    return m_idxToSlot.size();
}

// ── Проверка непрерывного диапазона ──────────────────────────────────────────
bool FrameRingBuffer::hasRange(int64_t fromIdx, int64_t toIdx) const
{
    QMutexLocker lk(&m_mutex);
    if (fromIdx > toIdx) std::swap(fromIdx, toIdx);
    for (int64_t i = fromIdx; i <= toIdx; ++i) {
        if (!m_idxToSlot.contains(i)) return false;
    }
    return true;
}

// ── LRU вытеснение ───────────────────────────────────────────────────────────
int FrameRingBuffer::allocSlot(int64_t frameIdx)
{
    // Свободный слот
    for (int i = 0; i < m_slots.size(); ++i) {
        if (m_slots[i].frameIdx < 0) return i;
    }

    // Вытесняем самый старый
    int oldest = 0;
    uint64_t minAge = m_slots[0].age;
    for (int i = 1; i < m_slots.size(); ++i) {
        if (m_slots[i].age < minAge) {
            minAge = m_slots[i].age;
            oldest = i;
        }
    }

    m_idxToSlot.remove(m_slots[oldest].frameIdx);
    m_slots[oldest].frameIdx = -1;
    return oldest;
}