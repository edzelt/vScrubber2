#pragma once

#include <QOpenGLFunctions_4_5_Core>
#include <QMutex>
#include <QMap>
#include <QList>
#include <cmath>

struct DecodedFrame;

// ── Пара текстур для NV12 кадра ─────────────────────────────────────────────
struct NV12Textures {
    GLuint texY  = 0;   // GL_R8,  полный размер (width × height)
    GLuint texUV = 0;   // GL_RG8, половинный размер (width/2 × height/2)

    bool isValid() const { return texY != 0 && texUV != 0; }
    explicit operator bool() const { return isValid(); }
};

// ── Кольцевой GPU-буфер декодированных кадров (NV12) ─────────────────────────
//
// Хранит NV12 текстуры на GPU: Y (GL_R8) + UV (GL_RG8 чередующийся).
// Конвертация YUV → RGB выполняется шейдером при отрисовке.
//
// VRAM на кадр: width × height × 1.5 байт (вместо × 4 для RGBA).
// Для 1080p: 300 кадров × 3МБ ≈ 0.9ГБ VRAM.
// Для 4K:    300 кадров × 12МБ ≈ 3.6ГБ VRAM.
//
// Thread safety: store() и lookup() защищены мьютексом.
// store() вызывается из main потока (через QueuedConnection),
// lookup() из render потока.
class FrameRingBuffer : protected QOpenGLFunctions_4_5_Core
{
public:
    FrameRingBuffer() = default;
    ~FrameRingBuffer() { destroy(); }

    // ── Инициализация (вызывать в GL контексте) ──────────────────────────────
    void init(int maxFrames = 300);
    void destroy();
    void clear();

    // ── Размер кадра (при изменении — сброс всех текстур) ────────────────────
    void setFrameSize(int w, int h);
    int frameW() const { return m_frameW; }
    int frameH() const { return m_frameH; }

    // ── Основные операции ────────────────────────────────────────────────────

    // Сохранить декодированный кадр (NV12 данные из CPU → GL текстуры).
    // Thread-safe. Вызывается из main потока.
    void store(const DecodedFrame& frame);

    // Получить пару GL текстур для кадра. Возвращает {0,0} при промахе.
    // При попадании обновляет age (LRU touch) — защищает от вытеснения.
    // Thread-safe. Вызывается из render потока.
    NV12Textures lookup(int64_t frameIdx);
    NV12Textures lookup(double pts, double fps);

    // Быстрая проверка наличия кадра в кэше (без touch).
    // Thread-safe. Вызывается из decode потока для cacheCheck.
    bool contains(int64_t frameIdx) const;

    // ── Информация ───────────────────────────────────────────────────────────
    int  count()    const;
    int  capacity() const { return m_maxFrames; }
    bool isInitialized() const { return m_initialized; }

    // Проверить есть ли непрерывный диапазон кадров [from..to] в буфере
    bool hasRange(int64_t fromIdx, int64_t toIdx) const;

private:
    int allocSlot(int64_t frameIdx);

    struct Slot {
        GLuint  texY     = 0;
        GLuint  texUV    = 0;
        int64_t frameIdx = -1;
        uint64_t age     = 0;
    };

    mutable QMutex         m_mutex;
    QList<Slot>            m_slots;
    QMap<int64_t, int>     m_idxToSlot;

    int      m_maxFrames   = 0;
    int      m_frameW      = 0;
    int      m_frameH      = 0;
    uint64_t m_ageCounter  = 0;
    bool     m_initialized = false;
};