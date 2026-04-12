#include "transportpanel.h"
#include "thumbnailgenerator.h"
#include <QSlider>
#include <QLabel>
#include <QPushButton>
#include <QListWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QPainter>
#include <QDir>
#include <QFileInfo>
#include <QEnterEvent>
#include <QMouseEvent>
#include <QApplication>
#include <QStyle>
#include <QStyleOptionSlider>
#include <algorithm>

// ── Фильтр видеофайлов ──────────────────────────────────────────────────────
static const QStringList VIDEO_EXTENSIONS = {
    "*.mp4", "*.mkv", "*.mov", "*.avi", "*.hevc", "*.h265",
    "*.h264", "*.ts", "*.mts", "*.m2ts", "*.wmv", "*.flv", "*.webm"
};

// ── Кастомный слайдер: клик = переход на позицию клика ─────────────────────
class ClickSlider : public QSlider {
public:
    using QSlider::QSlider;
protected:
    void mousePressEvent(QMouseEvent* event) override {
        QStyleOptionSlider opt;
        initStyleOption(&opt);
        QRect grooveRect = style()->subControlRect(
            QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
        int pos = static_cast<int>(event->position().x());
        int sliderMin = grooveRect.x();
        int sliderMax = grooveRect.right();
        if (sliderMax <= sliderMin) { QSlider::mousePressEvent(event); return; }
        int value = QStyle::sliderValueFromPosition(
            minimum(), maximum(), pos - sliderMin, sliderMax - sliderMin);
        setValue(value);
        emit sliderMoved(value);
        QSlider::mousePressEvent(event);
    }
};

// ── Конструктор ──────────────────────────────────────────────────────────────
TransportPanel::TransportPanel(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_TranslucentBackground);
    setAutoFillBackground(false);

    m_hideTimer.setSingleShot(true);
    m_hideTimer.setInterval(HIDE_DELAY_MS);
    connect(&m_hideTimer, &QTimer::timeout, this, &TransportPanel::onHideTimer);

    buildUI();
    hide();
}

// ── UI ───────────────────────────────────────────────────────────────────────
void TransportPanel::buildUI()
{
    QString btnStyle = R"(
        QPushButton {
            background: rgba(60, 60, 60, 200);
            color: #ccc;
            border: 1px solid rgba(100, 100, 100, 150);
            border-radius: 3px;
            padding: 3px 8px;
            font-size: 12px;
            min-width: 28px;
        }
        QPushButton:hover { background: rgba(80, 80, 80, 220); color: #fff; }
        QPushButton:pressed { background: rgba(100, 100, 100, 240); }
        QPushButton:checked {
            background: rgba(40, 100, 160, 220);
            color: #fff;
            border-color: rgba(60, 140, 220, 200);
        }
    )";

    QString sliderStyle = R"(
        QSlider::groove:horizontal {
            height: 6px;
            background: rgba(80, 80, 80, 200);
            border-radius: 3px;
        }
        QSlider::handle:horizontal {
            width: 14px; height: 14px; margin: -4px 0;
            background: rgba(200, 200, 200, 240);
            border-radius: 7px;
        }
        QSlider::handle:horizontal:hover { background: #fff; }
        QSlider::sub-page:horizontal {
            background: rgba(50, 130, 200, 200);
            border-radius: 3px;
        }
    )";

    m_slider = new ClickSlider(Qt::Horizontal, this);
    m_slider->setRange(0, 10000);
    m_slider->setStyleSheet(sliderStyle);
    m_slider->setMouseTracking(true);
    m_slider->installEventFilter(this);  // для превью при наведении
    connect(m_slider, &QSlider::sliderPressed,  this, &TransportPanel::onSliderPressed);
    connect(m_slider, &QSlider::sliderMoved,    this, &TransportPanel::onSliderMoved);
    connect(m_slider, &QSlider::sliderReleased, this, &TransportPanel::onSliderReleased);

    m_timeLabel = new QLabel("00:00 / 00:00", this);
    m_timeLabel->setStyleSheet("color: #ccc; font-size: 11px; font-family: Consolas;");
    m_timeLabel->setFixedWidth(130);

    m_btnPrev = new QPushButton("◀◀", this);
    m_btnPrev->setStyleSheet(btnStyle);
    m_btnPrev->setToolTip("Предыдущий файл");
    connect(m_btnPrev, &QPushButton::clicked, this, &TransportPanel::onPrevFile);

    m_btnNext = new QPushButton("▶▶", this);
    m_btnNext->setStyleSheet(btnStyle);
    m_btnNext->setToolTip("Следующий файл");
    connect(m_btnNext, &QPushButton::clicked, this, &TransportPanel::onNextFile);

    m_btnLoopFile = new QPushButton("🔁", this);
    m_btnLoopFile->setCheckable(true);
    m_btnLoopFile->setStyleSheet(btnStyle);
    m_btnLoopFile->setToolTip("Зацикливание файла");
    connect(m_btnLoopFile, &QPushButton::clicked, this, &TransportPanel::onToggleLoopFile);

    m_btnLoopDir = new QPushButton("📂", this);
    m_btnLoopDir->setCheckable(true);
    m_btnLoopDir->setStyleSheet(btnStyle);
    m_btnLoopDir->setToolTip("Проигрывание каталога");
    connect(m_btnLoopDir, &QPushButton::clicked, this, &TransportPanel::onToggleLoopDir);

    m_btnFileList = new QPushButton("📋", this);
    m_btnFileList->setStyleSheet(btnStyle);
    m_btnFileList->setToolTip("Список файлов каталога");
    connect(m_btnFileList, &QPushButton::clicked, this, &TransportPanel::onShowFileList);

    auto* btnLayout = new QHBoxLayout;
    btnLayout->setSpacing(4);
    btnLayout->addWidget(m_timeLabel);
    btnLayout->addStretch();
    btnLayout->addWidget(m_btnPrev);
    btnLayout->addWidget(m_btnNext);
    btnLayout->addSpacing(8);
    btnLayout->addWidget(m_btnLoopFile);
    btnLayout->addWidget(m_btnLoopDir);
    btnLayout->addSpacing(8);
    btnLayout->addWidget(m_btnFileList);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 4, 8, 6);
    mainLayout->setSpacing(2);
    mainLayout->addWidget(m_slider);
    mainLayout->addLayout(btnLayout);
}

// ── Фон ──────────────────────────────────────────────────────────────────────
void TransportPanel::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QLinearGradient grad(0, 0, 0, height());
    grad.setColorAt(0.0, QColor(0, 0, 0, 0));
    grad.setColorAt(0.3, QColor(0, 0, 0, 160));
    grad.setColorAt(1.0, QColor(0, 0, 0, 200));
    p.fillRect(rect(), grad);
}

// ── Обновление позиции ───────────────────────────────────────────────────────
void TransportPanel::setPosition(double pts, double duration, double fps)
{
    m_currentPts = pts;
    m_duration   = duration;
    m_fps        = fps;

    if (!m_sliderDragging && m_duration > 0.0) {
        int val = static_cast<int>(pts / m_duration * 10000.0);
        m_slider->setValue(std::clamp(val, 0, 10000));
    }
    updateTimecodeLabel();
}

void TransportPanel::setKeyframePts(const std::vector<double>& keyframePts)
{
    m_keyframePts = keyframePts;
}

void TransportPanel::updateTimecodeLabel()
{
    m_timeLabel->setText(QString("%1 / %2")
                             .arg(formatTime(m_currentPts), formatTime(m_duration)));
}

// ── Видимость ────────────────────────────────────────────────────────────────
void TransportPanel::showPanel()
{
    if (m_panelVisible) return;
    m_panelVisible = true;
    show();
    raise();
    m_hideTimer.stop();
}

void TransportPanel::hidePanel()
{
    if (!m_panelVisible) return;
    m_panelVisible = false;
    hide();
    if (m_fileListWgt && m_fileListWgt->isVisible())
        m_fileListWgt->hide();
    if (m_thumbTooltip)
        m_thumbTooltip->hide();
}

void TransportPanel::checkCursorVisibility(const QPointF& cursorPos, int widgetHeight)
{
    bool inZone = cursorPos.y() > widgetHeight * 0.85;
    bool popupOpen = m_fileListWgt && m_fileListWgt->isVisible();

    if (inZone || m_sliderDragging || popupOpen) {
        showPanel();
        m_hideTimer.stop();
    } else if (m_panelVisible && !underMouse()) {
        if (!m_hideTimer.isActive())
            m_hideTimer.start();
    }
}

void TransportPanel::enterEvent(QEnterEvent* event)
{
    Q_UNUSED(event)
    m_hideTimer.stop();
}

void TransportPanel::leaveEvent(QEvent* event)
{
    Q_UNUSED(event)
    bool popupOpen = m_fileListWgt && m_fileListWgt->isVisible();
    if (!m_sliderDragging && !popupOpen)
        m_hideTimer.start();
}

void TransportPanel::onHideTimer() { hidePanel(); }

bool TransportPanel::isFileListVisible() const
{
    return m_fileListWgt && m_fileListWgt->isVisible();
}

// ── Слайдер ──────────────────────────────────────────────────────────────────
void TransportPanel::onSliderPressed()
{
    m_sliderDragging = true;
    if (m_duration > 0.0) {
        double pts = m_slider->value() / 10000.0 * m_duration;
        m_currentPts = pts;
        updateTimecodeLabel();
        emit seekRequested(pts);
    }
}

void TransportPanel::onSliderMoved(int value)
{
    if (m_duration <= 0.0) return;
    double pts = value / 10000.0 * m_duration;
    m_currentPts = pts;
    updateTimecodeLabel();
    emit seekRequested(pts);
}

void TransportPanel::onSliderReleased()
{
    m_sliderDragging = false;
    if (m_duration <= 0.0) return;
    double pts = m_slider->value() / 10000.0 * m_duration;
    emit seekRequested(pts);
}

// ── Кнопки Loop ──────────────────────────────────────────────────────────────
void TransportPanel::onToggleLoopFile()
{
    m_loopFile = m_btnLoopFile->isChecked();
}

void TransportPanel::onToggleLoopDir()
{
    m_loopDir = m_btnLoopDir->isChecked();
}

// ── Файловая навигация ───────────────────────────────────────────────────────
void TransportPanel::setCurrentFile(const QString& filePath)
{
    m_currentFile = filePath;
    loadDirectoryFiles(filePath);
}

void TransportPanel::loadDirectoryFiles(const QString& filePath)
{
    QFileInfo fi(filePath);
    QDir dir = fi.absoluteDir();

    m_dirFiles.clear();
    m_currentFileIdx = -1;

    const QFileInfoList entries = dir.entryInfoList(VIDEO_EXTENSIONS, QDir::Files, QDir::Name);
    for (const auto& entry : std::as_const(entries)) {
        m_dirFiles.append(entry.absoluteFilePath());
        if (entry.absoluteFilePath() == fi.absoluteFilePath())
            m_currentFileIdx = m_dirFiles.size() - 1;
    }

    m_btnPrev->setEnabled(m_dirFiles.size() > 1);
    m_btnNext->setEnabled(m_dirFiles.size() > 1);
}

void TransportPanel::onPrevFile()
{
    if (!tryPrevFile()) return;
}

void TransportPanel::onNextFile()
{
    if (!tryNextFile()) return;
}

bool TransportPanel::tryNextFile()
{
    if (m_dirFiles.isEmpty() || m_currentFileIdx < 0) return false;
    int idx = m_currentFileIdx + 1;
    if (idx >= m_dirFiles.size()) return false;
    emit fileSelected(m_dirFiles[idx]);
    return true;
}

bool TransportPanel::tryPrevFile()
{
    if (m_dirFiles.isEmpty() || m_currentFileIdx < 0) return false;
    int idx = m_currentFileIdx - 1;
    if (idx < 0) return false;
    emit fileSelected(m_dirFiles[idx]);
    return true;
}

void TransportPanel::goToFirstFile()
{
    if (m_dirFiles.isEmpty()) return;
    emit fileSelected(m_dirFiles.first());
}

void TransportPanel::goToLastFile()
{
    if (m_dirFiles.isEmpty()) return;
    emit fileSelected(m_dirFiles.last());
}

// ── Список файлов (отдельное окно) ───────────────────────────────────────────
void TransportPanel::onShowFileList()
{
    if (!m_fileListWgt) {
        // Создаём как child родительского виджета (VideoWidget), не панели
        m_fileListWgt = new QListWidget(parentWidget());
        m_fileListWgt->setStyleSheet(R"(
            QListWidget {
                background: rgba(30, 30, 30, 240);
                color: #ccc;
                border: 1px solid rgba(100, 100, 100, 150);
                font-size: 13px;
            }
            QListWidget::item { padding: 4px; }
            QListWidget::item:selected {
                background: rgba(50, 130, 200, 200);
                color: #fff;
            }
            QListWidget::item:hover { background: rgba(60, 60, 60, 200); }
        )");
        m_fileListWgt->hide();

        connect(m_fileListWgt, &QListWidget::itemDoubleClicked,
                this, [this](QListWidgetItem*) {
                    int idx = m_fileListWgt->currentRow();
                    if (idx >= 0 && idx < m_dirFiles.size()) {
                        emit fileSelected(m_dirFiles[idx]);
                        m_fileListWgt->hide();
                    }
                });
    }

    // Toggle видимость
    if (m_fileListWgt->isVisible()) {
        m_fileListWgt->hide();
        return;
    }

    // Заполняем список
    m_fileListWgt->clear();
    for (const auto& path : std::as_const(m_dirFiles)) {
        QFileInfo fi(path);
        m_fileListWgt->addItem(fi.fileName());
    }

    if (m_currentFileIdx >= 0)
        m_fileListWgt->setCurrentRow(m_currentFileIdx);

    // Позиционируем над панелью, внутри родительского виджета
    QWidget* parent = parentWidget();
    int listH = std::min(static_cast<int>(parent->height() * 0.6),
                         static_cast<int>(m_dirFiles.size()) * 24 + 6);
    listH = std::max(listH, 60);
    int listW = std::min(parent->width() - 16, 500);
    int x = (parent->width() - listW) / 2;
    int y = parent->height() - height() - listH - 4;

    m_fileListWgt->setGeometry(x, y, listW, listH);
    m_fileListWgt->show();
    m_fileListWgt->raise();
    m_fileListWgt->setFocus();
}

// ── Превью при наведении на таймлайн ──────────────────────────────────────────
bool TransportPanel::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == m_slider && m_thumbGen && m_duration > 0.0) {
        if (event->type() == QEvent::MouseMove) {
            auto* me = static_cast<QMouseEvent*>(event);
            // Позиция на слайдере → PTS
            int sliderW = m_slider->width();
            if (sliderW <= 0) return false;
            double ratio = std::clamp(me->position().x() / sliderW, 0.0, 1.0);
            double pts = ratio * m_duration;

            QImage thumb = m_thumbGen->thumbnailAt(pts);
            if (!thumb.isNull()) {
                // Создаём tooltip label при первом использовании
                if (!m_thumbTooltip) {
                    m_thumbTooltip = new QLabel(parentWidget());
                    m_thumbTooltip->setStyleSheet(
                        "background: rgba(0,0,0,220); border: 1px solid #555; padding: 2px;");
                    m_thumbTooltip->setAlignment(Qt::AlignCenter);
                    m_thumbTooltip->hide();
                }

                m_thumbTooltip->setPixmap(QPixmap::fromImage(thumb));
                m_thumbTooltip->setFixedSize(thumb.width() + 4, thumb.height() + 4);

                // Позиционируем над слайдером
                QPoint sliderGlobal = m_slider->mapTo(parentWidget(), QPoint(0, 0));
                int tooltipX = sliderGlobal.x() + static_cast<int>(me->position().x())
                               - m_thumbTooltip->width() / 2;
                int tooltipY = sliderGlobal.y() - m_thumbTooltip->height() - 4;

                // Ограничиваем границами окна
                tooltipX = std::clamp(tooltipX, 0,
                                      parentWidget()->width() - m_thumbTooltip->width());
                tooltipY = std::max(0, tooltipY);

                m_thumbTooltip->move(tooltipX, tooltipY);
                m_thumbTooltip->show();
                m_thumbTooltip->raise();
            }
            return false;
        }

        if (event->type() == QEvent::Leave) {
            if (m_thumbTooltip)
                m_thumbTooltip->hide();
            return false;
        }
    }
    return QWidget::eventFilter(obj, event);
}

// ── Форматирование ──────────────────────────────────────────────────────────
QString TransportPanel::formatTime(double pts)
{
    if (pts < 0.0) pts = 0.0;
    int totalSec = static_cast<int>(pts);
    int min = totalSec / 60;
    int sec = totalSec % 60;
    return QString("%1:%2").arg(min, 2, 10, QChar('0'))
        .arg(sec, 2, 10, QChar('0'));
}