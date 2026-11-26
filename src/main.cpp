// omaci — the builds of one repository, live: how far, how long still, and what failed.

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include "backend.h"
#include "github.h"
#include "theme.h"

// One window, however often the bar widget asks for one: a second launch
// hands its repository and commit to the window that is already open.
static QString socketName() {
    return QString("omaci-%1").arg(qEnvironmentVariable("USER", "user"));
}

static bool handOver(const QString &repo, const QString &sha) {
    QLocalSocket socket;
    socket.connectToServer(socketName());
    if (!socket.waitForConnected(300))
        return false;
    socket.write((repo + '\n' + sha + '\n').toUtf8());
    socket.waitForBytesWritten(300);
    socket.disconnectFromServer();
    return true;
}

// The repository of the checkout omaci is started in: its origin on
// GitHub, or else the first remote that is on GitHub. Empty outside one.
static QString repoOfDirectory() {
    QProcess git;
    git.start("git", {"config", "--get-regexp", R"(^remote\..*\.url$)"});
    if (!git.waitForFinished(2000) || git.exitStatus() != QProcess::NormalExit || git.exitCode() != 0)
        return {};
    QString first;
    for (const auto &line : QString::fromUtf8(git.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts)) {
        const auto repo = GitHub::repoFromRemote(line.section(' ', 1));
        if (repo.isEmpty())
            continue;
        if (line.section(' ', 0, 0) == "remote.origin.url")
            return repo;
        if (first.isEmpty())
            first = repo;
    }
    return first;
}

static void raise(QQuickWindow *window) {
    window->show();
    window->raise();
    window->requestActivate();
    // Wayland doesn't let an app take focus on its own; on Hyprland the
    // compositor can be asked to do it.
    if (!QStandardPaths::findExecutable("hyprctl").isEmpty())
        QProcess::startDetached("hyprctl", {"dispatch", "focuswindow", "class:^(omaci)$"});
}

// Draws the window into a file without ever showing it: walks through the
// scene's steps (another build, another job, the help), lets the window
// settle, and saves what it looks like.
static void screenshot(QQuickWindow *window, Backend *backend, const QStringList &scene, const QString &file) {
    auto *steps = new QStringList(scene);
    auto *timer = new QTimer(window);
    timer->setInterval(700);
    QObject::connect(timer, &QTimer::timeout, window, [=] {
        if (steps->isEmpty()) {
            timer->stop();
            QTimer::singleShot(1300, window, [=] {
                const bool saved = window->grabWindow().save(file);
                if (!saved)
                    qWarning("Couldn't save the screenshot to %s", qPrintable(file));
                QCoreApplication::exit(saved ? 0 : 1);
            });
            return;
        }
        const auto step = steps->takeFirst();
        if (step == "build+")
            backend->moveBuild(1);
        else if (step == "build-")
            backend->moveBuild(-1);
        else if (step == "job+")
            backend->moveJob(1);
        else if (step == "job-")
            backend->moveJob(-1);
        else if (step == "skipped")
            backend->toggleSkipped();
        else if (step == "branch")
            backend->toggleBranch();
        else if (step == "main")
            backend->toggleMain();
        else if (step == "running")
            backend->toggleRunning();
        else if (step != "wait")
            QMetaObject::invokeMethod(window, "scene", Q_ARG(QVariant, step));
    });
    timer->start();
}

int main(int argc, char *argv[]) {
    // A screenshot is drawn offscreen, whatever the session is: it must
    // never put a window on the desktop.
    for (int i = 1; i < argc; ++i) {
        if (QByteArray(argv[i]).startsWith("--screenshot")) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
            if (qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND"))
                qputenv("QT_QUICK_BACKEND", "software");
        }
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName("omaci");
    app.setApplicationVersion("0.4.1");

    // Matches the window to omaci.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon.
    app.setDesktopFileName("omaci");
    app.setWindowIcon(QIcon::fromTheme("omaci"));

    QCommandLineParser parser;
    parser.setApplicationDescription("The builds of one repository, live: how far, how long still, and what failed.");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("repo", "The repository to watch, as owner/name. Without one: the repository of "
                                         "the current directory, or else the one watched last, or else a menu of yours.");
    QCommandLineOption shaOption("sha", "Open on the build of this commit.", "sha");
    parser.addOption(shaOption);
    QCommandLineOption demoOption("demo", "Show invented builds instead of asking GitHub, for screenshots and trying it out.");
    parser.addOption(demoOption);
    // For the README's screenshots and for looking at a change without a window.
    QCommandLineOption screenshotOption("screenshot", "Draw the window offscreen into this PNG and quit.", "file");
    screenshotOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(screenshotOption);
    QCommandLineOption sceneOption("scene", "What to do before the screenshot, comma separated.", "steps");
    sceneOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(sceneOption);
    QCommandLineOption sizeOption("size", "The size of the screenshot, as 1440x900.", "size");
    sizeOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(sizeOption);
    QCommandLineOption themeOption("theme", "A directory to read the theme from instead of Omarchy's.", "directory");
    themeOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(themeOption);
    QCommandLineOption radiusOption("radius", "Corner rounding for the screenshot, as Hyprland would give it.", "pixels");
    radiusOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(radiusOption);
    parser.process(app);
    const bool demo = parser.isSet(demoOption);
    const bool shot = parser.isSet(screenshotOption);
    auto repo = parser.positionalArguments().value(0);
    if (repo.isEmpty())
        repo = demo ? QString("acme/storefront") : repoOfDirectory();
    const auto sha = parser.value(shaOption).toLower();

    if (!demo && !shot && handOver(repo, sha))
        return 0;

    // The window draws its own frames, bars and buttons; the controls it
    // does use (scroll bars) come without a style of their own.
    QQuickStyle::setStyle("Basic");

    // A screenshot shows the app's own colours unless it is given a theme,
    // so the README's pictures don't depend on whose desktop drew them.
    Theme theme(parser.isSet(themeOption) ? parser.value(themeOption) : shot ? QString("/nonexistent/omaci") : QString());
    if (parser.isSet(radiusOption))
        theme.setRadius(parser.value(radiusOption).toInt());
    Backend backend(repo, demo);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());

    // A demo window stays out of the way of the real one. A socket left
    // behind by a crash would stop the next launch listening.
    QLocalServer server;
    if (!demo && !shot) {
        QLocalServer::removeServer(socketName());
        server.setSocketOptions(QLocalServer::UserAccessOption);
        server.listen(socketName());
    }
    QObject::connect(&server, &QLocalServer::newConnection, &app, [&] {
        while (auto *client = server.nextPendingConnection()) {
            // The sender writes one short request and hangs up, so the whole
            // of it is here by the time the connection closes.
            QObject::connect(client, &QLocalSocket::disconnected, client, [&, client] {
                const auto request = QString::fromUtf8(client->readAll().left(1024)).split('\n');
                client->deleteLater();
                backend.show(request.value(0), request.value(1));
                if (window)
                    raise(window);
            });
        }
    });

    if (shot && window) {
        const auto size = parser.value(sizeOption).split('x');
        if (size.size() == 2) {
            window->setWidth(size[0].toInt());
            window->setHeight(size[1].toInt());
        }
        screenshot(window, &backend, parser.value(sceneOption).split(',', Qt::SkipEmptyParts), parser.value(screenshotOption));
    }

    backend.show(repo, sha);
    return app.exec();
}
