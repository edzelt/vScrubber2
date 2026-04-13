#pragma once

#include <QOpenGLFunctions_4_5_Core>
#include <QMutex>
#include <QHash>
#include <deque>
#include <vector>
#include <cstdint>

struct DecodedFrame;

// ── Пара текстур для отображения NV12 кадра ──────────────────────────────────
struct NV12Textures {
    GLuint texY  = 0;   // GL_R8,  полный размер (width × height)
    GLuint texUV = 0;   // GL_RG8, половинный размер (width/2 × height/2)

    bool isValid() const { return texY != 0 && texUV != 0; }
    explicit operator bool() const { return isValid(); }
};

// ── Кольцевой кэш GOP-слотов в RAM ──────────────────────────────────────────
//
// Архитектура:
//   - Кадры хранятся в RAM как NV12 данные (не GPU-текстуры)
//   - Единица хранения — один полный GOP (GopSlot)
//   - Слоты упорядочены по времени в deque
//   - Суммарный объём ограничен лимитом RAM (по умолчанию 2 ГБ)
//   - Для отображения — одна пара display-текстур, upload через PBO
//
// Логика вытеснения:
//   - Вперёд: если нового GOP нет в кэше — убираем самые ранние (хвост)
//   - Назад:  при выходе из текущего GOP вперёд — удаляем его (голову)
//   - Перескок: убираем с ненужной стороны
//
// Фоновое заполнение:
//   - Декодер заполняет текущий GOP на максимальной скорости
//   - После текущего — наращивает хвост назад (для будущего реверса)
//
// VRAM: ~12.4 МБ (одна пара 4K текстур + 2 PBO)
// RAM:  настраиваемый лимит (по умолчанию 2 ГБ)
//
class GopCache : protected QOpenGLFunctions_4_5_Core
{
public:
    GopCache() = default;
    ~GopCache() { destroyGL(); }

    // Запрет копирования
    GopCache(const GopCache&) = delete;
    GopCache& operator=(const GopCache&) = delete;

    // ── Инициализация ────────────────────────────────────────────────────────
    void initGL();                         // создать GL ресурсы (вызывать в GL контексте)
    void destroyGL();                      // удалить GL ресурсы
    void setFrameSize(int w, int h);       // размер кадра (при открытии файла)
    void setMemoryLimit(size_t bytes);     // лимит RAM (по умолчанию 2 ГБ)
    void clear();                          // очистить все слоты

    bool isInitialized() const { return m_glReady; }
    int  frameW() const { return m_frameW; }
    int  frameH() const { return m_frameH; }

    // ── Запись кадра (вызывается из любого потока) ────────────────────────────
    // Кадр сохраняется в RAM-слот соответствующего GOP.
    // Если GOP ещё нет — создаётся автоматически.
    // gopIdx — индекс GOP в PacketBuffer.
    // Возвращает true если кадр принят.
    bool storeFrame(const DecodedFrame& frame, int gopIdx,
                    int64_t gopFirstIdx, int64_t gopLastIdx);

    // ── Чтение кадра ─────────────────────────────────────────────────────────
    // Проверка наличия кадра в RAM
    bool contains(int64_t frameIdx) const;

    // Найти ближайший кадр к targetIdx в пределах ±tolerance (по умолчанию ±2).
    // Возвращает frameIdx найденного кадра или -1 если не найден.
    int64_t findNearest(int64_t targetIdx, int tolerance = 2) const;

    // Загрузить кадр в display-текстуры (через PBO) и вернуть их.
    // Вызывать в GL контексте (paintGL).
    // Если кадра нет — возвращает невалидные текстуры.
    NV12Textures displayFrame(int64_t frameIdx);

    // ── Управление памятью ───────────────────────────────────────────────────
    // Освободить место для нового GOP.
    // direction > 0: убираем с хвоста (ранние), direction < 0: убираем с головы (поздние)
    // direction == 0: убираем самый дальний от targetGopIdx.
    void evictForSpace(size_t needed, int direction, int targetGopIdx = -1);

    // Удалить все GOP позднее указанного (при движении назад)
    void evictAfter(int gopIdx);

    // Удалить все GOP ранее указанного (при движении вперёд — освобождение хвоста)
    void evictBefore(int gopIdx);

    // ── Состояние ────────────────────────────────────────────────────────────
    int    slotCount() const;              // количество GOP в кэше
    size_t usedBytes() const;              // суммарный размер NV12 данных
    size_t freeBytes() const;              // доступно до лимита
    bool   hasGop(int gopIdx) const;       // GOP уже в кэше?
    bool   isGopComplete(int gopIdx) const; // GOP полностью заполнен?
    void   markGopComplete(int gopIdx);    // принудительно пометить GOP как завершённый

    // Самый ранний и самый поздний GOP в кэше
    int    earliestGopIdx() const;
    int    latestGopIdx() const;

private:
    // ── Запись одного кадра внутри GOP ────────────────────────────────────────
    struct FrameEntry {
        int64_t frameIdx = -1;    // номер кадра
        size_t  dataOffset = 0;   // смещение в GopSlot::nv12Data
        int     yStride = 0;      // stride Y плоскости
        int     uvStride = 0;     // stride UV плоскости
    };

    // ── Один GOP в кэше ──────────────────────────────────────────────────────
    struct GopSlot {
        int     gopIdx       = -1;
        int64_t firstFrameIdx = 0;
        int64_t lastFrameIdx  = 0;
        int     expectedCount = 0;  // сколько кадров ожидаем
        bool    complete     = false;

        std::vector<uint8_t>   nv12Data;   // непрерывный блок NV12 данных
        std::vector<FrameEntry> frames;    // описание каждого кадра
        QHash<int64_t, int>    idxToFrame; // frameIdx → индекс в frames

        size_t totalBytes() const { return nv12Data.size(); }
    };

    // ── Поиск слота ──────────────────────────────────────────────────────────
    GopSlot* findSlot(int gopIdx);
    const GopSlot* findSlot(int gopIdx) const;
    const FrameEntry* findFrame(int64_t frameIdx, const GopSlot** outSlot = nullptr) const;

    // ── PBO upload ───────────────────────────────────────────────────────────
    void uploadToPBO(const uint8_t* yData, int yStride,
                     const uint8_t* uvData, int uvStride);

    static GLuint createTex(QOpenGLFunctions_4_5_Core* gl,
                            GLenum intFmt, int w, int h, GLenum fmt, GLenum type);

    // ── Данные ───────────────────────────────────────────────────────────────
    mutable QMutex m_mutex;

    // GOP-слоты, упорядочены по gopIdx (по времени)
    std::deque<GopSlot> m_slots;

    // Размер кадра
    int  m_frameW = 0;
    int  m_frameH = 0;

    // Лимит RAM
    size_t m_maxBytes  = 2ULL * 1024 * 1024 * 1024;  // 2 ГБ по умолчанию
    size_t m_usedBytes = 0;

    // ── Display-текстуры (одна пара для рендера) ─────────────────────────────
    GLuint m_dispTexY  = 0;
    GLuint m_dispTexUV = 0;
    int64_t m_dispFrameIdx = -1;  // какой кадр сейчас в текстурах

    // ── PBO ping-pong ────────────────────────────────────────────────────────
    static constexpr int PBO_COUNT = 2;
    GLuint m_pbo[PBO_COUNT] = {};
    int    m_pboIndex       = 0;
    size_t m_pboSize        = 0;
    GLsync m_pboFence       = nullptr;

    bool m_glReady = false;
};