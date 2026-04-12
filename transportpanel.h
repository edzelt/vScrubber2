#pragma once

#include <QWidget>
#include <QTimer>
#include <QStringList>

class QSlider;
class QLabel;
class QPushButton;
class QListWidget;
class ThumbnailGenerator;

// ── Панель управления (transport bar) ────────────────────────────────────────
//
// Появляется при наведении курсора на нижние 15% экрана.
//
// Логика кнопок Loop / Dir:
//   Loop          — зацикливание текущего файла
//   Dir           — проигрывание файлов каталога до конца
//   Loop + Dir    — зацикливание всего каталога
//
class TransportPanel : public QWidget
{
    Q_OBJECT

public:
    explicit TransportPanel(QWidget* parent = nullptr);

    void setPosition(double pts, double duration, double fps);
    void setKeyframePts(const std::vector<double>& keyframePts);
    void setThumbnailGenerator(ThumbnailGenerator* gen) { m_thumbGen = gen; }

    void showPanel();
    void hidePanel();
    void checkCursorVisibility(const QPointF& cursorPos, int widgetHeight);

    void setCurrentFile(const QString& filePath);
    QString currentFilePath() const { return m_currentFile; }
    bool isLoopFile() const { return m_loopFile; }
    bool isLoopDir()  const { return m_loopDir; }
    bool isFileListVisible() const;

    // Навигация по каталогу (возвращают false если файлов нет)
    bool tryNextFile();
    bool tryPrevFile();
    void goToFirstFile();
    void goToLastFile();

signals:
    void seekRequested(double pts);
    void fileSelected(const QString& path);

public slots:
    void onPrevFile();
    void onNextFile();

protected:
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void onSliderPressed();
    void onSliderMoved(int value);
    void onSliderReleased();
    void onHideTimer();
    void onToggleLoopFile();
    void onToggleLoopDir();
    void onShowFileList();

private:
    void buildUI();
    void updateTimecodeLabel();
    void loadDirectoryFiles(const QString& filePath);
    static QString formatTime(double pts);

    // ── Виджеты ──────────────────────────────────────────────────────────────
    QSlider*     m_slider       = nullptr;
    QLabel*      m_timeLabel    = nullptr;
    QPushButton* m_btnPrev      = nullptr;
    QPushButton* m_btnNext      = nullptr;
    QPushButton* m_btnLoopFile  = nullptr;
    QPushButton* m_btnLoopDir   = nullptr;
    QPushButton* m_btnFileList  = nullptr;

    // Popup список файлов (child родительского виджета)
    QListWidget* m_fileListWgt  = nullptr;

    // Превью при наведении на таймлайн
    QLabel*      m_thumbTooltip = nullptr;  // child родительского виджета
    ThumbnailGenerator* m_thumbGen = nullptr;  // не владеет

    // ── Состояние ────────────────────────────────────────────────────────────
    double m_currentPts     = 0.0;
    double m_duration       = 0.0;
    double m_fps            = 25.0;
    bool   m_loopFile       = false;
    bool   m_loopDir        = false;
    bool   m_sliderDragging = false;
    bool   m_panelVisible   = false;

    std::vector<double> m_keyframePts;

    QStringList m_dirFiles;
    QString     m_currentFile;
    int         m_currentFileIdx = -1;

    QTimer m_hideTimer;
    static constexpr int HIDE_DELAY_MS = 2000;
};