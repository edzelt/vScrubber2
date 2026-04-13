#pragma once

#include <QOpenGLWidget>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLShaderProgram>
#include <memory>
#include <atomic>

#include "ffmpegdecoder.h"
#include "gopcache.h"
#include "zoompanstate.h"
#include "osd.h"
#include "thumbnailgenerator.h"

class InputController;
class TransportPanel;

// ── Видео-виджет: decode pipeline + OpenGL рендер ────────────────────────────
//
// Движок vScrubber2:
//   - Декодирование: FFmpeg + NVDEC, пакеты в RAM (PacketBuffer)
//   - Кэш кадров:   NV12 в RAM (GopCache), кольцевой буфер GOP-слотов
//   - Рендер:        OpenGL 4.5, YUV→RGB в шейдере (BT.709)
//   - Буфер:         хвост распакованных GOP для мгновенного реверса
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
    void stepFrame(int delta);        // +N вперёд, -N назад

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
    void endOfFileReached();
    void seekCompleted();              // seek/gopDecode завершён — для resetClock

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
    void onGopDecoded(double startPts, double endPts, int frameCount);
    void onNextDecoded(double pts);
    void onEndOfStream();

private:
    void initShaders();
    void blitTexToScreen(const NV12Textures& tex, int w, int h);
    void onFrameDecoded(const DecodedFrame& frame);

    // ── Управление GOP-кэшем ─────────────────────────────────────────────────
    void ensureGopLoaded(int gopIdx);      // текущий GOP загружен или загружается?
    void fillTailBackground();             // фоновое наращивание хвоста назад
    int  gopIdxForFrame(int64_t frameIdx) const;

    // ── Подсистемы ───────────────────────────────────────────────────────────
    std::unique_ptr<FFmpegDecoder>   m_decoder;
    std::unique_ptr<GopCache>        m_gopCache;
    InputController* m_inputController = nullptr;

    // ── OpenGL ───────────────────────────────────────────────────────────────
    std::unique_ptr<QOpenGLShaderProgram> m_shader;
    GLuint m_quadVAO = 0;

    // ── Состояние навигации ──────────────────────────────────────────────────
    double   m_currentPts      = 0.0;
    int64_t  m_currentIdx      = 0;
    double   m_pendingSeekPts  = -1.0;
    std::atomic<int64_t> m_lastDecodedIdx{-1};

    // ── GOP-управление ───────────────────────────────────────────────────────
    int      m_currentGopIdx   = -1;    // индекс текущего GOP
    int      m_fillingGopIdx   = -1;    // GOP который сейчас заполняет декодер
    bool     m_fillInProgress  = false; // декодер занят заполнением

    // ── Fallback — последний показанный кадр ────────────────────────────────
    int64_t  m_lastDisplayedIdx = -1;

    // ── Флаги ────────────────────────────────────────────────────────────────
    bool     m_initialized     = false;
    bool     m_fileLoaded      = false;
    bool     m_isFullRange     = false;

    // ── Zoom/Pan ─────────────────────────────────────────────────────────────
    ZoomPanState m_zoomPan;

    // ── OSD ──────────────────────────────────────────────────────────────────
    OSD m_osd;

    // ── Панель управления ────────────────────────────────────────────────────
    TransportPanel* m_transport = nullptr;

    // ── Генератор превью ─────────────────────────────────────────────────────
    std::unique_ptr<ThumbnailGenerator> m_thumbGen;

    // ── Пул буферов для копирования NV12 из decode потока ─────────────────────
    static constexpr int BUFFER_POOL_SIZE = 4;
    struct FrameBuffer {
        std::shared_ptr<std::vector<uint8_t>> data;
        std::atomic<bool> inUse{false};
    };
    FrameBuffer m_bufferPool[BUFFER_POOL_SIZE];
    std::shared_ptr<std::vector<uint8_t>> acquireBuffer(size_t size);
};