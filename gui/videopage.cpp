#include "videopage.h"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardPaths>
#include <QTextCursor>
#include <QTimer>
#include <QWebSocket>
#include <QVBoxLayout>

#include "shm_binder.h"

#include <cstdio>
#include <unistd.h>

namespace {
constexpr int kMaxLogBytes = 256 * 1024;
}

VideoPage::VideoPage(ShmHeader* hdr_, QWidget* parent, bool listenForApp)
    : QWidget(parent), hdr(hdr_) {
    auto* root = new QVBoxLayout(this);

    auto* top = new QHBoxLayout;
    urlEdit = new QLineEdit(this);
    urlEdit->setPlaceholderText(
        tr("TikTok / Douyin / other link, or a local file. Paste and press Play."));
    playBtn = new QPushButton(tr("Play"), this);
    pasteBtn = new QPushButton(tr("Paste"), this);
    browseBtn = new QPushButton(tr("Browse..."), this);
    grabBtn = new QPushButton(tr("Grab browser video"), this);
    grabBtn->setToolTip(FormatTip(
        tr("Read the playing <video> URL out of a Chrome tab through DevTools and play it here, "
           "where the DLSS5 layer can enhance it.\n"
           "Chrome must be started with --remote-debugging-port=9222. Use the "
           "'Chrome for DLSS5 Video' desktop entry, or quit Chrome and relaunch it with that flag.")));
    pasteBtn->setToolTip(tr("Take the link out of the clipboard."));
    playBtn->setDefault(true);
    top->addWidget(urlEdit, 1);
    top->addWidget(grabBtn);
    top->addWidget(pasteBtn);
    top->addWidget(browseBtn);
    top->addWidget(playBtn);
    root->addLayout(top);

    auto* mid = new QHBoxLayout;
    videoWidget = new QWidget(this);
    videoWidget->setAttribute(Qt::WA_NativeWindow);
    videoWidget->setAttribute(Qt::WA_OpaquePaintEvent);
    videoWidget->setAutoFillBackground(true);
    QPalette vp = videoWidget->palette();
    vp.setColor(QPalette::Window, Qt::black);
    vp.setColor(QPalette::Base, Qt::black);
    videoWidget->setPalette(vp);
    videoWidget->setMinimumSize(320, 568);  // 9:16; mpv letterboxes a landscape source inside it
    videoWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // The wrap centres the video when Portrait caps its width; in Fit mode the video expands.
    auto* videoWrap = new QWidget(this);
    auto* wrapLay = new QHBoxLayout(videoWrap);
    wrapLay->setContentsMargins(0, 0, 0, 0);
    wrapLay->addStretch(1);
    wrapLay->addWidget(videoWidget, 1);
    wrapLay->addStretch(1);
    mid->addWidget(videoWrap, 1);

    auto* side = new QWidget(this);
    side->setFixedWidth(300);
    auto* sideLay = new QVBoxLayout(side);
    sideLay->setContentsMargins(8, 0, 0, 0);

    auto* quick = new QGroupBox(tr("Quick NR"), side);
    auto* qf = new QFormLayout(quick);
    styleCombo = new QComboBox(quick);
    styleCombo->addItem(tr("Default"));
    styleCombo->addItem(tr("Natural"));
    styleCombo->addItem(tr("Cinematic"));
    styleCombo->setToolTip(FormatTip(tr("The model's own processing profile. Latched at feature build.")));
    qf->addRow(tr("Style"), styleCombo);

    intensitySlider = new QSlider(Qt::Horizontal, quick);
    intensitySlider->setRange(0, 400);
    intensitySlider->setToolTip(FormatTip(tr("How hard the model works. Latched at feature build.")));
    qf->addRow(tr("Intensity"), intensitySlider);

    bypassCheck = new QCheckBox(tr("Bypass NR (show original)"), quick);
    bypassCheck->setToolTip(FormatTip(
        tr("Keeps the pass running and shows the clean frame, so the cost is unchanged and only the "
           "picture differs. Instant A/B.")));
    qf->addRow(bypassCheck);

    compareCombo = new QComboBox(quick);
    compareCombo->addItem(tr("Off"));
    compareCombo->addItem(tr("Side by side"));
    compareCombo->addItem(tr("Wipe"));
    compareCombo->setToolTip(FormatTip(
        tr("Shows the pass against itself in the same frame. The wipe cuts one frame and resamples "
           "nothing.")));
    qf->addRow(tr("Compare"), compareCombo);

    splitSlider = new QSlider(Qt::Horizontal, quick);
    splitSlider->setRange(0, 100);
    splitSlider->setValue(50);
    qf->addRow(tr("Split"), splitSlider);

    auto* capRow = new QHBoxLayout;
    captureBtn = new QPushButton(tr("Capture A/B"), quick);
    captureBtn->setToolTip(FormatTip(
        tr("Writes four matched before/after PNG pairs to the state directory.\n"
           "Same frames, same run, one variable.")));
    openCapturesBtn = new QPushButton(tr("Open folder"), quick);
    capRow->addWidget(captureBtn);
    capRow->addWidget(openCapturesBtn);
    qf->addRow(capRow);
    sideLay->addWidget(quick);

    nrLabel = new QLabel(quick);
    nrLabel->setWordWrap(true);
    nrLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    qf->addRow(nrLabel);

    logBtn = new QPushButton(tr("Player log"), quick);
    logBtn->setToolTip(tr("mpv and layer output, for when a source will not play."));
    qf->addRow(logBtn);

    cookiesCheck = new QCheckBox(tr("Use Chrome cookies (Douyin)"), quick);
    cookiesCheck->setToolTip(FormatTip(
        tr("Passes yt-dlp the cookies from your Chrome profile. Douyin rejects anonymous requests.\n"
           "Log into Douyin in Chrome first. Toggling this restarts the player.")));
    qf->addRow(cookiesCheck);
    cookiesFileBtn = new QPushButton(tr("Cookies file..."), quick);
    cookiesFileBtn->setToolTip(FormatTip(
        tr("Netscape cookies.txt exported from your browser, for yt-dlp. Toggling/changing this "
           "restarts the player.")));
    qf->addRow(cookiesFileBtn);

    auto* hint = new QLabel(
        FormatTip(tr("The other tabs tune the same model the games use; changes apply live.\n"
                     "Model resolution above 100% is internal supersampling, not output upscaling.")),
        side);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: rgba(128,128,128,0.9);"));
    sideLay->addWidget(hint);
    sideLay->addStretch(1);
    mid->addWidget(side);
    root->addLayout(mid, 1);

    auto* bottom = new QHBoxLayout;
    pauseBtn = new QPushButton(tr("Pause"), this);
    pauseBtn->setEnabled(false);
    seekSlider = new QSlider(Qt::Horizontal, this);
    seekSlider->setRange(0, 100000);
    seekSlider->setEnabled(false);
    timeLabel = new QLabel(QStringLiteral("--:-- / --:--"), this);
    loopCheck = new QCheckBox(tr("Loop"), this);
    loopCheck->setChecked(true);
    loopCheck->setToolTip(tr("TikTok and Douyin clips are short; this restarts them at the end."));
    portraitCheck = new QCheckBox(tr("Portrait 9:16"), this);
    portraitCheck->setToolTip(tr("Cap the preview to a phone-shaped area, the way TikTok/Douyin frame their video."));
    onTopCheck = new QCheckBox(tr("On top"), this);
    onTopCheck->setToolTip(tr("Keep this player above the settings window so you can see changes while tuning."));
    auto* volLabel = new QLabel(tr("Vol"), this);
    volumeSlider = new QSlider(Qt::Horizontal, this);
    volumeSlider->setRange(0, 100);
    volumeSlider->setValue(80);
    volumeSlider->setMaximumWidth(120);
    bottom->addWidget(pauseBtn);
    bottom->addWidget(seekSlider, 1);
    bottom->addWidget(timeLabel);
    bottom->addWidget(loopCheck);
    bottom->addWidget(portraitCheck);
    bottom->addWidget(onTopCheck);
    bottom->addWidget(volLabel);
    bottom->addWidget(volumeSlider);
    root->addLayout(bottom);

    statusLabel = new QLabel(
        tr("Paste a link and press Play. The player is mpv; its Vulkan output is what the layer "
           "enhances, exactly as it is for a game."), this);
    statusLabel->setWordWrap(true);
    root->addWidget(statusLabel);

    titleLabel = new QLabel(this);
    titleLabel->setTextFormat(Qt::RichText);
    titleLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    titleLabel->setVisible(false);
    root->addWidget(titleLabel);

    connect(playBtn, &QPushButton::clicked, this, &VideoPage::playClicked);
    connect(urlEdit, &QLineEdit::returnPressed, this, &VideoPage::playClicked);
    connect(pasteBtn, &QPushButton::clicked, this, &VideoPage::pasteClicked);
    connect(browseBtn, &QPushButton::clicked, this, &VideoPage::browse);
    connect(pauseBtn, &QPushButton::clicked, this, &VideoPage::togglePause);
    connect(seekSlider, &QSlider::sliderMoved, this, &VideoPage::seek);
    connect(volumeSlider, &QSlider::valueChanged, this, &VideoPage::volumeChanged);
    connect(loopCheck, &QCheckBox::toggled, this, &VideoPage::loopToggled);
    connect(captureBtn, &QPushButton::clicked, this, &VideoPage::captureClicked);
    connect(openCapturesBtn, &QPushButton::clicked, this, &VideoPage::openCaptures);
    connect(logBtn, &QPushButton::clicked, this, &VideoPage::showLog);

    connect(styleCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &VideoPage::quickStyle);
    connect(intensitySlider, &QSlider::valueChanged, this, [this](int v) { quickIntensity(v / 100.0); });
    connect(bypassCheck, &QCheckBox::toggled, this, &VideoPage::quickBypass);
    connect(compareCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &VideoPage::quickCompare);
    connect(splitSlider, &QSlider::valueChanged, this, &VideoPage::quickSplit);
    connect(portraitCheck, &QCheckBox::toggled, this, &VideoPage::portraitToggled);
    connect(cookiesCheck, &QCheckBox::toggled, this, &VideoPage::cookiesToggled);
    connect(cookiesFileBtn, &QPushButton::clicked, this, &VideoPage::chooseCookiesFile);
    connect(grabBtn, &QPushButton::clicked, this, &VideoPage::grabFromBrowser);
    connect(onTopCheck, &QCheckBox::toggled, this, &VideoPage::toggleOnTop);

    // The browser extension's native host hands video URLs here. One socket per uid, so the
    // extension does not need to know the player's pid.
    if (listenForApp) {
        const QString appSock = QDir::tempPath() + QStringLiteral("/dlssnr-video/app-%1.sock")
                                    .arg(quint64(getuid()));
        QDir().mkpath(QFileInfo(appSock).absolutePath());
        QLocalServer::removeServer(appSock);
        appServer = new QLocalServer(this);
        appServer->listen(appSock);
        connect(appServer, &QLocalServer::newConnection, this, &VideoPage::onAppConnection);
    }

    statusTimer = new QTimer(this);
    statusTimer->setInterval(500);
    connect(statusTimer, &QTimer::timeout, this, &VideoPage::pollStatus);
    statusTimer->start();
    updateQuickFromHeader();
    pollStatus();
}

VideoPage::~VideoPage() {
    closing = true;
    if (statusTimer) statusTimer->stop();
    if (ipcTimer) ipcTimer->stop();
    if (appServer) {
        const QString name = appServer->serverName();
        appServer->close();
        QLocalServer::removeServer(name);
    }
    killMpv();
}

void VideoPage::killMpv() {
    if (ipc) {
        ipc->disconnect();
        ipc->close();
        delete ipc;
        ipc = nullptr;
    }
    if (mpv) {
        mpv->disconnect();
        mpv->kill();
        mpv->waitForFinished(1000);
        // onMpvFinished() may have nulled the pointer and posted a deferred delete during the wait;
        // otherwise this is the delete that reaps the child.
        if (mpv) {
            delete mpv;
            mpv = nullptr;
        }
    }
    pauseBtn->setEnabled(false);
    seekSlider->setEnabled(false);
    setPausedButton(false);
}

void VideoPage::play(const QString& source) {
    QString src = extractUrl(source);
    if (src.isEmpty()) {
        const QFileInfo fi(source);
        if (fi.exists()) src = fi.absoluteFilePath();
    }
    if (src.isEmpty()) {
        setStatus(tr("That is neither an existing file nor a link."), true);
        return;
    }
    currentSource = src;
    pendingSource = src;

    if (!mpv) {
        setStatus(tr("Starting player..."));
        startMpv();
    } else if (ipc && ipc->state() == QLocalSocket::ConnectedState) {
        setStatus(tr("Loading..."));
        sendCommand(QJsonArray{ "loadfile", src });
        pendingSource.clear();
    } else {
        setStatus(tr("Connecting player..."));
    }
}

void VideoPage::stopMpv() {
    if (ipc) {
        if (ipc->state() == QLocalSocket::ConnectedState) sendCommand(QJsonArray{ "quit" });
        ipc->disconnect();
        ipc->close();
        ipc->deleteLater();
        ipc = nullptr;
    }
    if (mpv) {
        // No waitForFinished() here: it runs a nested event loop, and the deferred deletion posted
        // by onMpvFinished() can be delivered inside it, leaving the QProcess in a state that the
        // next deleteLater() trips over during application shutdown.
        mpv->disconnect();
        mpv->terminate();
        mpv->kill();
        mpv->deleteLater();
        mpv = nullptr;
    }
    pauseBtn->setEnabled(false);
    seekSlider->setEnabled(false);
    setPausedButton(false);
}

void VideoPage::startMpv() {
    if (mpv) return;
    if (QStandardPaths::findExecutable("mpv").isEmpty()) {
        setStatus(tr("mpv is not installed. Install it (sudo apt install mpv), then play again."), true);
        return;
    }

    const QString dir = QDir::tempPath() + QStringLiteral("/dlssnr-video");
    QDir().mkpath(dir);
    socketPath = dir + QStringLiteral("/mpv-%1.sock").arg(QCoreApplication::applicationPid());
    QFile::remove(socketPath);

    mpv = new QProcess(this);
    mpv->setProcessChannelMode(QProcess::SeparateChannels);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // mpv's --wid and the layer's X11 surface need the X11 display, not Wayland.
    env.remove(QStringLiteral("WAYLAND_DISPLAY"));
    env.insert(QStringLiteral("VKLayer_DLSS5"), QStringLiteral("1"));
    env.insert(QStringLiteral("VK_INSTANCE_LAYERS"), QStringLiteral("VK_LAYER_NV_dlssnr"));
    mpv->setProcessEnvironment(env);

    // The video widget owns the native window mpv renders into. Force it now: winId() creates the
    // X11 window even before the page is first shown.
    const WId wid = videoWidget->winId();

    QStringList ytdlOpts;
    // YouTube's extractor needs a JS runtime now; node is installed and yt-dlp picks it up here.
    ytdlOpts << QStringLiteral("js-runtimes=node");
    if (useChromeCookies) ytdlOpts << QStringLiteral("cookies-from-browser=chrome");
    if (!cookiesFile.isEmpty())
        ytdlOpts << QStringLiteral("cookies=%1").arg(cookiesFile);

    QStringList args{
        QStringLiteral("--no-config"),
        QStringLiteral("--idle=yes"),
        QStringLiteral("--force-window=yes"),
        QStringLiteral("--wid=%1").arg(QString::number(quintptr(wid))),
        QStringLiteral("--vo=gpu-next"),
        QStringLiteral("--gpu-api=vulkan"),
        QStringLiteral("--gpu-context=x11vk"),
        QStringLiteral("--hwdec=auto-safe"),
        QStringLiteral("--osd-level=0"),
        QStringLiteral("--sub-visibility=no"),
        QStringLiteral("--loop-file=inf"),
        QStringLiteral("--keep-open=no"),
        QStringLiteral("--input-ipc-server=%1").arg(socketPath),
        QStringLiteral("--msg-level=all=warn"),
        QStringLiteral("--audio-display=no"),
    };
    if (!ytdlOpts.isEmpty())
        args << QStringLiteral("--ytdl-raw-options=%1").arg(ytdlOpts.join(QLatin1Char(',')));
    if (!httpHeaders.isEmpty())
        args << QStringLiteral("--http-header-fields=%1").arg(httpHeaders);
    connect(mpv, &QProcess::readyReadStandardError, this, &VideoPage::onMpvReadyRead);
    connect(mpv, &QProcess::readyReadStandardOutput, this, &VideoPage::onMpvReadyRead);
    connect(mpv, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            &VideoPage::onMpvFinished);

    mpv->start(QStringLiteral("mpv"), args);
    if (!mpv->waitForStarted(5000)) {
        setStatus(tr("mpv failed to start: %1").arg(mpv->errorString()), true);
        mpv->deleteLater();
        mpv = nullptr;
        return;
    }
    setStatus(tr("Player started; loading..."));
    pauseBtn->setEnabled(true);
    seekSlider->setEnabled(true);

    ipcTimer = new QTimer(this);
    ipcTimer->setInterval(150);
    connect(ipcTimer, &QTimer::timeout, this, &VideoPage::connectIpc);
    ipcTimer->start();
    connectIpc();
}

void VideoPage::connectIpc() {
    if (!mpv) return;
    if (ipc && (ipc->state() == QLocalSocket::ConnectedState ||
                ipc->state() == QLocalSocket::ConnectingState))
        return;

    if (ipc) {
        ipc->deleteLater();
        ipc = nullptr;
    }
    ipc = new QLocalSocket(this);
    connect(ipc, &QLocalSocket::connected, this, &VideoPage::onIpcConnected);
    connect(ipc, &QLocalSocket::readyRead, this, &VideoPage::onIpcReadyRead);
    connect(ipc, &QLocalSocket::errorOccurred, this, &VideoPage::onIpcError);
    ipc->connectToServer(socketPath, QIODevice::ReadWrite);
    if (ipc->waitForConnected(200)) onIpcConnected();
}

void VideoPage::onIpcConnected() {
    if (!ipc || ipc->state() != QLocalSocket::ConnectedState) return;
    if (ipcTimer) ipcTimer->stop();
    static const struct { const char* name; int id; } props[] = {
        { "time-pos", 1 }, { "duration", 2 }, { "pause", 3 }, { "volume", 4 },
        { "media-title", 5 }, { "eof-reached", 6 }, { "idle-active", 7 },
    };
    for (const auto& p : props) observeProperty(QString::fromUtf8(p.name), p.id);
    if (!pendingSource.isEmpty()) {
        const QString src = pendingSource;
        pendingSource.clear();
        setStatus(tr("Loading..."));
        sendCommand(QJsonArray{ "loadfile", src });
    }
}

void VideoPage::onIpcReadyRead() {
    if (!ipc) return;
    ipcBuffer.append(ipc->readAll());
    int nl;
    while ((nl = ipcBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = ipcBuffer.left(nl).trimmed();
        ipcBuffer.remove(0, nl + 1);
        if (line.isEmpty()) continue;
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) continue;
        const QJsonObject o = doc.object();
        if (o.value(QStringLiteral("event")).toString() != QLatin1String("property-change"))
            continue;
        const QString name = o.value(QStringLiteral("name")).toString();
        const QJsonValue data = o.value(QStringLiteral("data"));
        if (name == QLatin1String("time-pos")) {
            positionSec = data.toDouble();
            if (!userSeeking) {
                const QSignalBlocker b(seekSlider);
                seekSlider->setValue(int(positionSec * 1000));
            }
        } else if (name == QLatin1String("duration")) {
            durationSec = data.toDouble();
            const QSignalBlocker b(seekSlider);
            seekSlider->setRange(0, qMax(1, int(durationSec * 1000)));
        } else if (name == QLatin1String("pause")) {
            observedPaused = data.toBool();
            setPausedButton(observedPaused);
        } else if (name == QLatin1String("volume")) {
            observedVolume = int(data.toDouble());
            const QSignalBlocker b(volumeSlider);
            volumeSlider->setValue(observedVolume);
        } else if (name == QLatin1String("media-title")) {
            const QString t = data.toString();
            if (!t.isEmpty()) {
                titleLabel->setText(t);
                titleLabel->setVisible(true);
            }
        }
    }
}

void VideoPage::onIpcError(QLocalSocket::LocalSocketError error) {
    if (error == QLocalSocket::SocketTimeoutError) return;
    // A connection refused is just the socket not being there yet; the timer retries.
    if (error == QLocalSocket::ConnectionRefusedError ||
        error == QLocalSocket::ServerNotFoundError)
        return;
}

void VideoPage::sendCommand(const QJsonArray& command) {
    if (!ipc || ipc->state() != QLocalSocket::ConnectedState) return;
    QJsonObject o;
    o.insert(QStringLiteral("command"), command);
    o.insert(QStringLiteral("request_id"), nextRequestId++);
    ipc->write(QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n');
}

void VideoPage::observeProperty(const QString& property, int id) {
    sendCommand(QJsonArray{ "observe_property", id, property });
}

void VideoPage::onMpvReadyRead() {
    if (!mpv) return;
    const QByteArray err = mpv->readAllStandardError();
    const QByteArray out = mpv->readAllStandardOutput();
    mpvLog.append(err);
    mpvLog.append(out);
    if (mpvLog.size() > kMaxLogBytes) mpvLog.remove(0, mpvLog.size() - kMaxLogBytes);
    // A little of the layer/mpv chatter on stderr: this is the only place the embedded player's
    // output is visible when the app is launched from a menu.
    QByteArray all = err + out;
    int at = 0;
    while ((at = all.indexOf('\n', at)) >= 0) {
        const QByteArray line = all.left(at).trimmed();
        all.remove(0, at + 1);
        at = 0;
        if (line.contains("[dlssnr-layer]") || line.contains("VO:") || line.contains("ERROR") ||
            line.contains("Failed"))
            qInfo().noquote() << "mpv|" << QString::fromUtf8(line);
    }
}

void VideoPage::onMpvFinished(int exitCode, QProcess::ExitStatus status) {
    Q_UNUSED(status);
    if (closing) return;
    if (ipc) {
        ipc->deleteLater();
        ipc = nullptr;
    }
    if (mpv) {
        mpv->deleteLater();
        mpv = nullptr;
    }
    pendingSource.clear();
    setStatus(tr("Player exited (code %1). Press Play to restart it.").arg(exitCode));
    pauseBtn->setEnabled(false);
    seekSlider->setEnabled(false);
}

void VideoPage::browse() {
    const QString f = QFileDialog::getOpenFileName(this, tr("Choose a video"), QDir::homePath());
    if (!f.isEmpty()) {
        urlEdit->setText(f);
        play(f);
    }
}

void VideoPage::playClicked() {
    play(urlEdit->text());
}

void VideoPage::pasteClicked() {
    const QString text = QGuiApplication::clipboard()->text();
    const QString url = extractUrl(text);
    if (!url.isEmpty()) {
        urlEdit->setText(url);
        play(url);
    } else if (!text.trimmed().isEmpty()) {
        urlEdit->setText(text.trimmed());
    }
}

void VideoPage::togglePause() {
    sendCommand(QJsonArray{ "cycle", "pause" });
}

void VideoPage::seek(int ms) {
    userSeeking = true;
    sendCommand(QJsonArray{ "seek", double(ms) / 1000.0, "absolute" });
    QTimer::singleShot(300, this, [this] { userSeeking = false; });
}

void VideoPage::volumeChanged(int percent) {
    sendCommand(QJsonArray{ "set_property", "volume", double(percent) });
}

void VideoPage::loopToggled(bool on) {
    sendCommand(QJsonArray{ "set_property", "loop-file", on ? "inf" : "no" });
}

void VideoPage::captureClicked() {
    if (!hdr) return;
    hdr->captureRequest.store(4);
    hdr->controlSeq.fetch_add(1);
    setStatus(tr("Capture requested: four before/after pairs to %1").arg(captureDir()));
}

void VideoPage::openCaptures() {
    QDir().mkpath(captureDir());
    QDesktopServices::openUrl(QUrl::fromLocalFile(captureDir()));
}

void VideoPage::showLog() {
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Player log"));
    dlg.resize(760, 460);
    auto* lay = new QVBoxLayout(&dlg);
    auto* text = new QPlainTextEdit(&dlg);
    text->setReadOnly(true);
    text->setPlainText(QString::fromUtf8(mpvLog));
    text->moveCursor(QTextCursor::End);
    lay->addWidget(text);
    auto* close = new QPushButton(tr("Close"), &dlg);
    connect(close, &QPushButton::clicked, &dlg, &QDialog::accept);
    lay->addWidget(close, 0, Qt::AlignRight);
    dlg.exec();
}

void VideoPage::quickStyle(int index) {
    if (!hdr) return;
    hdr->style.store(uint32_t(qBound(0, index, 2)));
    hdr->controlSeq.fetch_add(1);
    hdr->tuningSeq.fetch_add(1);
    if (reloadSettings) reloadSettings();
    emit settingsChanged();
}

void VideoPage::quickIntensity(double value) {
    if (!hdr) return;
    hdr->intensityBits.store(FloatToBits(float(qBound(0.0, value, 4.0))));
    hdr->controlSeq.fetch_add(1);
    hdr->tuningSeq.fetch_add(1);
    if (reloadSettings) reloadSettings();
    emit settingsChanged();
}

void VideoPage::quickBypass(bool on) {
    if (!hdr) return;
    hdr->applyModel.store(on ? 1u : 0u);
    hdr->controlSeq.fetch_add(1);
    emit settingsChanged();
}

void VideoPage::quickCompare(int index) {
    if (!hdr) return;
    hdr->compareMode.store(uint32_t(qBound(0, index, 2)));
    hdr->controlSeq.fetch_add(1);
    emit settingsChanged();
}

void VideoPage::quickSplit(int value) {
    if (!hdr) return;
    hdr->compareSplitBits.store(FloatToBits(float(qBound(0, value, 100)) / 100.0f));
    hdr->controlSeq.fetch_add(1);
    emit settingsChanged();
}

void VideoPage::portraitToggled(bool on) {
    if (on) {
        videoWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
        videoWidget->setMinimumWidth(340);
        videoWidget->setMaximumWidth(420);
    } else {
        videoWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        videoWidget->setMinimumWidth(320);
        videoWidget->setMaximumWidth(QWIDGETSIZE_MAX);
    }
    videoWidget->updateGeometry();
}

void VideoPage::cookiesToggled(bool on) {
    useChromeCookies = on;
    if (mpv && !currentSource.isEmpty()) {
        pendingSource = currentSource;
        stopMpv();
        startMpv();
    }
}

void VideoPage::chooseCookiesFile() {
    const QString f = QFileDialog::getOpenFileName(this, tr("yt-dlp cookies.txt"), QDir::homePath(),
                                                   tr("Cookies file (*.txt);;All files (*)"));
    if (f.isEmpty()) return;
    cookiesFile = f;
    cookiesFileBtn->setText(tr("Cookies: %1").arg(QFileInfo(f).fileName()));
    cookiesFileBtn->setToolTip(f);
    if (mpv && !currentSource.isEmpty()) {
        pendingSource = currentSource;
        stopMpv();
        startMpv();
    }
}

void VideoPage::grabFromBrowser() {
    auto* nam = new QNetworkAccessManager(this);
    QNetworkRequest req(QUrl(QStringLiteral("http://127.0.0.1:9222/json/list")));
    setStatus(tr("Looking for Chrome's debugging port..."));
    QNetworkReply* reply = nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, nam] {
        const QByteArray body = reply->readAll();
        const QNetworkReply::NetworkError err = reply->error();
        reply->deleteLater();
        nam->deleteLater();
        if (err != QNetworkReply::NoError) {
            setStatus(tr("Chrome is not listening on :9222. Quit it and start it with "
                         "--remote-debugging-port=9222 (the 'Chrome for DLSS5 Video' entry does "
                         "that), then try again."), true);
            return;
        }
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isArray()) {
            setStatus(tr("Chrome's DevTools returned something unexpected."), true);
            return;
        }
        QStringList labels, sockets;
        for (const QJsonValue& v : doc.array()) {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("type")).toString() != QLatin1String("page")) continue;
            const QString u = o.value(QStringLiteral("url")).toString();
            if (!u.startsWith(QStringLiteral("http"), Qt::CaseInsensitive)) continue;
            labels << o.value(QStringLiteral("title")).toString().left(60) + QStringLiteral(" — ")
                      + u.left(90);
            sockets << o.value(QStringLiteral("webSocketDebuggerUrl")).toString();
        }
        if (sockets.isEmpty()) {
            setStatus(tr("No web page is open in that Chrome instance."), true);
            return;
        }
        bool ok = false;
        const QString picked = QInputDialog::getItem(this, tr("Browser tab"),
                                                     tr("Which tab is playing the video?"),
                                                     labels, 0, false, &ok);
        if (!ok || picked.isEmpty()) return;
        const int idx = labels.indexOf(picked);
        if (idx < 0 || idx >= sockets.size()) return;
        connectBrowserWs(sockets[idx]);
    });
}

void VideoPage::connectBrowserWs(const QString& wsUrl) {
    struct NetReq { QString url, referer, ua, cookie; };
    struct State {
        QVector<NetReq> media;
        bool done = false;
    };
    auto* st = new State;
    auto* ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);

    const auto finish = [this, ws, st](const NetReq& pick) {
        if (st->done || pick.url.isEmpty()) return;
        st->done = true;
        QStringList h;
        if (!pick.referer.isEmpty()) h << QStringLiteral("Referer: ") + pick.referer;
        if (!pick.ua.isEmpty()) h << QStringLiteral("User-Agent: ") + pick.ua;
        if (!pick.cookie.isEmpty()) h << QStringLiteral("Cookie: ") + pick.cookie;
        httpHeaders = h.join(QStringLiteral(", "));
        urlEdit->setText(pick.url);
        setStatus(tr("Grabbed %1; loading with the page's request headers.").arg(pick.url.left(100)));
        ws->close();
        ws->deleteLater();
        delete st;
        // The headers are baked into the mpv command line, so restart the player if it is up.
        if (mpv && !currentSource.isEmpty()) {
            pendingSource = pick.url;
            stopMpv();
            startMpv();
        } else {
            play(pick.url);
        }
    };

    connect(ws, &QWebSocket::connected, ws, [ws] {
        const auto send = [ws](int id, const QString& method, const QJsonObject& params = {}) {
            QJsonObject o;
            o.insert(QStringLiteral("id"), id);
            o.insert(QStringLiteral("method"), method);
            if (!params.isEmpty()) o.insert(QStringLiteral("params"), params);
            ws->sendTextMessage(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
        };
        send(1, QStringLiteral("Runtime.enable"));
        send(2, QStringLiteral("Network.enable"));
        QJsonObject p;
        p.insert(QStringLiteral("expression"),
                 QStringLiteral("(() => { const vs=[...document.querySelectorAll('video')]"
                                 ".filter(v => v.currentSrc || v.src);"
                                 "if(!vs.length) return null;"
                                 "const v=vs.sort((a,b)=>(b.videoWidth*b.videoHeight)-(a.videoWidth*a.videoHeight))[0];"
                                 "return {src:v.currentSrc||v.src, w:v.videoWidth, h:v.videoHeight,"
                                 "title:document.title}; })()"));
        p.insert(QStringLiteral("returnByValue"), true);
        send(3, QStringLiteral("Runtime.evaluate"), p);
    });

    connect(ws, &QWebSocket::textMessageReceived, ws, [this, ws, st, finish](const QString& text) {
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isObject()) return;
        const QJsonObject o = doc.object();

        if (o.value(QStringLiteral("id")).toInt() == 3) {
            const QJsonValue val = o.value(QStringLiteral("result"))
                                     .toObject().value(QStringLiteral("result"))
                                     .toObject().value(QStringLiteral("value"));
            if (val.isObject()) {
                const QString src = val.toObject().value(QStringLiteral("src")).toString();
                if (!src.isEmpty() && !src.startsWith(QStringLiteral("blob:"), Qt::CaseInsensitive)) {
                    NetReq pick;
                    pick.url = src;
                    pick.referer = ws->request().url().toString();
                    finish(pick);
                    return;
                }
                setStatus(tr("The tab uses Media Source Extensions; watching its network "
                             "requests for the stream URL..."));
            }
        }

        if (o.value(QStringLiteral("method")).toString() == QLatin1String("Network.requestWillBeSent")) {
            const QJsonObject req = o.value(QStringLiteral("params"))
                                      .toObject().value(QStringLiteral("request")).toObject();
            const QString url = req.value(QStringLiteral("url")).toString();
            const QString type = o.value(QStringLiteral("params")).toObject()
                                   .value(QStringLiteral("type")).toString();
            const bool media = type == QLatin1String("Media") ||
                url.contains(QStringLiteral(".m3u8"), Qt::CaseInsensitive) ||
                url.contains(QStringLiteral(".mp4"), Qt::CaseInsensitive) ||
                url.contains(QStringLiteral(".webm"), Qt::CaseInsensitive) ||
                url.contains(QStringLiteral(".ts?"), Qt::CaseInsensitive);
            if (!media) return;
            NetReq r;
            r.url = url;
            const QJsonObject hdr = req.value(QStringLiteral("headers")).toObject();
            r.referer = hdr.value(QStringLiteral("Referer")).toString();
            r.ua = hdr.value(QStringLiteral("User-Agent")).toString();
            r.cookie = hdr.value(QStringLiteral("Cookie")).toString();
            if (r.referer.isEmpty()) r.referer = ws->request().url().toString();
            st->media.append(r);
            // Prefer a master playlist; a fragment is a poor fallback.
            for (const NetReq& n : st->media)
                if (n.url.contains(QStringLiteral(".m3u8"), Qt::CaseInsensitive)) {
                    finish(n);
                    return;
                }
        }
    });

    connect(ws, &QWebSocket::errorOccurred, ws, [this, ws, st](QAbstractSocket::SocketError) {
        if (st->done) return;
        st->done = true;
        setStatus(tr("Could not attach to that Chrome tab."), true);
        ws->deleteLater();
        delete st;
    });
    QTimer::singleShot(5000, ws, [this, ws, st, finish] {
        if (st->done) return;
        if (!st->media.isEmpty()) finish(st->media.last());
        else {
            st->done = true;
            setStatus(tr("No playable media request was seen in that tab in five seconds."), true);
            ws->close();
            ws->deleteLater();
            delete st;
        }
    });
    ws->open(QUrl(wsUrl));
}

void VideoPage::onAppConnection() {
    while (appServer && appServer->hasPendingConnections()) {
        QLocalSocket* c = appServer->nextPendingConnection();
        connect(c, &QLocalSocket::readyRead, this, [this, c] {
            const QByteArray data = c->readAll().trimmed();
            if (!data.isEmpty()) play(QString::fromUtf8(data));
            c->deleteLater();
        });
    }
}

void VideoPage::toggleOnTop(bool on) {
    Qt::WindowFlags f = windowFlags();
    if (on) f |= Qt::WindowStaysOnTopHint;
    else f &= ~Qt::WindowStaysOnTopHint;
    setWindowFlags(f);
    show();
}

void VideoPage::updateQuickFromHeader() {
    if (!hdr) return;
    {
        const QSignalBlocker b(styleCombo);
        styleCombo->setCurrentIndex(qBound(0, int(hdr->style.load()), 2));
    }
    {
        const QSignalBlocker b(intensitySlider);
        intensitySlider->setValue(qBound(0, int(BitsToFloat(hdr->intensityBits.load()) * 100.0), 400));
    }
    {
        const QSignalBlocker b(bypassCheck);
        bypassCheck->setChecked(hdr->applyModel.load() != 0);
    }
    {
        const QSignalBlocker b(compareCombo);
        compareCombo->setCurrentIndex(qBound(0, int(hdr->compareMode.load()), 2));
    }
    {
        const QSignalBlocker b(splitSlider);
        splitSlider->setValue(qBound(0, int(BitsToFloat(hdr->compareSplitBits.load()) * 100.0), 100));
    }
}

void VideoPage::pollStatus() {
    if (!hdr) return;
    if (ipc && ipc->state() == QLocalSocket::ConnectedState && !pendingSource.isEmpty()) {
        const QString src = pendingSource;
        pendingSource.clear();
        setStatus(tr("Loading..."));
        sendCommand(QJsonArray{ "loadfile", src });
    }

    const auto st = hdr->helperState.load();
    const bool modelUp = hdr->modelUp.load() != 0;
    const uint32_t pid = hdr->layerPid.load();
    const bool attached = pid != 0 && QFileInfo::exists(QStringLiteral("/proc/%1").arg(pid));
    const quint64 frames = (quint64(hdr->layerFramesHi.load()) << 32) | hdr->layerFramesLo.load();
    const float evalMs = BitsToFloat(hdr->helperEvalMsBits.load());

    QString text = tr("NR: ");
    switch (st) {
        case kHelperStarting: text += tr("starting"); break;
        case kHelperNoVulkan: text += tr("no Vulkan device"); break;
        case kHelperNoBinaries: text += tr("no NGX binaries"); break;
        case kHelperModelFailed: text += tr("model failed"); break;
        case kHelperRunning: text += modelUp ? tr("running") : tr("waiting"); break;
        case kHelperStopped: text += tr("stopped"); break;
        default: text += tr("unknown"); break;
    }
    if (attached) text += tr(" | layer attached, %1 frames").arg(frames);
    else text += tr(" | layer not presenting");
    if (evalMs > 0.0f) text += tr(" | eval %1 ms").arg(evalMs, 0, 'f', 1);
    nrLabel->setText(text);

    static int dbg = 0;
    if (dbg < 6) {
        ++dbg;
        qInfo() << "video| widget" << videoWidget->size() << "winId" << quintptr(videoWidget->winId())
                << "layerPid" << pid;
    }

    if (durationSec > 0.0) {
        const int total = int(durationSec * 1000);
        const QSignalBlocker b(seekSlider);
        if (seekSlider->maximum() != qMax(1, total)) seekSlider->setRange(0, qMax(1, total));
        if (!userSeeking) seekSlider->setValue(int(positionSec * 1000));
    }
    auto fmt = [](double sec) {
        const int s = int(sec);
        return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
    };
    timeLabel->setText(QStringLiteral("%1 / %2").arg(fmt(positionSec), fmt(durationSec)));

    updateQuickFromHeader();
}

void VideoPage::setStatus(const QString& text, bool error) {
    statusLabel->setText(text);
    statusLabel->setStyleSheet(error ? QStringLiteral("color:#e06c75;") : QString());
}

void VideoPage::setPausedButton(bool paused) {
    pauseBtn->setText(paused ? tr("Play") : tr("Pause"));
}

QString VideoPage::extractUrl(const QString& text) {
    if (text.trimmed().isEmpty()) return QString();
    // TikTok/Douyin share text often wraps the link in words; take the first http(s) token.
    static const QRegularExpression re(QStringLiteral("https?://[^\\s\"'<>]+"),
                                       QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(text);
    if (m.hasMatch()) return m.captured(0);
    // A bare www. link, or a bare host the share sheet dropped the scheme from.
    static const QRegularExpression host(QStringLiteral("(?:^|\\s)([\\w.-]+\\.(?:com|cn|tv|net|org)(?:[/:][^\\s]*)?)"));
    const QRegularExpressionMatch h = host.match(text);
    if (h.hasMatch()) return QStringLiteral("https://") + h.captured(1);
    return QString();
}

QString VideoPage::captureDir() const {
    QString state = qEnvironmentVariable("XDG_STATE_HOME");
    if (state.isEmpty()) state = QDir::homePath() + QStringLiteral("/.local/state");
    return state + QStringLiteral("/dlssnr/captures");
}
