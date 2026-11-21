#pragma once
// The Video tab: a streaming/local source played by an embedded mpv, whose Vulkan swapchain the
// DLSS5 layer intercepts and enhances. The same settings header and helper as games, so the picture
// is the game picture; this page adds a source, transport controls, and the live A/B controls.
//
// mpv is embedded with --wid rather than a GStreamer sink: vulkansink opens its own window (its
// GstVideoOverlay vfunc for X11 is a stub), while mpv's X11 Vulkan output renders into a window the
// Qt side owns. The app therefore runs under X11 in video mode; the desktop entry and --video mode
// set QT_QPA_PLATFORM=xcb and clear WAYLAND_DISPLAY.

#include <QWidget>
#include <QString>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QPointer>
#include <functional>

#include "../common/shm_protocol.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QTimer;

class VideoPage : public QWidget {
    Q_OBJECT
public:
    explicit VideoPage(ShmHeader* hdr, QWidget* parent = nullptr, bool listenForApp = false);
    ~VideoPage() override;

    // Start (or load into a running player) a URL, a local path, or clipboard share text containing
    // either. Public because --video <url> hands it the command line.
    void play(const QString& source);
    void stopMpv();
    void killMpv();

    // Called after a quick control writes the header, so the full settings tabs read the same values.
    void setSettingsReloadCallback(std::function<void()> cb) { reloadSettings = cb; }

signals:
    void settingsChanged();

private slots:
    void browse();
    void playClicked();
    void pasteClicked();
    void togglePause();
    void seek(int ms);
    void volumeChanged(int percent);
    void loopToggled(bool on);
    void captureClicked();
    void openCaptures();
    void showLog();
    void quickStyle(int index);
    void quickIntensity(double value);
    void quickBypass(bool on);
    void quickCompare(int index);
    void quickSplit(int value);
    void portraitToggled(bool on);
    void cookiesToggled(bool on);
    void chooseCookiesFile();
    void grabFromBrowser();
    void toggleOnTop(bool on);
    void onAppConnection();
    void pollStatus();
    void connectIpc();
    void onIpcConnected();
    void onIpcReadyRead();
    void onIpcError(QLocalSocket::LocalSocketError error);
    void onMpvReadyRead();
    void onMpvFinished(int exitCode, QProcess::ExitStatus status);

private:
    void startMpv();
    void sendCommand(const QJsonArray& command);
    void observeProperty(const QString& property, int id);
    void setStatus(const QString& text, bool error = false);
    void connectBrowserWs(const QString& wsUrl);
    void updateQuickFromHeader();
    QString captureDir() const;
    static QString extractUrl(const QString& text);
    void setPausedButton(bool paused);

    ShmHeader* hdr = nullptr;
    std::function<void()> reloadSettings;

    QLineEdit* urlEdit = nullptr;
    QPushButton* playBtn = nullptr;
    QPushButton* browseBtn = nullptr;
    QPushButton* pasteBtn = nullptr;
    QLabel* statusLabel = nullptr;
    QLabel* titleLabel = nullptr;
    QLabel* timeLabel = nullptr;
    QLabel* nrLabel = nullptr;
    QWidget* videoWidget = nullptr;

    QPushButton* pauseBtn = nullptr;
    QSlider* seekSlider = nullptr;
    QSlider* volumeSlider = nullptr;
    QCheckBox* loopCheck = nullptr;

    QComboBox* styleCombo = nullptr;
    QSlider* intensitySlider = nullptr;
    QCheckBox* bypassCheck = nullptr;
    QComboBox* compareCombo = nullptr;
    QSlider* splitSlider = nullptr;
    QPushButton* captureBtn = nullptr;
    QPushButton* openCapturesBtn = nullptr;
    QPushButton* logBtn = nullptr;
    QCheckBox* portraitCheck = nullptr;
    QCheckBox* onTopCheck = nullptr;
    QPushButton* grabBtn = nullptr;
    QCheckBox* cookiesCheck = nullptr;
    QPushButton* cookiesFileBtn = nullptr;
    QString cookiesFile;
    QString httpHeaders;
    bool useChromeCookies = false;

    // QPointer: waitForFinished() runs a nested event loop that can deliver the deferred deletion
    // posted by onMpvFinished(), leaving a raw pointer dangling by the time stopMpv() resumes.
    QLocalServer* appServer = nullptr;
    QPointer<QProcess> mpv;
    QPointer<QLocalSocket> ipc;
    QString socketPath;
    QString pendingSource;
    QString currentSource;
    QByteArray mpvLog;
    QByteArray ipcBuffer;
    int nextRequestId = 100;
    bool userSeeking = false;
    bool closing = false;
    double positionSec = 0.0;
    double durationSec = 0.0;
    int observedVolume = 80;
    bool observedPaused = false;
    QTimer* statusTimer = nullptr;
    QTimer* ipcTimer = nullptr;
};
