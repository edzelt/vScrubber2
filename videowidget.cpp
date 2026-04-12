#include "videowidget.h"
#include "inputcontroller.h"
#include "transportpanel.h"
#include <QDebug>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QVector2D>
#include <cmath>
#include <cstring>

VideoWidget::VideoWidget(QWidget* parent)
    : QOpenGLWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);

    // Панель управления (child widget поверх OpenGL)
    m_transport = new TransportPanel(this);

    // Генератор превью (фоновый поток)
    m_thumbGen = std::make_unique<ThumbnailGenerator>();
    m_thumbGen->start(QThread::LowPriority);
    m_transport->setThumbnailGenerator(m_thumbGen.get());
}

VideoWidget::~VideoWidget()
{
    shutdown();
}

// ─────────────────────────────────────────────────────────────────────────────
// ИНИЦИАЛИЗАЦИЯ
// ─────────────────────────────────────────────────────────────────────────────

void VideoWidget::initializeGL()
{
    initializeOpenGLFunctions();
    initShaders();

    // Ring buffer — создаём, но не инициализируем размер.
    // Размер определится после открытия файла (2 × maxGopFrames).
    m_ringBuffer = std::make_unique<FrameRingBuffer>();

    // Декодер
    m_decoder = std::make_unique<FFmpegDecoder>();

    // Callback: декодированные кадры → ring buffer (через main thread)
    m_decoder->setFrameCallback([this](const DecodedFrame& frame) {
        onFrameDecoded(frame);
    });

    // Проверка кэша из decode потока (для пропуска уже кэшированных при prefetch)
    m_decoder->setCacheCheck([this](int64_t frameIdx) -> bool {
        return m_ringBuffer && m_ringBuffer->contains(frameIdx);
    });

    // Сигналы декодера
    connect(m_decoder.get(), &FFmpegDecoder::fileOpened,
            this, &VideoWidget::onFileOpened, Qt::QueuedConnection);
    connect(m_decoder.get(), &FFmpegDecoder::seekComplete,
            this, &VideoWidget::onSeekComplete, Qt::QueuedConnection);
    connect(m_decoder.get(), &FFmpegDecoder::gopDecoded,
            this, &VideoWidget::onGopDecoded, Qt::QueuedConnection);
    connect(m_decoder.get(), &FFmpegDecoder::nextDecoded,
            this, &VideoWidget::onNextDecoded, Qt::QueuedConnection);
    connect(m_decoder.get(), &FFmpegDecoder::prefetchGopDecoded,
            this, &VideoWidget::onPrefetchGopDecoded, Qt::QueuedConnection);
    connect(m_decoder.get(), &FFmpegDecoder::endOfStream,
            this, &VideoWidget::onEndOfStream, Qt::QueuedConnection);

    m_decoder->start(QThread::HighPriority);
    m_initialized = true;
}

void VideoWidget::initShaders()
{
    m_shader = std::make_unique<QOpenGLShaderProgram>();

    m_shader->addShaderFromSourceCode(QOpenGLShader::Vertex, R"glsl(
        #version 450 core
        const vec2 pos[4] = vec2[](
            vec2(-1,-1), vec2(1,-1), vec2(-1,1), vec2(1,1)
        );
        const vec2 uv[4] = vec2[](
            vec2(0,1), vec2(1,1), vec2(0,0), vec2(1,0)
        );
        out vec2 vUV;
        void main() {
            vUV = uv[gl_VertexID];
            gl_Position = vec4(pos[gl_VertexID], 0.0, 1.0);
        }
    )glsl");

    // NV12 → RGB по BT.709 (поддержка limited и full range)
    m_shader->addShaderFromSourceCode(QOpenGLShader::Fragment, R"glsl(
        #version 450 core
        in  vec2 vUV;
        out vec4 fragColor;

        uniform sampler2D uTexY;
        uniform sampler2D uTexUV;
        uniform vec2 uScale;
        uniform vec2 uOffset;
        uniform bool uFullRange;   // true = 0-255, false = 16-235

        void main() {
            vec2 uv = (vUV - 0.5 - uOffset) / uScale + 0.5;

            if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
                fragColor = vec4(0.0, 0.0, 0.0, 1.0);
                return;
            }

            float y    = texture(uTexY,  uv).r;
            vec2  cbcr = texture(uTexUV, uv).rg;

            // Нормализация в зависимости от диапазона
            if (uFullRange) {
                // Full range: 0-255 → 0.0-1.0 (уже нормализовано текстурой)
                float cb = cbcr.r - 0.5;
                float cr = cbcr.g - 0.5;
                float r = y + 1.5748 * cr;
                float g = y - 0.1873 * cb - 0.4681 * cr;
                float b = y + 1.8556 * cb;
                fragColor = vec4(clamp(r, 0.0, 1.0),
                                 clamp(g, 0.0, 1.0),
                                 clamp(b, 0.0, 1.0), 1.0);
            } else {
                // Limited range: Y 16-235, UV 16-240
                y  = (y  - 16.0/255.0) * (255.0/219.0);
                float cb = (cbcr.r - 128.0/255.0) * (255.0/224.0);
                float cr = (cbcr.g - 128.0/255.0) * (255.0/224.0);
                float r = y + 1.5748 * cr;
                float g = y - 0.1873 * cb - 0.4681 * cr;
                float b = y + 1.8556 * cb;
                fragColor = vec4(clamp(r, 0.0, 1.0),
                                 clamp(g, 0.0, 1.0),
                                 clamp(b, 0.0, 1.0), 1.0);
            }
        }
    )glsl");

    if (!m_shader->link())
        qWarning() << "Ошибка линковки шейдера:" << m_shader->log();

    glGenVertexArrays(1, &m_quadVAO);
}

void VideoWidget::resizeGL(int /*w*/, int /*h*/)
{
    // Ничего — letterbox пересчитывается в paintGL
}

void VideoWidget::resizeEvent(QResizeEvent* event)
{
    QOpenGLWidget::resizeEvent(event);

    // Позиционируем панель управления в нижней части виджета
    if (m_transport) {
        int panelH = 60;
        m_transport->setGeometry(0, height() - panelH, width(), panelH);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// РЕНДЕР
// ─────────────────────────────────────────────────────────────────────────────

void VideoWidget::paintGL()
{
    if (!m_initialized) return;

    const int w = width()  * devicePixelRatio();
    const int h = height() * devicePixelRatio();

    glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    glViewport(0, 0, w, h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (!m_ringBuffer || !m_ringBuffer->isInitialized()) return;

    NV12Textures tex = m_ringBuffer->lookup(m_currentIdx);
    if (tex) {
        m_lastDisplayedTex = tex;
        m_lastDisplayedIdx = m_currentIdx;
        blitTexToScreen(tex, w, h);
    } else if (m_lastDisplayedTex) {
        // Fallback — проверяем что текстура не вытеснена
        NV12Textures check = m_ringBuffer->lookup(m_lastDisplayedIdx);
        if (check.texY == m_lastDisplayedTex.texY) {
            blitTexToScreen(m_lastDisplayedTex, w, h);
        } else {
            m_lastDisplayedTex = {};
            m_lastDisplayedIdx = -1;
        }
    }

    // ── OSD поверх видео ─────────────────────────────────────────────────────
    if (m_fileLoaded) {
        m_osd.setTimecode(m_currentPts, duration(), fps());
        m_osd.setZoom(m_zoomPan.zoomLevel());

        QPainter painter(this);
        painter.setRenderHint(QPainter::TextAntialiasing);
        m_osd.draw(painter, width(), height());
        painter.end();

        // Обновляем панель управления
        if (m_transport)
            m_transport->setPosition(m_currentPts, duration(), fps());
    }
}

void VideoWidget::blitTexToScreen(const NV12Textures& tex, int w, int h)
{
    m_shader->bind();

    float videoAspect  = (m_decoder && m_decoder->videoHeight() > 0)
                            ? float(m_decoder->videoWidth()) / float(m_decoder->videoHeight())
                            : 16.0f / 9.0f;
    float screenAspect = (h > 0) ? float(w) / float(h) : 1.0f;

    // Обновляем aspect для ZoomPanState (нужно для clampPan)
    m_zoomPan.setAspects(videoAspect, screenAspect);

    // Scale и Offset от ZoomPanState (letterbox + zoom + pan)
    QPointF scale  = m_zoomPan.calcShaderScale();
    QPointF offset = m_zoomPan.calcShaderOffset();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex.texY);
    m_shader->setUniformValue("uTexY", 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex.texUV);
    m_shader->setUniformValue("uTexUV", 1);

    m_shader->setUniformValue("uScale", QVector2D(scale.x(), scale.y()));
    m_shader->setUniformValue("uOffset", QVector2D(offset.x(), offset.y()));
    m_shader->setUniformValue("uFullRange", m_isFullRange);

    glBindVertexArray(m_quadVAO);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    m_shader->release();
}

// ─────────────────────────────────────────────────────────────────────────────
// УПРАВЛЕНИЕ
// ─────────────────────────────────────────────────────────────────────────────

void VideoWidget::openFile(const QString& path)
{
    if (m_decoder) m_decoder->openFile(path);
}

void VideoWidget::seekTo(double pts)
{
    if (!m_decoder || !m_fileLoaded) return;

    double fpsVal = m_decoder->fps();
    int64_t targetIdx = static_cast<int64_t>(std::round(pts * fpsVal));

    // Любой seek сбрасывает очередь непрерывного воспроизведения
    m_continuousPending = 0;

    // Определяем направление навигации
    if (m_prevNavIdx >= 0 && targetIdx != m_prevNavIdx) {
        int newDir = (targetIdx > m_prevNavIdx) ? 1 : -1;
        if (newDir != m_navDirection) {
            m_decoder->cancelPrefetch();
            m_prefetchActive = false;
        }
        m_navDirection = newDir;
    }
    m_prevNavIdx = targetIdx;

    m_currentPts = pts;
    m_currentIdx = targetIdx;

    // Cache hit — показываем мгновенно
    if (m_ringBuffer && m_ringBuffer->lookup(targetIdx)) {
        m_pendingSeekPts = -1.0;
        emit positionChanged(pts);
        update();
        schedulePrefetch();
        return;
    }

    // Cache miss — декодируем
    m_pendingSeekPts = pts;
    m_decoder->cancelPrefetch();
    m_prefetchActive = false;

    // decodeNext если следующий кадр последовательный
    int64_t lastDecoded = m_lastDecodedIdx.load(std::memory_order_acquire);
    if (lastDecoded > 0 && targetIdx == lastDecoded + 1) {
        m_decoder->decodeNext();
    } else {
        m_decoder->decodeGOP(pts);
    }
}

void VideoWidget::setContinuousPlay(bool enabled)
{
    bool wasEnabled = m_continuousPlay;
    m_continuousPlay = enabled;
    m_continuousPending = 0;

    if (enabled && !wasEnabled) {
        // Синхронизируем декодер с текущей позицией (только при переходе в continuous)
        if (m_decoder && m_fileLoaded) {
            m_decoder->cancelPrefetch();
            m_prefetchActive = false;
            m_decoder->seekAndDecode(m_currentPts);
        }
    }
}

// Принудительная ресинхронизация (для loop)
void VideoWidget::forceSyncDecoder()
{
    m_continuousPending = 0;
    if (m_decoder && m_fileLoaded && m_continuousPlay) {
        m_decoder->cancelPrefetch();
        m_prefetchActive = false;
        m_decoder->seekAndDecode(m_currentPts);
    }
}

void VideoWidget::stepFrame(int delta)
{
    if (!m_decoder || !m_fileLoaded) return;

    double fpsVal = m_decoder->fps();
    if (fpsVal <= 0.0) return;

    double maxPtsVal = maxPts();  // реальный PTS последнего кадра

    // ── Непрерывное воспроизведение: decodeNext без seek ──────────────────────
    if (m_continuousPlay && delta == 1) {
        // Ограничиваем очередь: максимум 2 pending decodeNext
        // Конец файла определяется сигналом endOfStream от декодера
        if (m_continuousPending >= 2)
            return;

        m_continuousPending++;
        m_decoder->decodeNext();
        return;
    }

    // ── Реверс при непрерывном воспроизведении ───────────────────────────────
    if (m_continuousPlay && delta == -1) {
        // Проверяем начало файла
        if (m_currentPts <= 0.5 / fpsVal) {
            emit endOfFileReached();
            return;
        }

        double newPts = m_currentPts - 1.0 / fpsVal;
        newPts = std::clamp(newPts, 0.0, maxPtsVal);
        seekTo(newPts);
        return;
    }

    // ── Обычный режим (jog, shuttle, стрелки) ────────────────────────────────
    if (m_pendingSeekPts >= 0.0) {
        double nextPts = m_pendingSeekPts + delta / fpsVal;
        nextPts = std::clamp(nextPts, 0.0, maxPtsVal);
        if (std::abs(nextPts - m_currentPts) > 8.0 / fpsVal)
            return;
        m_pendingSeekPts = nextPts;
        m_currentPts = nextPts;
        m_currentIdx = static_cast<int64_t>(std::round(nextPts * fpsVal));
        return;
    }

    double newPts = m_currentPts + delta / fpsVal;
    newPts = std::clamp(newPts, 0.0, maxPtsVal);
    seekTo(newPts);
}

void VideoWidget::shutdown()
{
    if (!m_initialized) return;
    m_initialized = false;

    if (m_decoder) {
        m_decoder->stopThread();
        m_decoder.reset();
    }

    if (m_thumbGen) {
        m_thumbGen->stopThread();
        m_thumbGen.reset();
    }

    makeCurrent();
    if (m_ringBuffer) {
        m_ringBuffer->destroy();
        m_ringBuffer.reset();
    }
    if (m_quadVAO) { glDeleteVertexArrays(1, &m_quadVAO); m_quadVAO = 0; }
    doneCurrent();
}

double VideoWidget::duration() const
{
    return m_decoder ? m_decoder->duration() : 0.0;
}

double VideoWidget::fps() const
{
    return m_decoder ? m_decoder->fps() : 0.0;
}

double VideoWidget::maxPts() const
{
    return m_decoder ? m_decoder->lastFramePts() : 0.0;
}

// ─────────────────────────────────────────────────────────────────────────────
// CALLBACKS
// ─────────────────────────────────────────────────────────────────────────────

// ── Пул буферов: переиспользуем память вместо аллокации на каждый кадр ────────
std::shared_ptr<std::vector<uint8_t>> VideoWidget::acquireBuffer(size_t size)
{
    for (auto& slot : m_bufferPool) {
        bool expected = false;
        if (slot.inUse.compare_exchange_strong(expected, true)) {
            // Нашли свободный слот
            if (!slot.data || slot.data->size() < size)
                slot.data = std::make_shared<std::vector<uint8_t>>(size);
            // Возвращаем shared_ptr с кастомным deleter — освобождает слот при уничтожении
            auto* slotPtr = &slot;
            return std::shared_ptr<std::vector<uint8_t>>(
                slot.data.get(),
                [slotPtr](std::vector<uint8_t>*) {
                    slotPtr->inUse.store(false, std::memory_order_release);
                });
        }
    }
    // Все слоты заняты — fallback на обычную аллокацию
    return std::make_shared<std::vector<uint8_t>>(size);
}

void VideoWidget::onFrameDecoded(const DecodedFrame& frame)
{
    if (!m_ringBuffer) return;

    // Обновляем позицию только для пользовательских команд (не prefetch)
    if (!frame.isPrefetch)
        m_lastDecodedIdx.store(frame.frameIdx, std::memory_order_release);

    // Копируем NV12 данные (Y + UV) в буфер из пула
    size_t ySize  = frame.linesize[0] * frame.height;
    size_t uvSize = frame.linesize[1] * (frame.height / 2);

    auto dataCopy = acquireBuffer(ySize + uvSize);
    std::memcpy(dataCopy->data(), frame.data[0], ySize);
    std::memcpy(dataCopy->data() + ySize, frame.data[1], uvSize);

    DecodedFrame frameCopy = frame;
    frameCopy.data[0] = dataCopy->data();
    frameCopy.data[1] = dataCopy->data() + ySize;

    // TODO: Заменить glTexSubImage2D на PBO для асинхронного upload на 4K
    QMetaObject::invokeMethod(this, [this, frameCopy, dataCopy]() {
        if (!m_ringBuffer || !m_initialized) return;
        makeCurrent();
        m_ringBuffer->store(frameCopy);
        doneCurrent();
        update();
    }, Qt::QueuedConnection);
}

void VideoWidget::onFileOpened(bool success, const QString& error)
{
    if (!success) {
        qWarning() << "VideoWidget: ошибка открытия:" << error;
        emit fileLoaded(false);
        return;
    }

    m_fileLoaded = true;
    m_isFullRange = m_decoder->isFullRange();

    // Размер буфера: 2 GOP + запас
    int gopFrames = m_decoder->maxGopFrames();
    if (gopFrames <= 0) gopFrames = 150;
    int bufferSize = gopFrames * 2 + 30;

    qDebug() << "VideoWidget: файл открыт,"
             << m_decoder->videoWidth() << "x" << m_decoder->videoHeight()
             << "@" << m_decoder->fps() << "fps,"
             << m_decoder->gopCount() << "GOP,"
             << "макс. GOP:" << gopFrames << "кадров,"
             << "буфер:" << bufferSize << "слотов";

    makeCurrent();
    m_ringBuffer->init(bufferSize);
    m_ringBuffer->setFrameSize(m_decoder->videoWidth(), m_decoder->videoHeight());
    doneCurrent();

    seekTo(0.0);
    emit fileLoaded(true);

    // Передаём данные в панель управления
    if (m_transport) {
        // Keyframe карта для snap-to-keyframe
        std::vector<double> kfPts;
        for (int i = 0; i < m_decoder->gopCount(); ++i)
            kfPts.push_back(m_decoder->gop(i).startPts);
        m_transport->setKeyframePts(kfPts);
    }

    // Запускаем генерацию превью (фоновый поток)
    // Путь берём из TransportPanel (он уже установлен через setCurrentFile)
    if (m_thumbGen && m_transport) {
        m_thumbGen->openFile(m_transport->currentFilePath());
    }
}

void VideoWidget::onSeekComplete(double pts)
{
    m_currentPts = pts;
    m_currentIdx = static_cast<int64_t>(std::round(pts * m_decoder->fps()));
    m_pendingSeekPts = -1.0;
    emit positionChanged(pts);
    update();
}

void VideoWidget::onGopDecoded(double startPts, double endPts, int frameCount)
{
    Q_UNUSED(startPts) Q_UNUSED(endPts) Q_UNUSED(frameCount)

    m_pendingSeekPts = -1.0;
    emit positionChanged(m_currentPts);
    update();
    schedulePrefetch();
}

void VideoWidget::onNextDecoded(double pts)
{
    m_currentPts = pts;
    m_currentIdx = static_cast<int64_t>(std::round(pts * m_decoder->fps()));
    m_pendingSeekPts = -1.0;

    // Уменьшаем счётчик pending для непрерывного воспроизведения
    if (m_continuousPending > 0)
        m_continuousPending--;

    emit positionChanged(m_currentPts);
    update();
    schedulePrefetch();
}

// ─────────────────────────────────────────────────────────────────────────────
// PREFETCH: держим предыдущий GOP в буфере для мгновенного реверса
// ─────────────────────────────────────────────────────────────────────────────

void VideoWidget::schedulePrefetch()
{
    if (!m_decoder || !m_fileLoaded || !m_ringBuffer) return;
    if (m_prefetchActive) return;
    if (m_decoder->gopCount() < 2) return;

    // Не запускаем prefetch при непрерывном воспроизведении —
    // он сбивает m_demuxerContinuous и ломает decodeNext
    if (m_continuousPlay) return;

    // Текущий GOP
    int curGopIdx = m_decoder->findGopByPts(m_currentPts);
    if (curGopIdx <= 0) return;  // первый GOP — некуда

    // Предыдущий GOP
    const GOPInfo& prevGop = m_decoder->gop(curGopIdx - 1);

    // Проверяем — есть ли начало и конец предыдущего GOP в буфере
    double fpsVal = m_decoder->fps();
    int64_t firstIdx = static_cast<int64_t>(std::round(prevGop.startPts * fpsVal));
    int64_t lastIdx  = static_cast<int64_t>(std::round(prevGop.endPts * fpsVal));

    if (m_ringBuffer->contains(firstIdx) && m_ringBuffer->contains(lastIdx))
        return;  // уже в кэше

    m_decoder->prefetchGOP(prevGop.startPts, prevGop.endPts);
    m_prefetchActive = true;
}

void VideoWidget::onPrefetchGopDecoded(double /*startPts*/, double /*endPts*/,
                                       int /*frameCount*/)
{
    m_prefetchActive = false;
}

void VideoWidget::onEndOfStream()
{
    m_continuousPending = 0;
    emit endOfFileReached();
}

// ─────────────────────────────────────────────────────────────────────────────
// ZOOM / PAN
// ─────────────────────────────────────────────────────────────────────────────

void VideoWidget::applyZoom(double delta, const QPointF& cursorPos)
{
    m_zoomPan.zoom(delta, cursorPos, QSizeF(width(), height()));
    update();
}

void VideoWidget::applyPan(const QPointF& deltaPx)
{
    m_zoomPan.pan(deltaPx, QSizeF(width(), height()));
    update();
}

void VideoWidget::resetZoom()
{
    m_zoomPan.resetZoom();
    update();
}

// ─────────────────────────────────────────────────────────────────────────────
// OSD ОБНОВЛЕНИЕ (вызывается из MainWindow)
// ─────────────────────────────────────────────────────────────────────────────

void VideoWidget::setOsdSpeed(double speed)
{
    m_osd.setSpeed(speed);
    update();
}

void VideoWidget::setOsdWheelMode(int mode)
{
    m_osd.setWheelMode(mode);
    update();
}

// ─────────────────────────────────────────────────────────────────────────────
// КЛАВИАТУРА — делегирование в InputController, стрелки напрямую
// ─────────────────────────────────────────────────────────────────────────────

void VideoWidget::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Right: stepFrame(1);  break;
    case Qt::Key_Left:  stepFrame(-1); break;
    default:
        // Остальное обрабатывает InputController через MainWindow
        QOpenGLWidget::keyPressEvent(event);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// СОБЫТИЯ МЫШИ — делегирование в InputController
// ─────────────────────────────────────────────────────────────────────────────

void VideoWidget::mousePressEvent(QMouseEvent* event)
{
    if (m_inputController)
        m_inputController->handleMousePress(event->button(), event->position());
    // Не вызываем base — не нужно стандартное поведение
}

void VideoWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_inputController)
        m_inputController->handleMouseRelease(event->button(), event->position());
}

void VideoWidget::mouseMoveEvent(QMouseEvent* event)
{
    // Проверяем видимость панели управления
    if (m_transport)
        m_transport->checkCursorVisibility(event->pos(), height());

    if (m_inputController)
        m_inputController->handleMouseMove(event->position());
}

void VideoWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (m_inputController)
        m_inputController->handleMouseDoubleClick(event->button());
}

void VideoWidget::wheelEvent(QWheelEvent* event)
{
    // Если открыт список файлов — колесо для скролла списка, не для видео
    if (m_transport && m_transport->isFileListVisible())
        return;

    if (m_inputController)
        m_inputController->handleWheel(event->angleDelta().y(), event->position());
}

// ─────────────────────────────────────────────────────────────────────────────
// GOP НАВИГАЦИЯ — для перемотки по ключевым кадрам
// ─────────────────────────────────────────────────────────────────────────────

int VideoWidget::gopCount() const
{
    return m_decoder ? m_decoder->gopCount() : 0;
}

int VideoWidget::findGopByPts(double pts) const
{
    return m_decoder ? m_decoder->findGopByPts(pts) : -1;
}

double VideoWidget::gopStartPts(int gopIdx) const
{
    if (!m_decoder || gopIdx < 0 || gopIdx >= m_decoder->gopCount())
        return 0.0;
    return m_decoder->gop(gopIdx).startPts;
}