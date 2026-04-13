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

    m_transport = new TransportPanel(this);

    m_thumbGen = std::make_unique<ThumbnailGenerator>();
    m_thumbGen->start(QThread::LowPriority);
    m_transport->setThumbnailGenerator(m_thumbGen.get());
}

VideoWidget::~VideoWidget() { shutdown(); }

// ─────────────────────────────────────────────────────────────────────────────
// ИНИЦИАЛИЗАЦИЯ
// ─────────────────────────────────────────────────────────────────────────────

void VideoWidget::initializeGL()
{
    initializeOpenGLFunctions();
    initShaders();

    m_gopCache = std::make_unique<GopCache>();
    m_gopCache->initGL();

    m_decoder = std::make_unique<FFmpegDecoder>();

    // Callback: декодированные кадры → GopCache (RAM)
    m_decoder->setFrameCallback([this](const DecodedFrame& frame) {
        onFrameDecoded(frame);
    });

    // Проверка кэша для prefetch
    m_decoder->setCacheCheck([this](int64_t frameIdx) -> bool {
        return m_gopCache && m_gopCache->contains(frameIdx);
    });

    connect(m_decoder.get(), &FFmpegDecoder::fileOpened,
            this, &VideoWidget::onFileOpened, Qt::QueuedConnection);
    connect(m_decoder.get(), &FFmpegDecoder::gopDecoded,
            this, &VideoWidget::onGopDecoded, Qt::QueuedConnection);
    connect(m_decoder.get(), &FFmpegDecoder::prefetchGopDecoded,
            this, &VideoWidget::onGopDecoded, Qt::QueuedConnection);
    connect(m_decoder.get(), &FFmpegDecoder::nextDecoded,
            this, &VideoWidget::onNextDecoded, Qt::QueuedConnection);
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

    m_shader->addShaderFromSourceCode(QOpenGLShader::Fragment, R"glsl(
        #version 450 core
        in  vec2 vUV;
        out vec4 fragColor;

        uniform sampler2D uTexY;
        uniform sampler2D uTexUV;
        uniform vec2 uScale;
        uniform vec2 uOffset;
        uniform bool uFullRange;

        void main() {
            vec2 uv = (vUV - 0.5 - uOffset) / uScale + 0.5;
            if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
                fragColor = vec4(0.0, 0.0, 0.0, 1.0);
                return;
            }
            float y    = texture(uTexY,  uv).r;
            vec2  cbcr = texture(uTexUV, uv).rg;
            if (uFullRange) {
                float cb = cbcr.r - 0.5;
                float cr = cbcr.g - 0.5;
                fragColor = vec4(clamp(y + 1.5748*cr, 0.0, 1.0),
                                 clamp(y - 0.1873*cb - 0.4681*cr, 0.0, 1.0),
                                 clamp(y + 1.8556*cb, 0.0, 1.0), 1.0);
            } else {
                y  = (y - 16.0/255.0) * (255.0/219.0);
                float cb = (cbcr.r - 128.0/255.0) * (255.0/224.0);
                float cr = (cbcr.g - 128.0/255.0) * (255.0/224.0);
                fragColor = vec4(clamp(y + 1.5748*cr, 0.0, 1.0),
                                 clamp(y - 0.1873*cb - 0.4681*cr, 0.0, 1.0),
                                 clamp(y + 1.8556*cb, 0.0, 1.0), 1.0);
            }
        }
    )glsl");

    if (!m_shader->link())
        qWarning() << "Ошибка линковки шейдера:" << m_shader->log();
    glGenVertexArrays(1, &m_quadVAO);
}

void VideoWidget::resizeGL(int, int) {}

void VideoWidget::resizeEvent(QResizeEvent* event)
{
    QOpenGLWidget::resizeEvent(event);
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

    if (!m_gopCache || !m_gopCache->isInitialized()) return;

    // displayFrame загружает NV12 из RAM в display-текстуры через PBO
    NV12Textures tex = m_gopCache->displayFrame(m_currentIdx);
    if (tex) {
        m_lastDisplayedIdx = m_currentIdx;
        blitTexToScreen(tex, w, h);
    } else if (m_lastDisplayedIdx >= 0) {
        // Fallback — показываем последний валидный кадр
        NV12Textures fallback = m_gopCache->displayFrame(m_lastDisplayedIdx);
        if (fallback)
            blitTexToScreen(fallback, w, h);
    }

    if (m_fileLoaded) {
        m_osd.setTimecode(m_currentPts, duration(), fps());
        m_osd.setZoom(m_zoomPan.zoomLevel());
        QPainter painter(this);
        painter.setRenderHint(QPainter::TextAntialiasing);
        m_osd.draw(painter, width(), height());
        painter.end();
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
    m_zoomPan.setAspects(videoAspect, screenAspect);

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
// НАВИГАЦИЯ — всё через seekTo
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
    int targetGop = m_decoder->findGopByPts(pts);

    // Ищем кадр в кэше с допуском ±2 (ошибки округления PTS→frameIdx)
    int64_t foundIdx = m_gopCache ? m_gopCache->findNearest(targetIdx) : -1;

    if (foundIdx >= 0) {
        // Кадр найден — мгновенный показ
        m_currentIdx = foundIdx;
        m_currentPts = foundIdx / fpsVal;
        m_pendingSeekPts = -1.0;
        m_lastDecodedIdx.store(foundIdx, std::memory_order_release);
        m_currentGopIdx = targetGop;

        emit positionChanged(m_currentPts);
        emit seekCompleted();
        update();

        fillTailBackground();
        return;
    }

    // Кадра нет — загружаем GOP
    qDebug() << "seekTo: CACHE MISS idx=" << targetIdx
             << "pts=" << QString::number(pts, 'f', 4)
             << "gopIdx=" << targetGop
             << "curGop=" << m_currentGopIdx
             << "hasGop=" << m_gopCache->hasGop(targetGop)
             << "complete=" << m_gopCache->isGopComplete(targetGop)
             << "filling=" << m_fillInProgress
             << "fillingGop=" << m_fillingGopIdx;

    m_pendingSeekPts = pts;
    ensureGopLoaded(targetGop);
}

void VideoWidget::stepFrame(int delta)
{
    if (!m_decoder || !m_fileLoaded) return;
    if (delta == 0) return;

    double fpsVal = m_decoder->fps();
    if (fpsVal <= 0.0) return;
    if (m_pendingSeekPts >= 0.0) return;

    double newPts = m_currentPts + delta / fpsVal;
    double limit = m_decoder->lastFramePts();

    if (newPts > limit || newPts < 0.0) {
        emit endOfFileReached();
        return;
    }

    seekTo(newPts);
}

void VideoWidget::shutdown()
{
    if (!m_initialized) return;
    m_initialized = false;

    if (m_decoder) { m_decoder->stopThread(); m_decoder.reset(); }
    if (m_thumbGen) { m_thumbGen->stopThread(); m_thumbGen.reset(); }

    makeCurrent();
    if (m_gopCache) { m_gopCache->destroyGL(); m_gopCache.reset(); }
    if (m_quadVAO) { glDeleteVertexArrays(1, &m_quadVAO); m_quadVAO = 0; }
    doneCurrent();
}

double VideoWidget::duration() const { return m_decoder ? m_decoder->duration() : 0.0; }
double VideoWidget::fps()      const { return m_decoder ? m_decoder->fps() : 0.0; }
double VideoWidget::maxPts()   const { return m_decoder ? m_decoder->lastFramePts() : 0.0; }

// ─────────────────────────────────────────────────────────────────────────────
// GOP-УПРАВЛЕНИЕ
// ─────────────────────────────────────────────────────────────────────────────

int VideoWidget::gopIdxForFrame(int64_t frameIdx) const
{
    if (!m_decoder || m_decoder->fps() <= 0.0) return -1;
    return m_decoder->findGopByPts(frameIdx / m_decoder->fps());
}

void VideoWidget::ensureGopLoaded(int gopIdx)
{
    if (!m_decoder || gopIdx < 0 || gopIdx >= m_decoder->gopCount()) return;

    // Уже в кэше и полностью заполнен?
    if (m_gopCache->isGopComplete(gopIdx)) {
        m_currentGopIdx = gopIdx;
        return;
    }

    // Уже заполняется этот GOP (и prefetch, и decodeGOP)?
    // Тогда просто ждём — onFrameDecoded обработает pendingSeek
    if (m_fillInProgress && m_fillingGopIdx == gopIdx) return;

    // Заполняется другой GOP — прерываем и запускаем нужный
    m_currentGopIdx = gopIdx;
    m_fillingGopIdx = gopIdx;
    m_fillInProgress = true;

    m_decoder->cancelPrefetch();

    const GOPInfo& gop = m_decoder->gop(gopIdx);
    m_decoder->decodeGOP(gop.endPts);
}

// Фоновое наращивание буфера — декодируем соседние GOP пока есть место.
// Приоритет: сначала следующий GOP (для play forward), потом предыдущие (для реверса).
void VideoWidget::fillTailBackground()
{
    if (!m_decoder || m_fillInProgress) return;
    if (m_currentGopIdx < 0) return;

    int totalGops = m_decoder->gopCount();

    // 1) Следующий GOP — для бесшовного play forward
    int nextGop = m_currentGopIdx + 1;
    if (nextGop < totalGops && !m_gopCache->hasGop(nextGop)) {
        const GOPInfo& gop = m_decoder->gop(nextGop);
        size_t frameBytes = static_cast<size_t>(m_decoder->videoWidth())
                            * m_decoder->videoHeight() * 3 / 2;
        size_t needed = frameBytes * gop.frameCount;

        // Если не хватает места — освобождаем самые ранние (хвост)
        if (m_gopCache->freeBytes() < needed) {
            m_gopCache->evictForSpace(needed, 1, nextGop);  // 1 = убираем ранние
        }

        if (m_gopCache->freeBytes() >= needed) {
            m_fillingGopIdx = nextGop;
            m_fillInProgress = true;
            m_decoder->prefetchGOP(gop.startPts, gop.endPts);
            return;
        }
    }

    // 2) Предыдущий GOP — наращиваем хвост назад для реверса
    int earliestInCache = m_gopCache->earliestGopIdx();
    int prevGop = (earliestInCache > 0) ? earliestInCache - 1
                                        : m_currentGopIdx - 1;

    if (prevGop < 0) return;
    if (m_gopCache->hasGop(prevGop)) return;

    const GOPInfo& gop = m_decoder->gop(prevGop);
    size_t frameBytes = static_cast<size_t>(m_decoder->videoWidth())
                        * m_decoder->videoHeight() * 3 / 2;
    size_t needed = frameBytes * gop.frameCount;

    if (m_gopCache->freeBytes() < needed) return;  // пул полон — не трогаем

    m_fillingGopIdx = prevGop;
    m_fillInProgress = true;
    m_decoder->prefetchGOP(gop.startPts, gop.endPts);
}

// ─────────────────────────────────────────────────────────────────────────────
// CALLBACKS
// ─────────────────────────────────────────────────────────────────────────────

std::shared_ptr<std::vector<uint8_t>> VideoWidget::acquireBuffer(size_t size)
{
    for (auto& slot : m_bufferPool) {
        bool expected = false;
        if (slot.inUse.compare_exchange_strong(expected, true)) {
            if (!slot.data || slot.data->size() < size)
                slot.data = std::make_shared<std::vector<uint8_t>>(size);
            auto* slotPtr = &slot;
            return std::shared_ptr<std::vector<uint8_t>>(
                slot.data.get(),
                [slotPtr](std::vector<uint8_t>*) {
                    slotPtr->inUse.store(false, std::memory_order_release);
                });
        }
    }
    return std::make_shared<std::vector<uint8_t>>(size);
}

void VideoWidget::onFrameDecoded(const DecodedFrame& frame)
{
    if (!m_gopCache || !m_decoder) return;

    // Обновляем последний декодированный индекс
    m_lastDecodedIdx.store(frame.frameIdx, std::memory_order_release);

    // Определяем GOP для этого кадра
    int gopIdx = m_decoder->findGopByPts(frame.pts);
    if (gopIdx < 0) return;

    const GOPInfo& gop = m_decoder->gop(gopIdx);
    double fpsVal = m_decoder->fps();
    int64_t gopFirstIdx = static_cast<int64_t>(std::round(gop.startPts * fpsVal));
    int64_t gopLastIdx  = static_cast<int64_t>(std::round(gop.endPts * fpsVal));

    // Сохраняем NV12 данные в RAM-кэш (thread-safe)
    m_gopCache->storeFrame(frame, gopIdx, gopFirstIdx, gopLastIdx);

    // Обновляем экран из main thread
    // Обработка m_pendingSeekPts — только в onGopDecoded (main thread),
    // чтобы избежать гонки на неатомарном double.
    QMetaObject::invokeMethod(this, [this]() {
        if (!m_initialized) return;

        // Если ждём seek и кадр появился — сбрасываем pending
        if (m_pendingSeekPts >= 0.0 && m_gopCache) {
            double fpsVal = m_decoder ? m_decoder->fps() : 0.0;
            int64_t targetIdx = static_cast<int64_t>(std::round(m_pendingSeekPts * fpsVal));
            int64_t foundIdx = m_gopCache->findNearest(targetIdx);
            if (foundIdx >= 0) {
                m_currentIdx = foundIdx;
                m_currentPts = foundIdx / fpsVal;
                m_pendingSeekPts = -1.0;
                m_currentGopIdx = gopIdxForFrame(foundIdx);

                emit positionChanged(m_currentPts);
                emit seekCompleted();
            }
        }

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
    m_currentGopIdx = -1;
    m_fillingGopIdx = -1;
    m_fillInProgress = false;

    qDebug() << "VideoWidget: файл открыт,"
             << m_decoder->videoWidth() << "x" << m_decoder->videoHeight()
             << "@" << m_decoder->fps() << "fps,"
             << m_decoder->gopCount() << "GOP,"
             << "макс. GOP:" << m_decoder->maxGopFrames() << "кадров";

    makeCurrent();
    m_gopCache->clear();
    m_gopCache->setFrameSize(m_decoder->videoWidth(), m_decoder->videoHeight());
    doneCurrent();

    seekTo(0.0);
    emit fileLoaded(true);

    if (m_transport) {
        std::vector<double> kfPts;
        for (int i = 0; i < m_decoder->gopCount(); ++i)
            kfPts.push_back(m_decoder->gop(i).startPts);
        m_transport->setKeyframePts(kfPts);
    }

    if (m_thumbGen && m_transport)
        m_thumbGen->openFile(m_transport->currentFilePath());
}

void VideoWidget::onGopDecoded(double /*startPts*/, double endPts, int /*frameCount*/)
{
    m_fillInProgress = false;

    // Помечаем заполненный GOP как завершённый
    if (m_fillingGopIdx >= 0)
        m_gopCache->markGopComplete(m_fillingGopIdx);

    // Если pendingSeek всё ещё висит — onFrameDecoded мог не обработать
    // (из-за округления frameIdx или B-frames). Принудительно сбрасываем.
    if (m_pendingSeekPts >= 0.0) {
        double fpsVal = m_decoder->fps();
        int64_t targetIdx = static_cast<int64_t>(std::round(m_pendingSeekPts * fpsVal));

        // Пробуем точный кадр
        if (m_gopCache->contains(targetIdx)) {
            m_currentPts = m_pendingSeekPts;
            m_currentIdx = targetIdx;
        } else {
            // Fallback: берём последний декодированный
            int64_t idx = m_lastDecodedIdx.load(std::memory_order_acquire);
            if (idx >= 0) {
                m_currentIdx = idx;
                m_currentPts = idx / fpsVal;
            } else {
                m_currentPts = endPts;
                m_currentIdx = static_cast<int64_t>(std::round(endPts * fpsVal));
            }
        }

        m_pendingSeekPts = -1.0;
        m_currentGopIdx = gopIdxForFrame(m_currentIdx);

        emit positionChanged(m_currentPts);
        emit seekCompleted();
    }

    update();
    fillTailBackground();
}

void VideoWidget::onNextDecoded(double pts)
{
    int64_t idx = m_lastDecodedIdx.load(std::memory_order_acquire);
    m_currentPts = pts;
    m_currentIdx = (idx >= 0) ? idx : static_cast<int64_t>(std::round(pts * m_decoder->fps()));
    m_pendingSeekPts = -1.0;
    emit positionChanged(m_currentPts);
    update();
}

void VideoWidget::onEndOfStream()
{
    emit endOfFileReached();
}

// ─────────────────────────────────────────────────────────────────────────────
// ZOOM / PAN / OSD
// ─────────────────────────────────────────────────────────────────────────────

void VideoWidget::applyZoom(double delta, const QPointF& cursorPos)
{ m_zoomPan.zoom(delta, cursorPos, QSizeF(width(), height())); update(); }

void VideoWidget::applyPan(const QPointF& deltaPx)
{ m_zoomPan.pan(deltaPx, QSizeF(width(), height())); update(); }

void VideoWidget::resetZoom() { m_zoomPan.resetZoom(); update(); }

void VideoWidget::setOsdSpeed(double speed) { m_osd.setSpeed(speed); update(); }
void VideoWidget::setOsdWheelMode(int mode) { m_osd.setWheelMode(mode); update(); }

// ─────────────────────────────────────────────────────────────────────────────
// КЛАВИАТУРА / МЫШЬ
// ─────────────────────────────────────────────────────────────────────────────

void VideoWidget::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Right: stepFrame(1);  break;
    case Qt::Key_Left:  stepFrame(-1); break;
    default: QOpenGLWidget::keyPressEvent(event);
    }
}

void VideoWidget::mousePressEvent(QMouseEvent* e)
{ if (m_inputController) m_inputController->handleMousePress(e->button(), e->position()); }

void VideoWidget::mouseReleaseEvent(QMouseEvent* e)
{ if (m_inputController) m_inputController->handleMouseRelease(e->button(), e->position()); }

void VideoWidget::mouseMoveEvent(QMouseEvent* e)
{
    if (m_transport) m_transport->checkCursorVisibility(e->pos(), height());
    if (m_inputController) m_inputController->handleMouseMove(e->position());
}

void VideoWidget::mouseDoubleClickEvent(QMouseEvent* e)
{ if (m_inputController) m_inputController->handleMouseDoubleClick(e->button()); }

void VideoWidget::wheelEvent(QWheelEvent* e)
{
    if (m_transport && m_transport->isFileListVisible()) return;
    if (m_inputController) m_inputController->handleWheel(e->angleDelta().y(), e->position());
}

// ─────────────────────────────────────────────────────────────────────────────
// GOP НАВИГАЦИЯ
// ─────────────────────────────────────────────────────────────────────────────

int    VideoWidget::gopCount()    const { return m_decoder ? m_decoder->gopCount() : 0; }
int    VideoWidget::findGopByPts(double pts) const { return m_decoder ? m_decoder->findGopByPts(pts) : -1; }
double VideoWidget::gopStartPts(int gopIdx) const
{
    if (!m_decoder || gopIdx < 0 || gopIdx >= m_decoder->gopCount()) return 0.0;
    return m_decoder->gop(gopIdx).startPts;
}