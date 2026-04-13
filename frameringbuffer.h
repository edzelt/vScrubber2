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

// ── Кольцевой GPU-буфер декодированных кадров (NV12) + PBO upload ────────────
//
// Хранит NV12 текстуры на GPU: Y (GL_R8) + UV (GL_RG8 чередующийся).
// Загрузка CPU → GPU через PBO (Pixel Buffer Objects) — асинхронный DMA.
//
// PBO ping-pong: два PBO чередуются. Пока GPU передаёт данные из PBO[0],
// CPU заполняет PBO[1]. glTexSubImage2D из PBO возвращается мгновенно.
//
// VRAM на кадр: width × height × 1.5 байт (NV12).
// PBO: 2 × (width × height × 1.5) байт в системной DMA-памяти.
//
class FrameRingBuffer : protected QOpenGLFunctions_4_5_Core
{
public:
    FrameRingBuffer() = default;
    ~FrameRingBuffer() { destroy(); }

    void init(int maxFrames = 300);
    void destroy();
    void clear();

    void setFrameSize(int w, int h);
    int frameW() const { return m_frameW; }
    int frameH() const { return m_frameH; }

    // Сохранить кадр: CPU NV12 → PBO → GL текстуры (асинхронно)
    void store(const DecodedFrame& frame);

    // Получить текстуры для кадра (LRU touch)
    NV12Textures lookup(int64_t frameIdx);
    NV12Textures lookup(double pts, double fps);

    // Проверка наличия (без touch)
    bool contains(int64_t frameIdx) const;

    int  count()    const;
    int  capacity() const { return m_maxFrames; }
    bool isInitialized() const { return m_initialized; }
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

    // ── PBO ping-pong ────────────────────────────────────────────────────────
    static constexpr int PBO_COUNT = 2;
    GLuint  m_pbo[PBO_COUNT] = {};     // PBO для Y+UV данных
    int     m_pboIndex       = 0;      // текущий PBO (0 или 1)
    size_t  m_pboSize        = 0;      // размер каждого PBO в байтах
    GLsync  m_pboFence       = nullptr; // fence для ожидания завершения предыдущего upload
};