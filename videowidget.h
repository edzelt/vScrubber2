#pragma once

#include <QOpenGLWidget>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLShaderProgram>
#include <memory>
#include <atomic>

#include "ffmpegdecoder.h"
#include "frameringbuffer.h"
#include "zoompanstate.h"
#include "osd.h"
#include "thumbnailgenerator.h"

class InputController;
class TransportPanel;

// ── Видео-виджет: decode pipeline + OpenGL рендер ────────────────────────────
//
// Движок vScrubber2:
//   - Декодирование: FFmpeg + NVDEC, пакеты в RAM (PacketBuffer)
//   - Кэш кадров:   NV12 текстуры на GPU (FrameRingBuffer, 2 GOP)
//   - Рендер:        OpenGL 4.5, YUV→RGB конвертация в шейдере (BT.709)
//   - Prefetch:      автоматическая подгрузка предыдущего GOP для реверса
//
// Готовые точки интеграции для UI:
//   - seekTo(pts)     — перемотка на произвольную позицию (для ползунка)
//   - stepFrame(±N)   — покадровая навигация (для jog/shuttle)
//   - currentPts()    — текущая позиция в секундах
//   - currentIdx()    — текущий номер кадра
//   - duration()      — длительность файла
//   - fps()           — частота кадров
//   - positionChanged — сигнал обновления позиции (для ползунка)
class VideoWidget : public QOpenGLWidget,
                    protected QOpenGLFunctions_4_5_Core
{
    Q_OBJECT

public:
    explicit VideoWidget(QWidget* parent = nullptr);
    ~VideoWidget() override;

    // ── Управление файлом ────────────────────────────────────────────────────
    void openFile(const QString& path);
    void shutdown();

    // ── Контроллер ввода (устанавливается из MainWindow) ─────────────────────
    void setInputController(InputController* ic) { m_inputController = ic; }

    // ── Навигация ────────────────────────────────────────────────────────────
    void seekTo(double pts);          // перемотка на PTS (секунды)
    void stepFrame(int delta);        // +1 вперёд, -1 назад

    // ── Режим непрерывного воспроизведения ───────────────────────────────────
    // При play() — кадры декодируются последовательно без seek+flush.
    // При pause/jog — обычный путь через seekTo().
    void setContinuousPlay(bool enabled);
    bool isContinuousPlay() const { return m_continuousPlay; }
    void forceSyncDecoder();   // принудительная ресинхронизация (для loop)

    // ── GOP навигация (для перемотки по keyframe) ────────────────────────────
    int    gopCount()       const;
    int    findGopByPts(double pts) const;
    double gopStartPts(int gopIdx) const;

    // ── Свойства (read-only) ─────────────────────────────────────────────────
    double  currentPts() const { return m_currentPts; }
    int64_t currentIdx() const { return m_currentIdx; }
    double  duration()   const;
    double  fps()        const;
    double  maxPts()     const;   // реальный PTS последнего кадра
    bool    isFileLoaded() const { return m_fileLoaded; }

    // ── Zoom/Pan ─────────────────────────────────────────────────────────────
    void applyZoom(double delta, const QPointF& cursorPos);
    void applyPan(const QPointF& deltaPx);
    void resetZoom();
    double zoomLevel() const { return m_zoomPan.zoomLevel(); }

    // ── OSD обновление ───────────────────────────────────────────────────────
    void setOsdSpeed(double speed);
    void setOsdWheelMode(int mode);

    // ── Панель управления ────────────────────────────────────────────────────
    TransportPanel* transportPanel() const { return m_transport; }

signals:
    void fileLoaded(bool success);
    void positionChanged(double pts);
    void endOfFileReached();           // конец или начало файла при воспроизведении

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void onFileOpened(bool success, const QString& error);
    void onSeekComplete(double pts);
    void onGopDecoded(double startPts, double endPts, int frameCount);
    void onNextDecoded(double pts);
    void onPrefetchGopDecoded(double startPts, double endPts, int frameCount);
    void onEndOfStream();

private:
    void initShaders();
    void blitTexToScreen(const NV12Textures& tex, int w, int h);
    void onFrameDecoded(const DecodedFrame& frame);
    void schedulePrefetch();

    // ── Подсистемы ───────────────────────────────────────────────────────────
    std::unique_ptr<FFmpegDecoder>   m_decoder;
    std::unique_ptr<FrameRingBuffer> m_ringBuffer;
    InputController* m_inputController = nullptr;  // не владеет

    // ── OpenGL ───────────────────────────────────────────────────────────────
    std::unique_ptr<QOpenGLShaderProgram> m_shader;
    GLuint m_quadVAO = 0;

    // ── Состояние навигации ──────────────────────────────────────────────────
    double   m_currentPts      = 0.0;
    int64_t  m_currentIdx      = 0;
    double   m_pendingSeekPts  = -1.0;
    std::atomic<int64_t> m_lastDecodedIdx{-1};

    // ── Непрерывное воспроизведение ──────────────────────────────────────────
    bool     m_continuousPlay  = false;  // режим непрерывного декодирования
    int      m_continuousPending = 0;    // сколько decodeNext в очереди

    // ── Fallback-текстура (показывается пока decode не завершён) ──────────────
    NV12Textures m_lastDisplayedTex;
    int64_t      m_lastDisplayedIdx = -1;

    // ── Prefetch ─────────────────────────────────────────────────────────────
    int      m_navDirection    = 0;       // -1 / 0 / +1
    int64_t  m_prevNavIdx      = -1;
    bool     m_prefetchActive  = false;

    // ── Флаги ────────────────────────────────────────────────────────────────
    bool     m_initialized     = false;
    bool     m_fileLoaded      = false;
    bool     m_isFullRange     = false;   // color range видео

    // ── Zoom/Pan ─────────────────────────────────────────────────────────────
    ZoomPanState m_zoomPan;

    // ── OSD ──────────────────────────────────────────────────────────────────
    OSD m_osd;

    // ── Панель управления ────────────────────────────────────────────────────
    TransportPanel* m_transport = nullptr;

    // ── Генератор превью ─────────────────────────────────────────────────────
    std::unique_ptr<ThumbnailGenerator> m_thumbGen;

    // ── Пул буферов для копирования NV12 данных из decode потока ─────────────
    // Избегаем аллокации на каждый кадр — переиспользуем буферы.
    static constexpr int BUFFER_POOL_SIZE = 4;
    struct FrameBuffer {
        std::shared_ptr<std::vector<uint8_t>> data;
        std::atomic<bool> inUse{false};
    };
    FrameBuffer m_bufferPool[BUFFER_POOL_SIZE];
    std::shared_ptr<std::vector<uint8_t>> acquireBuffer(size_t size);
};