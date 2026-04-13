#include "frameringbuffer.h"
#include "ffmpegdecoder.h"
#include <algorithm>
#include <cstring>

void FrameRingBuffer::init(int maxFrames)
{
    initializeOpenGLFunctions();
    m_maxFrames   = maxFrames;
    m_initialized = true;
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

    // Удаляем PBO
    for (int i = 0; i < PBO_COUNT; ++i) {
        if (m_pbo[i]) { glDeleteBuffers(1, &m_pbo[i]); m_pbo[i] = 0; }
    }
    if (m_pboFence) { glDeleteSync(m_pboFence); m_pboFence = nullptr; }
    m_pboSize = 0;

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
    m_frameW = w;
    m_frameH = h;

    // Пересоздаём текстуры
    for (auto& slot : m_slots) {
        if (slot.texY)  { glDeleteTextures(1, &slot.texY);  slot.texY  = 0; }
        if (slot.texUV) { glDeleteTextures(1, &slot.texUV); slot.texUV = 0; }
    }
    m_slots.clear();
    m_idxToSlot.clear();
    m_ageCounter = 0;
    m_slots.resize(m_maxFrames);

    // Пересоздаём PBO под новый размер
    // NV12: Y = w*h, UV = w*h/2, итого = w*h*3/2
    size_t frameBytes = static_cast<size_t>(w) * h * 3 / 2;
    m_pboSize = frameBytes;

    for (int i = 0; i < PBO_COUNT; ++i) {
        if (m_pbo[i]) glDeleteBuffers(1, &m_pbo[i]);
        glGenBuffers(1, &m_pbo[i]);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_pbo[i]);
        // GL_STREAM_DRAW: данные пишутся каждый кадр, используются один раз
        glBufferData(GL_PIXEL_UNPACK_BUFFER, frameBytes, nullptr, GL_STREAM_DRAW);
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    if (m_pboFence) { glDeleteSync(m_pboFence); m_pboFence = nullptr; }
    m_pboIndex = 0;
}

// ── Создание текстуры ────────────────────────────────────────────────────────
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

// ── Сохранение кадра через PBO ───────────────────────────────────────────────
//
// Двойная буферизация PBO (ping-pong):
//   Кадр N:   CPU → PBO[0] (memcpy в mapped buffer)
//             PBO[0] → texture (glTexSubImage2D, мгновенный возврат)
//   Кадр N+1: CPU → PBO[1]
//             PBO[1] → texture
//   Кадр N+2: CPU → PBO[0] (к этому моменту transfer из PBO[0] давно завершён)
//
// Без PBO: glTexSubImage2D блокирует пока данные не скопируются (~1мс на 1080p, ~4мс на 4K).
// С PBO:   memcpy в DMA-буфер (~0.3мс) + glTexSubImage2D из PBO (возврат ~0мс).
//
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
        slot.texY = createTexture(this, GL_R8, m_frameW, m_frameH,
                                  GL_RED, GL_UNSIGNED_BYTE);
        slot.texUV = createTexture(this, GL_RG8, m_frameW / 2, m_frameH / 2,
                                   GL_RG, GL_UNSIGNED_BYTE);
    }

    // ── PBO upload ───────────────────────────────────────────────────────────

    // Ждём завершения предыдущего PBO transfer (если есть fence)
    if (m_pboFence) {
        // Неблокирующая проверка — если transfer ещё не завершён, подождём немного
        GLenum result = glClientWaitSync(m_pboFence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000);
        // 1мс таймаут — если не успел, всё равно продолжаем (перезапишем PBO)
        Q_UNUSED(result)
        glDeleteSync(m_pboFence);
        m_pboFence = nullptr;
    }

    // Выбираем текущий PBO
    GLuint pbo = m_pbo[m_pboIndex];
    if (!pbo) {
        // Fallback без PBO (не должно случиться)
        goto fallback;
    }

    {
        // Маппим PBO для записи
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);

        // Orphaning: сообщаем драйверу что старые данные не нужны
        // Это позволяет драйверу выделить новый буфер вместо ожидания
        glBufferData(GL_PIXEL_UNPACK_BUFFER, m_pboSize, nullptr, GL_STREAM_DRAW);

        void* mapped = glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
        if (!mapped) {
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            goto fallback;
        }

        // Копируем NV12 данные в PBO: сначала Y, потом UV
        size_t ySize  = static_cast<size_t>(frame.linesize[0]) * frame.height;
        size_t uvSize = static_cast<size_t>(frame.linesize[1]) * (frame.height / 2);

        uint8_t* dst = static_cast<uint8_t*>(mapped);
        std::memcpy(dst, frame.data[0], ySize);
        std::memcpy(dst + ySize, frame.data[1], uvSize);

        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);

        // Загружаем Y из PBO (offset 0)
        glBindTexture(GL_TEXTURE_2D, slot.texY);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.linesize[0]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_frameW, m_frameH,
                        GL_RED, GL_UNSIGNED_BYTE,
                        reinterpret_cast<void*>(0));  // offset в PBO

        // Загружаем UV из PBO (offset = ySize)
        glBindTexture(GL_TEXTURE_2D, slot.texUV);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.linesize[1] / 2);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_frameW / 2, m_frameH / 2,
                        GL_RG, GL_UNSIGNED_BYTE,
                        reinterpret_cast<void*>(ySize));  // offset в PBO

        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

        // Ставим fence — следующий store подождёт завершения этого transfer
        m_pboFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

        // Переключаем PBO (ping-pong)
        m_pboIndex = (m_pboIndex + 1) % PBO_COUNT;
    }

    slot.frameIdx = idx;
    slot.age      = ++m_ageCounter;
    m_idxToSlot[idx] = slotIdx;
    return;

fallback:
    // Прямая загрузка без PBO (аварийный вариант)
    glBindTexture(GL_TEXTURE_2D, slot.texY);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.linesize[0]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_frameW, m_frameH,
                    GL_RED, GL_UNSIGNED_BYTE, frame.data[0]);

    glBindTexture(GL_TEXTURE_2D, slot.texUV);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.linesize[1] / 2);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_frameW / 2, m_frameH / 2,
                    GL_RG, GL_UNSIGNED_BYTE, frame.data[1]);

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    slot.frameIdx = idx;
    slot.age      = ++m_ageCounter;
    m_idxToSlot[idx] = slotIdx;
}

// ── Поиск текстур ────────────────────────────────────────────────────────────
NV12Textures FrameRingBuffer::lookup(int64_t frameIdx)
{
    QMutexLocker lk(&m_mutex);
    auto it = m_idxToSlot.find(frameIdx);
    if (it == m_idxToSlot.end()) return {};

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
    Q_UNUSED(frameIdx)

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