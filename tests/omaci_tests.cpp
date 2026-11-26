#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include "backend.h"
#include "compare.h"
#include "demo.h"
#include "estimate.h"
#include "failures.h"
#include "github.h"
#include "logmodel.h"
#include "theme.h"

// A real failed job, with what could identify the project it came from
// replaced: 662 lines of log, and the jobs of its run as the API gave them.
static QByteArray fixture(const QString &name) {
    QFile file(QString(FIXTURES) + "/" + name);
    if (!file.open(QIODevice::ReadOnly))
        qFatal("Missing fixture %s", qPrintable(name));
    return file.readAll();
}

static QVariantMap fixtureJob(const QString &name) {
    for (const auto &job : GitHub::parseJobs(fixture("jobs.json")))
        if (job.toMap()["name"] == name)
            return job.toMap();
    return {};
}

static QString at(int secondsAfterNine) {
    return QDateTime(QDate(2026, 11, 25), QTime(9, 0), QTimeZone::UTC).addSecs(secondsAfterNine).toString(Qt::ISODate);
}

// A run as parseRuns gives it: `start` and `seconds` in seconds after nine.
static QVariantMap run(qlonglong id, const QString &state, const QString &sha = "aaaaaaa1", int start = 0,
                       int seconds = 60, const QString &workflow = "Tests & Deploy",
                       const QString &branch = "main", const QString &event = "push") {
    return {{"id", id},
            {"state", state},
            {"workflow", workflow},
            {"title", "Change " + sha},
            {"branch", branch},
            {"event", event},
            {"actor", "mira"},
            {"sha", sha.left(7)},
            {"fullSha", sha},
            {"url", QString("https://github.com/owner/repo/actions/runs/%1").arg(id)},
            {"startedAt", at(start)},
            {"updatedAt", at(start + seconds)}};
}

static QVariantMap job(qlonglong id, const QString &state, const QString &name = {}, int start = 0, int seconds = 30,
                       bool withSteps = true) {
    QVariantList steps;
    if (withSteps)
        steps << QVariantMap{{"number", 1}, {"name", "Run"}, {"state", state}, {"startedAt", at(start)},
                             {"completedAt", at(start + seconds)}};
    const bool done = !GitHub::isActive(state);
    return {{"id", id},
            {"name", name.isEmpty() ? QString("job %1").arg(id) : name},
            {"workflow", "Tests & Deploy"},
            {"state", state},
            {"url", QString("https://github.com/owner/repo/actions/runs/1/job/%1").arg(id)},
            {"startedAt", at(start)},
            {"completedAt", done ? at(start + seconds) : QString()},
            {"steps", steps}};
}

static Estimate::Job plain(const QString &key, const QString &state, double start = -1, double end = -1) {
    Estimate::Job job;
    job.key = key;
    job.state = state;
    job.start = start;
    job.end = end;
    return job;
}

// An earlier build: test and build side by side, deploy once both are done.
static Estimate::Earlier earlier(double test, double image, double deploy) {
    const double ready = qMax(10 + test, 10 + image);
    return {{"test", plain("test", "success", 10, 10 + test)},
            {"build", plain("build", "success", 10, 10 + image)},
            {"deploy", plain("deploy", "success", ready + 2, ready + 2 + deploy)}};
}

static QString stateFile() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + "/omaci/state.ini";
}

// QTest only types whole strings into widgets; a window gets them a key at a time.
static void typeInto(QWindow *window, const QString &text) {
    for (const auto c : text)
        QTest::keyClick(window, c.toLatin1());
}

class OmaciTests : public QObject {
    Q_OBJECT

private slots:
    // The repository a window remembers goes to a test directory, never to
    // the one the app itself opens on.
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QFile::remove(stateFile());
    }
    void cleanupTestCase() { QFile::remove(stateFile()); }

    void statesFromStatusAndConclusion()
    {
        QCOMPARE(GitHub::state("in_progress", ""), QString("running"));
        QCOMPARE(GitHub::state("queued", ""), QString("queued"));
        QCOMPARE(GitHub::state("waiting", ""), QString("queued"));
        QCOMPARE(GitHub::state("completed", "success"), QString("success"));
        QCOMPARE(GitHub::state("completed", "timed_out"), QString("failure"));
        QCOMPARE(GitHub::state("completed", "cancelled"), QString("cancelled"));
        QCOMPARE(GitHub::state("completed", "skipped"), QString("skipped"));
    }

    void parsesRuns()
    {
        const QByteArray json = R"({"workflow_runs":[{"id":37935804748,"name":"Tests & Deploy","display_title":"Post announcements","head_branch":"main","event":"push","actor":{"login":"mira"},"run_number":4335,"run_attempt":1,"head_sha":"7847dc23aa6e551090aff0df","html_url":"https://github.com/x","status":"in_progress","conclusion":null,"run_started_at":"2026-10-09T13:17:36Z","created_at":"2026-10-09T13:17:30Z","updated_at":"2026-10-09T13:17:49Z"}]})";
        QString error;
        const auto runs = GitHub::parseRuns(json, &error);
        QVERIFY(error.isEmpty());
        QCOMPARE(runs.size(), 1);
        const auto r = runs.first().toMap();
        QCOMPARE(r["id"].toLongLong(), 37935804748LL);
        QCOMPARE(r["state"].toString(), QString("running"));
        QCOMPARE(r["sha"].toString(), QString("7847dc2"));
        QCOMPARE(r["actor"].toString(), QString("mira"));
        QCOMPARE(r["startedAt"].toString(), QString("2026-10-09T13:17:36Z"));
    }

    void findsTheRepositoryOfAGitRemote()
    {
        QCOMPARE(GitHub::repoFromRemote("git@github.com:wbso-ai/application.git"), QString("wbso-ai/application"));
        QCOMPARE(GitHub::repoFromRemote("https://github.com/basecamp/omarchy"), QString("basecamp/omarchy"));
        QCOMPARE(GitHub::repoFromRemote("https://github.com/basecamp/omarchy.git/"), QString("basecamp/omarchy"));
        QCOMPARE(GitHub::repoFromRemote("ssh://git@github.com/jankeesvw/dot.files.git"), QString("jankeesvw/dot.files"));
        QCOMPARE(GitHub::repoFromRemote("ssh://git@github.com:22/owner/repo"), QString("owner/repo"));
        QCOMPARE(GitHub::repoFromRemote("https://token@github.com/owner/repo.git\n"), QString("owner/repo"));
        QVERIFY(GitHub::repoFromRemote("git@gitlab.com:owner/repo.git").isEmpty());
        QVERIFY(GitHub::repoFromRemote("https://github.com.evil.example/owner/repo").isEmpty());
        QVERIFY(GitHub::repoFromRemote("/srv/git/repo.git").isEmpty());
    }

    void parsesRepositories()
    {
        const QByteArray json = R"([{"full_name":"acme/shop","description":null,"private":true,"archived":false,"pushed_at":"2026-11-25T10:00:00Z"},
                                    {"full_name":"acme/old","description":"Gone","private":false,"archived":true,"pushed_at":"2024-01-01T00:00:00Z"}])";
        QString error;
        const auto repositories = GitHub::parseRepositories(json, &error);
        QVERIFY(error.isEmpty());
        QCOMPARE(repositories.size(), 2);
        QCOMPARE(repositories[0].toMap()["name"].toString(), QString("acme/shop"));
        QCOMPARE(repositories[0].toMap()["description"].toString(), QString());
        QVERIFY(repositories[1].toMap()["archived"].toBool());
        QVERIFY(GitHub::parseRepositories(R"({"message":"Bad credentials"})", &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void opensOnTheRepositoryWatchedLast()
    {
        // Nothing watched yet: the window stays empty and shows the menu.
        {
            Backend backend("");
            backend.setOffline(true);
            backend.show("");
            QVERIFY(backend.repo().isEmpty());
            QVERIFY(backend.error().isEmpty());
        }
        QSettings(stateFile(), QSettings::IniFormat).setValue("repository", "acme/storefront");
        QCOMPARE(Backend::rememberedRepo(), QString("acme/storefront"));
        {
            Backend backend("");
            backend.setOffline(true);
            backend.show("");
            QCOMPARE(backend.repo(), QString("acme/storefront"));
        }
        // A second launch without a repository leaves the open window on its own.
        {
            Backend backend("owner/repo");
            backend.setOffline(true);
            backend.show("owner/repo");
            backend.show("");
            QCOMPARE(backend.repo(), QString("owner/repo"));
        }
    }

    void reportsBrokenJson()
    {
        QString error;
        QVERIFY(GitHub::parseRuns("<html>", &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void parsesJobsWithSteps()
    {
        const auto jobs = GitHub::parseJobs(fixture("jobs.json"));
        QCOMPARE(jobs.size(), 5);
        const auto test = fixtureJob("test");
        QCOMPARE(test["id"].toLongLong(), 139105231175LL);
        QCOMPARE(test["runId"].toLongLong(), 45585201590LL);
        QCOMPARE(test["state"].toString(), QString("failure"));
        const auto steps = test["steps"].toList();
        QCOMPARE(steps.size(), 18);
        QCOMPARE(steps[13].toMap()["name"].toString(), QString("RSpec tests"));
        QCOMPARE(steps[13].toMap()["number"].toInt(), 14);
        QCOMPARE(steps[13].toMap()["state"].toString(), QString("failure"));
    }

    void parsesLogMarkersAndStripsNoise()
    {
        const QByteArray raw = QByteArray("\xEF\xBB\xBF") + "2026-10-09T13:17:39.8972190Z Current runner\r\n"
                               "2026-10-09T13:17:39.9008832Z ##[group]Runner Image\n"
                               "2026-10-09T13:17:39.9028323Z ##[endgroup]\n"
                               "2026-10-09T13:17:40.0000000Z \x1b[36;1mnpm test\x1b[0m\n"
                               "2026-10-09T13:17:41.0000000Z [command]/usr/bin/git log\n"
                               "2026-10-09T13:17:42.0000000Z ##[warning]Deprecated\n"
                               "2026-10-09T13:17:43.0000000Z ##[error]Process completed with exit code 1.\n";
        const auto lines = GitHub::parseLog(raw);
        QCOMPARE(lines.size(), 6);
        QCOMPARE(lines[0].text, QString("Current runner"));
        QCOMPARE(lines[0].time.size(), 8);
        QCOMPARE(lines[1].kind, int(GitHub::Group));
        QCOMPARE(lines[1].text, QString("Runner Image"));
        QCOMPARE(lines[2].text, QString("npm test"));
        QCOMPARE(lines[3].kind, int(GitHub::Command));
        QCOMPARE(lines[4].kind, int(GitHub::Warning));
        QCOMPARE(lines[5].kind, int(GitHub::Error));
        QCOMPARE(lines[5].text, QString("Process completed with exit code 1."));
        // The log's clock is kept to the millisecond, for telling steps apart.
        QVERIFY(lines[1].ms - lines[0].ms >= 3 && lines[1].ms - lines[0].ms <= 4);
    }

    void logModelAppendsGrowingLogs()
    {
        LogModel model;
        model.setLines(GitHub::parseLog("one\ntwo\n"));
        QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        model.setLines(GitHub::parseLog("one\ntwo\n##[error]three\n"));
        QCOMPARE(inserted.size(), 1);
        QCOMPARE(reset.size(), 0);
        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.text(), QString("one\ntwo\nthree"));
    }

    void logModelKnowsWhichStepALineBelongsTo()
    {
        LogModel model;
        model.setLines(GitHub::parseLog("a\nb\nc\nd\ne\n"));
        model.setSteps({0, -1, 2, 4}, {"Set up job", "Skipped", "Run tests", "Complete job"});
        QCOMPARE(model.stepAt(0), 0);
        QCOMPARE(model.stepAt(1), 0);
        QCOMPARE(model.stepAt(3), 2);
        QCOMPARE(model.stepAt(4), 3);
        QCOMPARE(model.stepEnd(0), 1);
        QCOMPARE(model.stepEnd(2), 3);
        QCOMPARE(model.stepEnd(3), 4);
        QCOMPARE(model.data(model.index(2), LogModel::StepRole).toString(), QString("Run tests"));
        QCOMPARE(model.data(model.index(3), LogModel::StepRole).toString(), QString());
    }

    // --- Builds ---

    void aBuildIsACommitWithItsRunsRolledUp()
    {
        const auto builds = GitHub::builds({
            run(4, "running", "bbbbbbb2", 100, 50),
            run(3, "success", "bbbbbbb2", 101, 40, "Linting & Security"),
            run(2, "success", "aaaaaaa1", 0, 400),
            run(1, "success", "aaaaaaa1", 2, 45, "Linting & Security"),
        });
        QCOMPARE(builds.size(), 2);
        const auto going = builds[0].toMap(), done = builds[1].toMap();
        QCOMPARE(going["state"].toString(), QString("running"));
        QCOMPARE(going["runIds"].toList().size(), 2);
        QCOMPARE(going["workflows"].toStringList(), QStringList({"Tests & Deploy", "Linting & Security"}));
        QCOMPARE(done["state"].toString(), QString("success"));
        // From the first run starting to the last one ending.
        QCOMPARE(done["seconds"].toInt(), 400);
        QCOMPARE(done["key"].toString(), QString("aaaaaaa1"));
    }

    void aBuildIsRedWhenAnyRunFailedAndSaysSoWhileStillRunning()
    {
        auto builds = GitHub::builds({run(2, "failure", "aaaaaaa1", 0, 100), run(1, "success", "aaaaaaa1", 0, 40)});
        QCOMPARE(builds[0].toMap()["state"].toString(), QString("failure"));
        // The failed run is the one to open and to name the build after.
        QVERIFY(builds[0].toMap()["url"].toString().endsWith("/2"));

        builds = GitHub::builds({run(2, "failure", "aaaaaaa1", 0, 100), run(1, "running", "aaaaaaa1", 0, 40)});
        QCOMPARE(builds[0].toMap()["state"].toString(), QString("running"));
        QVERIFY(builds[0].toMap()["failing"].toBool());
    }

    void scheduledAndCommentRunsStayOutOfTheBuild()
    {
        const auto builds = GitHub::builds({
            run(3, "success", "aaaaaaa1", 40000, 600, "Nightly", "main", "schedule"),
            run(2, "skipped", "aaaaaaa1", 500, 1, "Claude Code", "main", "issue_comment"),
            run(1, "success", "aaaaaaa1", 0, 400),
        });
        QCOMPARE(builds.size(), 3);
        QCOMPARE(builds[2].toMap()["seconds"].toInt(), 400);
        QVERIFY(!builds[0].toMap()["isBuild"].toBool());
        QCOMPARE(builds[1].toMap()["state"].toString(), QString("skipped"));
    }

    void finishedBuildsOnlyReportsTransitions()
    {
        const auto before = GitHub::builds({run(1, "running", "a"), run(2, "queued", "b"), run(3, "success", "c"),
                                            run(4, "running", "d")});
        const auto after = GitHub::builds({run(1, "failure", "a"), run(2, "running", "b"), run(3, "success", "c"),
                                           run(4, "skipped", "d"), run(5, "success", "e")});
        const auto finished = GitHub::finishedBuilds(before, after);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().toMap()["key"].toString(), QString("a"));
    }

    // --- Steps to lines ---

    void mapsEveryStepOfARealJobToItsLine()
    {
        const auto lines = GitHub::parseLog(fixture("test-job.log"));
        const auto steps = fixtureJob("test")["steps"].toList();
        const auto starts = GitHub::mapSteps(steps, lines);
        QCOMPARE(starts.size(), steps.size());

        // Three of these start in the same second (Set Rails Master Key, Set
        // up Ruby, Install gems), which is what times alone can't tell apart.
        const QList<QPair<QString, QString>> expected{
            {"Set up job", "Current runner version"},
            {"Initialize containers", "Checking docker version"},
            {"Run actions/checkout@v7.0.1", "Run actions/checkout@v7.0.1"},
            {"Look for a local signoff on this commit", ""},
            {"Set Rails Master Key", "Run echo \"RAILS_MASTER_KEY"},
            {"Set up Ruby", "Run ruby/setup-ruby"},
            {"Install gems", "Run bundle install"},
            {"Install system libraries", ""},
            {"Setup Chrome and ChromeDriver", ""},
            {"Run chrome --version", "Run chrome --version"},
            {"Build the stylesheet", "Run bundle exec rails tailwindcss:build"},
            {"Set up databases", "Run bundle exec parallel_test"},
            {"Restore the spec runtimes", "Run actions/cache/restore"},
            {"RSpec tests", "Run bundle exec parallel_rspec"},
            {"Save the spec runtimes", ""},
            {"Post Run actions/checkout@v7.0.1", "Post job cleanup."},
            {"Stop containers", "Print service container logs"},
            {"Complete job", "Cleaning up orphan processes"},
        };
        int previous = -1;
        for (int i = 0; i < steps.size(); ++i) {
            const auto name = steps[i].toMap()["name"].toString();
            QCOMPARE(name, expected[i].first);
            if (expected[i].second.isEmpty()) {
                QVERIFY2(starts[i] == -1, qPrintable(name + " was skipped and has no place in the log"));
                continue;
            }
            QVERIFY2(starts[i] > previous, qPrintable(name));
            QVERIFY2(lines[starts[i]].text.startsWith(expected[i].second),
                     qPrintable(name + " landed on: " + lines[starts[i]].text));
            previous = starts[i];
        }
        QCOMPARE(starts[0], 0);
    }

    void mapsTheStepsOfAJobThatIsStillRunning()
    {
        const auto lines = GitHub::parseLog("2026-11-25T09:00:00.1000000Z Current runner version: '2.338.2'\n"
                                            "2026-11-25T09:00:02.2000000Z ##[group]Run actions/checkout@v6\n"
                                            "2026-11-25T09:00:02.9000000Z ##[group]Run bundle install\n"
                                            "2026-11-25T09:00:03.5000000Z Fetching gems\n");
        auto step = [](const QString &name, const QString &state, int start, int end) {
            return QVariantMap{{"name", name}, {"state", state}, {"startedAt", start < 0 ? QString() : at(start)},
                               {"completedAt", end < 0 ? QString() : at(end)}};
        };
        const QVariantList steps{step("Set up job", "success", 0, 2), step("Check out", "success", 2, 2),
                                 step("Install gems", "running", 2, -1), step("Run tests", "queued", -1, -1),
                                 step("Post Check out", "queued", -1, -1)};
        QCOMPARE(GitHub::mapSteps(steps, lines), QList<int>({0, 1, 2, -1, -1}));
        // Nothing to map onto yet.
        QCOMPARE(GitHub::mapSteps(steps, {}), QList<int>({-1, -1, -1, -1, -1}));
    }

    void aStepWithoutAMarkerFallsBackToItsStartTime()
    {
        const auto lines = GitHub::parseLog("2026-11-25T09:00:00.1000000Z Current runner version\n"
                                            "2026-11-25T09:00:01.2000000Z still setting up\n"
                                            "2026-11-25T09:00:05.3000000Z a pre step, which has no marker of its own\n"
                                            "2026-11-25T09:00:09.2000000Z ##[group]Run make\n");
        const QVariantList steps{
            QVariantMap{{"name", "Set up job"}, {"state", "success"}, {"startedAt", at(0)}, {"completedAt", at(5)}},
            QVariantMap{{"name", "Pre Run some/action"}, {"state", "success"}, {"startedAt", at(5)}, {"completedAt", at(8)}},
            QVariantMap{{"name", "Build"}, {"state", "success"}, {"startedAt", at(9)}, {"completedAt", at(20)}}};
        QCOMPARE(GitHub::mapSteps(steps, lines), QList<int>({0, 2, 3}));
    }

    // --- What failed ---

    void findsTheRspecFailureInARealLogNotTheExitCode()
    {
        const auto lines = GitHub::parseLog(fixture("test-job.log"));
        const auto steps = fixtureJob("test")["steps"].toList();
        const auto starts = GitHub::mapSteps(steps, lines);
        LogModel model;
        model.setLines(lines);
        model.setSteps(starts, {});

        const auto failures = Failures::find(lines, starts[13], model.stepEnd(13));
        QCOMPARE(failures.size(), 1);
        const auto failure = failures.first();
        QCOMPARE(failure.title, QString("Admin::Orders lists the orders that are not shipped yet, oldest first"));
        QCOMPARE(failure.location, QString("spec/requests/admin/orders_spec.rb:107"));
        QVERIFY(failure.detail.startsWith("expect(body.index("));
        QVERIFY(lines[failure.line].text.contains("1) Admin::Orders"));
        QCOMPARE(lines[failure.first].text, QString("Failures:"));
        QVERIFY(lines[failure.last].text.startsWith("rspec ./spec/requests/admin/orders_spec.rb:107"));

        // The runner's own error line is the last of the step, some ninety
        // lines further down, behind the other processes' summaries.
        int exitLine = -1;
        for (int i = 0; i < lines.size(); ++i)
            if (lines[i].kind == GitHub::Error)
                exitLine = i;
        QVERIFY(exitLine - failure.line > 60);

        // Looking at the whole log finds the same one: the database's own
        // ERROR lines in "Stop containers" are not failures.
        const auto everywhere = Failures::find(lines);
        QCOMPARE(everywhere.size(), 1);
        QCOMPARE(everywhere.first().line, failure.line);

        const auto excerpt = Failures::excerpt(lines, failures);
        QVERIFY(excerpt.startsWith("Failures:"));
        QVERIFY(excerpt.contains("Failure/Error:"));
        QVERIFY(excerpt.contains("got:   114047"));
        QVERIFY(excerpt.trimmed().endsWith("oldest first"));
    }

    void findsEveryRspecFailureAndItsLocation()
    {
        const auto lines = GitHub::parseLog("Failures:\n\n"
                                            "  1) Cart adds a gift card\n"
                                            "     Failure/Error: expect(cart.total).to eq(5)\n"
                                            "       expected: 5\n\n"
                                            "  2) Cart removes a gift card\n"
                                            "     Failure/Error: raise \"boom\"\n\n"
                                            "Finished in 2 seconds\n2 examples, 2 failures\n\nFailed examples:\n\n"
                                            "rspec ./spec/cart_spec.rb:4 # Cart adds a gift card\n"
                                            "rspec ./spec/cart_spec.rb:9 # Cart removes a gift card\n"
                                            "##[error]Process completed with exit code 1.\n");
        const auto failures = Failures::find(lines);
        QCOMPARE(failures.size(), 2);
        QCOMPARE(failures[0].location, QString("spec/cart_spec.rb:4"));
        QCOMPARE(failures[0].detail, QString("expect(cart.total).to eq(5)"));
        QCOMPARE(failures[0].first, 0);
        QCOMPARE(failures[1].title, QString("Cart removes a gift card"));
        QCOMPARE(failures[1].location, QString("spec/cart_spec.rb:9"));
        QCOMPARE(failures[1].line, 6);
        QVERIFY(failures[0].last < failures[1].first);
    }

    void findsCucumberFailingScenarios()
    {
        const auto lines = GitHub::parseLog("  Scenario: Pay with a gift card # features/checkout.feature:12\n"
                                            "Failing Scenarios:\n"
                                            "cucumber features/checkout.feature:12 # Scenario: Pay with a gift card\n"
                                            "cucumber features/cart.feature:30 # Scenario: Empty the cart\n"
                                            "\n103 scenarios (2 failed, 101 passed)\n"
                                            "##[error]Process completed with exit code 1.\n");
        const auto failures = Failures::find(lines);
        QCOMPARE(failures.size(), 2);
        QCOMPARE(failures[0].line, 2);
        QCOMPARE(failures[0].first, 1);
        QCOMPARE(failures[0].location, QString("features/checkout.feature:12"));
        QCOMPARE(failures[0].title, QString("Scenario: Pay with a gift card"));
        QCOMPARE(failures[1].location, QString("features/cart.feature:30"));
    }

    void findsMinitestPytestJestAndGoFailures()
    {
        auto failures = Failures::find(GitHub::parseLog("Failure:\n"
                                                        "OrderTotalTest#test_includes_tax [test/models/order_total_test.rb:18]:\n"
                                                        "Expected: 121.0\n  Actual: 100.0\n\n"
                                                        "bin/rails test test/models/order_total_test.rb:18\n"));
        QCOMPARE(failures.size(), 1);
        QCOMPARE(failures[0].title, QString("OrderTotalTest#test_includes_tax"));
        QCOMPARE(failures[0].location, QString("test/models/order_total_test.rb:18"));
        QCOMPARE(failures[0].detail, QString("Expected: 121.0"));
        QCOMPARE(failures[0].last, 3);

        failures = Failures::find(GitHub::parseLog("FAILED tests/test_cart.py::test_total - assert 100 == 121\n"));
        QCOMPARE(failures.size(), 1);
        QCOMPARE(failures[0].location, QString("tests/test_cart.py"));
        QCOMPARE(failures[0].detail, QString("assert 100 == 121"));

        failures = Failures::find(GitHub::parseLog(QString("  ● Cart › adds a gift card\n").toUtf8()));
        QCOMPARE(failures.size(), 1);
        QCOMPARE(failures[0].title, QString("Cart › adds a gift card"));

        failures = Failures::find(GitHub::parseLog("--- FAIL: TestCartTotal (0.00s)\n"));
        QCOMPARE(failures.size(), 1);
        QCOMPARE(failures[0].title, QString("TestCartTotal"));
    }

    void annotationsCountAndTheExitCodeIsTheLastResort()
    {
        auto failures = Failures::find(GitHub::parseLog("##[error]app/models/cart.rb#L12: Style/StringLiterals: Prefer double quotes\n"
                                                        "##[error]Process completed with exit code 1.\n"));
        QCOMPARE(failures.size(), 1);
        QCOMPARE(failures[0].location, QString("app/models/cart.rb:12"));
        QCOMPARE(failures[0].title, QString("Style/StringLiterals: Prefer double quotes"));

        failures = Failures::find(GitHub::parseLog("make: *** [all] Error 2\n##[error]Process completed with exit code 2.\n"));
        QCOMPARE(failures.size(), 1);
        QCOMPARE(failures[0].line, 1);

        QVERIFY(Failures::find(GitHub::parseLog("312 examples, 0 failures\nAll good\n")).isEmpty());
    }

    // --- How long still ---

    void quantilesAndRounding()
    {
        QCOMPARE(Estimate::quantile({4, 1, 3, 2, 5}, .5), 3.0);
        QCOMPARE(Estimate::quantile({1, 2, 3, 4}, .5), 2.5);
        QCOMPARE(Estimate::quantile({10, 20, 30, 40, 50}, .25), 20.0);
        QCOMPARE(Estimate::quantile({}, .5), -1.0);
        // Rounded to what a forecast can be trusted to.
        QCOMPARE(Estimate::rounded(38), 40);
        QCOMPARE(Estimate::rounded(233), 230);
        QCOMPARE(Estimate::rounded(410), 420);
        QCOMPARE(Estimate::rounded(1000), 1020);
    }

    void comparesABuildWithBuildsLikeIt()
    {
        QVariantList runs;
        // Newest first: the build in question, then four on main, one that
        // failed among them, and three pull requests.
        runs << run(100, "running", "now00000", 9000, 10) << run(99, "success", "main0004", 8000, 400)
             << run(98, "failure", "main0003", 7000, 90) << run(97, "success", "main0002", 6000, 420)
             << run(96, "success", "main0001", 5000, 380);
        for (int i = 0; i < 3; ++i)
            runs << run(90 - i, "success", QString("pull000%1").arg(i), 4000 - i * 500, 200, "Tests & Deploy",
                        QString("feature-%1").arg(i), "pull_request");
        const auto builds = GitHub::builds(runs);

        const auto like = Estimate::likeBuilds(builds, builds.first().toMap());
        QCOMPARE(like.size(), 4); // Main only: there are three that passed.
        const auto spread = Estimate::spread(like);
        QCOMPARE(spread.samples, 3); // The failed one stopped early and doesn't count.
        QVERIFY(spread.known());
        QCOMPARE(spread.typical, 400.0);
        QCOMPARE(spread.low, 390.0);
        QCOMPARE(spread.high, 410.0);

        // A pull request on a branch nobody built before goes by other pull requests.
        const auto pull = GitHub::builds({run(200, "running", "newpull0", 9500, 5, "Tests & Deploy", "brand-new", "pull_request")})
                              .first().toMap();
        const auto likePull = Estimate::likeBuilds(builds, pull);
        QCOMPARE(likePull.size(), 3);
        QCOMPARE(Estimate::spread(likePull).typical, 200.0);

        // Builds that started after it are not history.
        const auto old = builds[3].toMap();
        for (const auto &other : Estimate::likeBuilds(builds, old))
            QVERIFY(GitHub::ms(other.toMap()["startedAt"].toString()) < GitHub::ms(old["startedAt"].toString()));
    }

    void withoutThreeEarlierBuildsThereIsNoEstimate()
    {
        Estimate::Spread two;
        two.samples = 2;
        two.typical = 400;
        const auto forecast = Estimate::forecast({plain("test", "running", 10)}, 60, two, {});
        QVERIFY(!forecast.known);
    }

    void fallsBackToWholeBuildsUntilTheJobsOfEarlierOnesAreHere()
    {
        Estimate::Spread spread;
        spread.samples = 5;
        spread.low = 380;
        spread.typical = 400;
        spread.high = 430;
        const auto forecast = Estimate::forecast({plain("test", "running", 10)}, 100, spread, {});
        QVERIFY(forecast.known);
        QCOMPARE(forecast.basis, QString("builds"));
        QCOMPARE(forecast.total, 400.0);
        QCOMPARE(forecast.low, 380.0);
        QCOMPARE(forecast.high, 430.0);
        QVERIFY(!forecast.late);
    }

    void playsTheBuildForwardJobByJob()
    {
        const QList<Estimate::Earlier> history{earlier(200, 100, 60), earlier(220, 110, 70), earlier(180, 90, 50)};
        // 100 seconds in: test and build both started at 12. GitHub hasn't
        // created deploy yet, because it needs both.
        const auto forecast = Estimate::forecast({plain("test", "running", 12), plain("build", "running", 12)}, 100,
                                                 Estimate::Spread{}, history);
        QVERIFY(forecast.known);
        QCOMPARE(forecast.basis, QString("jobs"));
        QCOMPARE(forecast.samples, 3);
        // test ends at 12 + 200, deploy runs 60 after it: 272. The earlier
        // builds had two seconds between them that the forecast doesn't add.
        QCOMPARE(forecast.total, 272.0);
        QCOMPARE(forecast.low, 12 + 190 + 55.0);
        QCOMPARE(forecast.high, 12 + 210 + 65.0);
        QVERIFY(!forecast.late);

        QCOMPARE(forecast.jobs.size(), 3);
        const auto deploy = forecast.jobs[2];
        QCOMPARE(deploy.key, QString("deploy"));
        QVERIFY(deploy.expected);
        QCOMPARE(deploy.start, 212.0); // When test, the slower of the two, is done.
        QCOMPARE(deploy.end, 272.0);
        QCOMPARE(deploy.typical, 60.0);
        const auto image = forecast.jobs[1];
        QCOMPARE(image.end, 112.0);
        QVERIFY(!image.late);
    }

    void aJobPastItsUsualLengthEndsNowAndTheBuildSaysWhenItIsLate()
    {
        const QList<Estimate::Earlier> history{earlier(200, 100, 60), earlier(220, 110, 70), earlier(180, 90, 50)};
        // build should have been done at 112 and is still going at 150.
        auto forecast = Estimate::forecast({plain("test", "running", 12), plain("build", "running", 12)}, 150,
                                           Estimate::Spread{}, history);
        QVERIFY(forecast.jobs[1].late);
        QCOMPARE(forecast.jobs[1].end, 150.0);
        QVERIFY(!forecast.late); // test and deploy are still to come, on time.

        // Everything done but a deploy that started at 215 and usually takes 60.
        forecast = Estimate::forecast({plain("test", "success", 12, 210), plain("build", "success", 12, 110),
                                       plain("deploy", "running", 215)},
                                      300, Estimate::Spread{}, history);
        QVERIFY(forecast.late);
        QCOMPARE(forecast.total, 300.0);
        QCOMPARE(forecast.over, 25.0);
    }

    void onlyEarlierBuildsThatDidTheSameWorkCount()
    {
        // Two builds that only touched the documentation and skipped the
        // tests, three that ran them.
        Estimate::Earlier docs{{"changes", plain("changes", "success", 2, 10)},
                               {"test", plain("test", "skipped", 12, 12)}};
        auto full = [](double test) {
            return Estimate::Earlier{{"changes", plain("changes", "success", 2, 10)},
                                     {"test", plain("test", "success", 14, 14 + test)}};
        };
        const QList<Estimate::Earlier> history{docs, full(200), docs, full(220), full(180)};

        // Once the tests run, the short builds say nothing about this one.
        const auto forecast = Estimate::forecast({plain("changes", "success", 2, 10), plain("test", "running", 14)}, 50,
                                                 Estimate::Spread{}, history);
        QVERIFY(forecast.known);
        QCOMPARE(forecast.samples, 3);
        QCOMPARE(forecast.total, 214.0);
    }

    void aJobBehindAFailureIsNotExpected()
    {
        const QList<Estimate::Earlier> history{earlier(200, 100, 60), earlier(220, 110, 70), earlier(180, 90, 50)};
        const auto forecast = Estimate::forecast({plain("test", "failure", 12, 80), plain("build", "running", 12)}, 90,
                                                 Estimate::Spread{}, history);
        QVERIFY(forecast.known);
        QCOMPARE(forecast.jobs.size(), 2); // No deploy.
        QCOMPARE(forecast.total, 112.0);   // Only build is left.
    }

    // --- Next to the build before it ---

    void thePreviousBuildIsTheLastFinishedOneOnMain()
    {
        const auto builds = GitHub::builds({
            run(6, "running", "new6", 6000, 10),
            run(5, "success", "pull5", 5000, 200, "Tests & Deploy", "feature", "pull_request"),
            run(4, "running", "main4", 4500, 10),
            run(3, "failure", "main3", 4000, 90),
            run(2, "skipped", "main2", 3000, 1),
            run(1, "success", "main1", 2000, 400),
        });
        // Not the pull request, not the one still running: the red one.
        QCOMPARE(Compare::previous(builds, builds[0].toMap(), "main")["key"].toString(), QString("main3"));
        // A pull request is compared with main as it was when it started.
        QCOMPARE(Compare::previous(builds, builds[1].toMap(), "main")["key"].toString(), QString("main3"));
        // Skipped builds did nothing to compare with.
        QCOMPARE(Compare::previous(builds, builds[3].toMap(), "main")["key"].toString(), QString("main1"));
        QVERIFY(Compare::previous(builds, builds[5].toMap(), "main").isEmpty());
    }

    void comparesEachJobWithTheSameJobBefore()
    {
        const QList<Estimate::Job> then{plain("test", "success", 10, 210), plain("build", "success", 10, 110),
                                        plain("lint", "success", 5, 45), plain("deploy", "success", 215, 275),
                                        plain("flaky", "failure", 10, 50)};
        const QList<Estimate::Job> now{plain("test", "failure", 10, 90),    // broke
                                       plain("build", "success", 10, 180),  // 70s slower
                                       plain("lint", "success", 5, 47),     // two seconds is nothing
                                       plain("deploy", "skipped", 95, 95),  // nothing to compare
                                       plain("flaky", "success", 10, 30),   // fixed
                                       plain("audit", "success", 5, 20)};   // wasn't there
        const auto rows = Compare::jobs(now, then);
        // deploy is on both lists (skipped now), so nothing is missing yet.
        QCOMPARE(rows.size(), 6);
        // A job only the earlier build had shows once this build is done,
        // not while it runs: GitHub may not have created it yet.
        const auto missing = Compare::jobs({plain("test", "success", 10, 200)}, then, true);
        QCOMPARE(missing.size(), 5);
        QCOMPARE(missing[1].key, QString("build"));
        QCOMPARE(missing[1].verdict, QString("gone"));
        QCOMPARE(missing[1].then, 100.0);
        QVERIFY(Compare::notes(missing).contains("deploy didn't run"));
        QCOMPARE(Compare::jobs({plain("test", "running", 10, 50)}, then, false).size(), 1);
        QCOMPARE(rows[0].verdict, QString("broke"));
        QCOMPARE(rows[0].then, 200.0);
        QCOMPARE(rows[1].verdict, QString("slower"));
        QCOMPARE(rows[1].delta, 70.0);
        QCOMPARE(rows[1].thenStart, 10.0);
        QCOMPARE(rows[1].thenEnd, 110.0);
        QCOMPARE(rows[2].verdict, QString("same"));
        QCOMPARE(rows[3].verdict, QString());
        QCOMPARE(rows[3].now, -1.0);
        QCOMPARE(rows[4].verdict, QString("fixed"));
        QCOMPARE(rows[5].verdict, QString("new"));
        // The worst first: what broke, then what got slower.
        QCOMPARE(Compare::notes(rows), QStringList({"test broke", "build 1m 10s slower", "flaky fixed"}));

        // A job that is still running says nothing until it has outlasted the last time.
        auto running = Compare::jobs({plain("test", "running", 10, 100)}, then, false);
        QCOMPARE(running[0].verdict, QString());
        running = Compare::jobs({plain("test", "running", 10, 260)}, then, false);
        QCOMPARE(running[0].verdict, QString("slower"));
        QCOMPARE(Compare::notes(running), QStringList({"test 50s slower"}));

        QVERIFY(Compare::notable(120, 100));
        QVERIFY(!Compare::notable(104, 100)); // Under five seconds.
        QVERIFY(!Compare::notable(1100, 1000)); // Under fifteen percent.
        QCOMPARE(Compare::jobs({plain("test", "success", 0, 60)}, {plain("test", "success", 0, 100)})[0].verdict,
                 QString("faster"));
    }

    void backendComparesWithThePreviousMainBuildAndGoesThere()
    {
        Backend backend("owner/repo");
        backend.setOffline(true);
        backend.applyRuns({run(3, "failure", "pull3", 3000, 100, "Tests & Deploy", "feature", "pull_request"),
                           run(2, "success", "main2", 2000, 300), run(1, "success", "main1", 1000, 280)});
        QCOMPARE(backend.selectedKey(), QString("pull3"));
        QCOMPARE(backend.previous()["key"].toString(), QString("main2"));
        QCOMPARE(backend.previous()["seconds"].toInt(), 300);
        QVERIFY(!backend.previous()["ready"].toBool()); // Its jobs aren't here yet.

        backend.applyJobs(3, {job(30, "failure", "test", 3005, 60), job(31, "success", "build", 3005, 90)});
        backend.applyJobs(2, {job(20, "success", "test", 2005, 200), job(21, "success", "build", 2005, 50)});
        const auto previous = backend.previous();
        QVERIFY(previous["ready"].toBool());
        QCOMPARE(previous["notes"].toStringList(), QStringList({"test broke", "build 40s slower"}));
        const auto build = previous["jobs"].toMap()["Tests & Deploy/build"].toMap();
        QCOMPARE(build["verdict"].toString(), QString("slower"));
        QCOMPARE(build["then"].toDouble(), 50.0);
        QCOMPARE(build["thenStart"].toDouble(), 5.0);

        // One press goes there, also when the list is narrowed to another branch.
        backend.toggleBranch();
        QCOMPARE(backend.branchFilter(), QString("feature"));
        backend.selectPrevious();
        QCOMPARE(backend.selectedKey(), QString("main2"));
        QCOMPARE(backend.branchFilter(), QString());
        // And from there, the one before that.
        QCOMPARE(backend.previous()["key"].toString(), QString("main1"));
        // Shift+B goes back to where the walk began, however far it went.
        backend.selectPrevious();
        QCOMPARE(backend.selectedKey(), QString("main1"));
        QCOMPARE(backend.cameFrom(), QString("Change pull3"));
        backend.selectCameFrom();
        QCOMPARE(backend.selectedKey(), QString("pull3"));
        QCOMPARE(backend.cameFrom(), QString());
    }

    // --- The backend ---

    void backendHidesSkippedAndSelectsFirst()
    {
        Backend backend("owner/repo");
        backend.setOffline(true);
        backend.applyRuns({run(1, "skipped", "aaaa", 300, 1, "Claude Code", "main", "issue_comment"),
                           run(2, "running", "bbbb", 200), run(3, "success", "cccc", 100)});
        QCOMPARE(backend.builds().size(), 2);
        QCOMPARE(backend.selectedKey(), QString("bbbb"));
        QCOMPARE(backend.activeCount(), 1);
        backend.toggleSkipped();
        QCOMPARE(backend.builds().size(), 3);
        QCOMPARE(backend.selectedKey(), QString("bbbb"));
    }

    void backendShowsOnlyTheBranchOfTheBuildAndBack()
    {
        Backend backend("owner/repo");
        backend.setOffline(true);
        backend.applyRuns({run(1, "success", "aaaa", 300, 60, "Tests & Deploy", "feature"), run(2, "success", "bbbb", 200),
                           run(3, "success", "cccc", 100)});
        backend.selectBuild("bbbb");
        backend.toggleBranch();
        QCOMPARE(backend.branchFilter(), QString("main"));
        QCOMPARE(backend.builds().size(), 2);
        QCOMPARE(backend.selectedKey(), QString("bbbb"));
        backend.toggleBranch();
        QCOMPARE(backend.branchFilter(), QString());
        QCOMPARE(backend.builds().size(), 3);
    }

    void backendShowsOnlyMainAndBack()
    {
        Backend backend("owner/repo");
        backend.setOffline(true);
        backend.applyRuns({run(1, "success", "aaaa", 300, 60, "Tests & Deploy", "feature"), run(2, "running", "bbbb", 200),
                           run(3, "success", "cccc", 100), run(4, "success", "dddd", 50, 60, "Nightly", "main", "schedule")});
        backend.selectBuild("aaaa");
        QCOMPARE(backend.builds().size(), 4);
        backend.toggleMain();
        QVERIFY(backend.mainOnly());
        // Main's builds only: not the branch, and not a scheduled job on main.
        QCOMPARE(backend.builds().size(), 2);
        QCOMPARE(backend.selectedKey(), QString("bbbb"));
        backend.toggleMain();
        QVERIFY(!backend.mainOnly());
        QCOMPARE(backend.builds().size(), 4);
    }

    void backendShowsOnlyRunningBuildsAndKeepsTheOneYouWatch()
    {
        Backend backend("owner/repo");
        backend.setOffline(true);
        const QVariantList before{run(1, "running", "aaaa", 300), run(2, "success", "bbbb", 200), run(3, "queued", "cccc", 100)};
        backend.applyRuns(before);
        backend.selectBuild("bbbb");
        backend.toggleRunning();
        QVERIFY(backend.runningOnly());
        QCOMPARE(backend.builds().size(), 2);
        QCOMPARE(backend.selectedKey(), QString("aaaa"));
        // The watched build finishes and stays; once another is picked it goes.
        const QVariantList after{run(1, "success", "aaaa", 300), run(2, "success", "bbbb", 200), run(3, "queued", "cccc", 100)};
        backend.applyRuns(after);
        QCOMPARE(backend.builds().size(), 2);
        QCOMPARE(backend.selectedKey(), QString("aaaa"));
        backend.selectBuild("cccc");
        backend.applyRuns(after);
        QCOMPARE(backend.builds().size(), 1);
        backend.toggleRunning();
        QCOMPARE(backend.builds().size(), 3);
    }

    void backendShowsTheJobsOfEveryRunOfTheBuild()
    {
        Backend backend("owner/repo");
        backend.setOffline(true);
        backend.applyRuns({run(2, "success", "aaaa", 1, 40, "Linting & Security"), run(1, "success", "aaaa", 0, 100)});
        backend.applyJobs(1, {job(10, "success", "test", 5, 90)});
        backend.applyJobs(2, {job(20, "success", "rubocop", 3, 30)});
        QCOMPARE(backend.jobs().size(), 2);
        // In the order they started, and each says which workflow it is from.
        QCOMPARE(backend.jobs()[0].toMap()["name"].toString(), QString("rubocop"));
        QCOMPARE(backend.jobs()[0].toMap()["workflow"].toString(), QString("Linting & Security"));
        QCOMPARE(backend.jobs()[1].toMap()["start"].toDouble(), 5.0);
        QCOMPARE(backend.jobs()[1].toMap()["end"].toDouble(), 95.0);
    }

    void backendPicksTheFailedJob()
    {
        Backend backend("owner/repo");
        backend.setOffline(true);
        backend.applyRuns({run(1, "failure")});
        backend.applyJobs(1, {job(10, "success"), job(11, "failure"), job(12, "skipped", {}, 0, 0, false)});
        QCOMPARE(backend.selectedJobId(), 11LL);
    }

    void backendFollowsTheRunningJobUntilPinned()
    {
        Backend backend("owner/repo");
        backend.setOffline(true);
        backend.applyRuns({run(1, "running")});
        backend.applyJobs(1, {job(10, "running"), job(11, "queued", {}, 0, 0, false)}, false);
        QCOMPARE(backend.selectedJobId(), 10LL);
        backend.applyJobs(1, {job(10, "success"), job(11, "running")}, false);
        QCOMPARE(backend.selectedJobId(), 11LL);
        backend.selectJob(10);
        backend.applyJobs(1, {job(10, "success"), job(11, "running")}, false);
        QCOMPARE(backend.selectedJobId(), 10LL);
    }

    // GitHub has no log of a running job, only of the steps it has done. The
    // log is those, in order, growing at the bottom as more come in, and the
    // whole log carries on from them once the job is done.
    void backendShowsTheLogOfARunningJobStepByStep()
    {
        Backend backend("owner/repo");
        backend.setOffline(true);
        auto step = [](int number, const QString &name, const QString &state, int start, int end) {
            return QVariantMap{{"number", number}, {"name", name}, {"state", state},
                               {"startedAt", start < 0 ? QString() : at(start)},
                               {"completedAt", end < 0 ? QString() : at(end)}};
        };
        auto running = job(10, "running");
        running["steps"] = QVariantList{step(1, "Set up job", "success", 0, 2), step(2, "Check out", "success", 2, 3),
                                        step(3, "Lint", "skipped", -1, -1), step(4, "Run tests", "running", 3, -1),
                                        step(5, "Complete job", "queued", -1, -1)};
        backend.applyRuns({run(1, "running")});
        backend.applyJobs(1, {running}, false);
        QCOMPARE(backend.selectedJobId(), 10LL);

        const QByteArray setUp = "2026-11-25T09:00:00.1000000Z Current runner version: '2.338.2'\n"
                                 "2026-11-25T09:00:01.5000000Z Complete job name: test\n";
        const QByteArray checkout = "2026-11-25T09:00:02.2000000Z ##[group]Run actions/checkout@v6\n"
                                    "2026-11-25T09:00:02.9000000Z Switched to a new branch 'main'\n";
        const QByteArray tests = "2026-11-25T09:00:03.1000000Z ##[group]Run bin/rails test\n"
                                 "2026-11-25T09:00:07.8000000Z 96 runs, 0 failures\n";
        const QByteArray complete = "2026-11-25T09:00:08.2000000Z Cleaning up orphan processes\n";

        // The steps come back in any order; the log waits for the one before.
        QSignalSpy reset(&backend, &Backend::logReset);
        backend.applyStepLog(10, 2, checkout);
        QCOMPARE(backend.log()->rowCount(), 0);
        backend.applyStepLog(10, 1, "\xEF\xBB\xBF" + setUp);
        QCOMPARE(backend.log()->rowCount(), 4);
        QCOMPARE(reset.size(), 1);
        QCOMPARE(backend.log()->lines().first().text, QString("Current runner version: '2.338.2'"));

        // Every step that is in has its place, and the running one is next,
        // at the end of the log: N goes there, past the skipped one.
        QCOMPARE(backend.logPending(), QString("Run tests"));
        QCOMPARE(backend.logStatus(), QString());
        QCOMPARE(backend.steps()[1].toMap()["line"].toInt(), 2);
        QCOMPARE(backend.steps()[3].toMap()["line"].toInt(), -1);
        QVERIFY(backend.steps()[3].toMap()["pending"].toBool());
        QCOMPARE(backend.moveStep(1, 1), 3);

        // Done: the whole log is added below what was there, not swapped in.
        auto done = running;
        done["state"] = "success";
        done["completedAt"] = at(9);
        auto steps = running["steps"].toList();
        steps[3] = step(4, "Run tests", "success", 3, 8);
        steps[4] = step(5, "Complete job", "success", 8, 9);
        done["steps"] = steps;
        backend.applyJobs(1, {done}, true);
        QSignalSpy inserted(backend.log(), &QAbstractItemModel::rowsInserted);
        backend.applyLog(10, "\xEF\xBB\xBF" + setUp + checkout + tests + complete);
        QCOMPARE(backend.log()->rowCount(), 7);
        QCOMPARE(inserted.size(), 1);
        QCOMPARE(reset.size(), 1);
        QCOMPARE(backend.logPending(), QString());
        QVERIFY(!backend.steps()[3].toMap()["pending"].toBool());
        QCOMPARE(backend.steps()[3].toMap()["line"].toInt(), 4);
    }

    void backendAnnouncesFinishedBuilds()
    {
        Backend backend("owner/repo");
        backend.setOffline(true);
        QSignalSpy finished(&backend, &Backend::buildFinished);
        backend.applyRuns({run(1, "running")});
        QCOMPARE(finished.size(), 0);
        backend.applyRuns({run(1, "success")});
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().first().toMap()["state"].toString(), QString("success"));
    }

    void backendOpensOnTheBuildOfTheCommitItWasHanded()
    {
        Backend backend("owner/repo");
        backend.setOffline(true);
        backend.applyRuns({run(1, "success", "aaaaaaa1111", 300), run(2, "success", "bbbbbbb2222", 200),
                           run(3, "failure", "bbbbbbb2222", 201, 60, "Linting")});
        QCOMPARE(backend.selectedKey(), QString("aaaaaaa1111"));
        backend.show("owner/repo", "bbbbbbb");
        // That build is picked, and the others stay around it.
        QCOMPARE(backend.selectedKey(), QString("bbbbbbb2222"));
        QCOMPARE(backend.builds().size(), 2);
        QCOMPARE(backend.selectedBuild()["state"].toString(), QString("failure"));
    }

    void backendRefusesAnUnknownRepositoryShape()
    {
        Backend backend("owner/repo");
        backend.setOffline(true);
        backend.show("not a repo; rm -rf");
        QCOMPARE(backend.repo(), QString("owner/repo"));
        QVERIFY(!backend.error().isEmpty());
    }

    void backendForecastsFromTheJobsOfEarlierBuilds()
    {
        Backend backend("owner/repo");
        backend.setOffline(true);
        // A build that started twenty seconds ago, and four that passed
        // in the hours before it.
        const auto now = QDateTime::currentDateTimeUtc();
        auto stamp = [&](int seconds) { return now.addSecs(seconds).toString(Qt::ISODate); };
        auto current = run(50, "running", "now0");
        current["startedAt"] = stamp(-20);
        current["updatedAt"] = stamp(0);
        QVariantList runs{current};
        for (int i = 1; i <= 4; ++i) {
            auto old = run(i, "success", QString("old%1").arg(i));
            old["startedAt"] = stamp(-i * 3600);
            old["updatedAt"] = stamp(-i * 3600 + 300 + i * 10);
            runs << old;
        }
        backend.applyRuns(runs);

        // Before any jobs are here: whole builds.
        QCOMPARE(backend.estimate()["known"].toBool(), true);
        QCOMPARE(backend.estimate()["basis"].toString(), QString("builds"));
        QCOMPARE(backend.estimate()["total"].toDouble(), 325.0);
        QCOMPARE(backend.history().size(), 5);
        QVERIFY(backend.history().last().toMap()["current"].toBool());
        QCOMPARE(backend.builds().first().toMap()["usual"].toDouble(), 325.0);

        auto timed = [&](QVariantMap entry, int start, int seconds) {
            entry["startedAt"] = stamp(start);
            entry["completedAt"] = seconds < 0 ? QString() : stamp(start + seconds);
            auto step = entry["steps"].toList().first().toMap();
            step["startedAt"] = entry["startedAt"];
            step["completedAt"] = entry["completedAt"];
            entry["steps"] = QVariantList{step};
            return entry;
        };
        for (int i = 1; i <= 4; ++i)
            backend.applyJobs(i, {timed(job(i * 10, "success", "test"), -i * 3600 + 5, 200),
                                  timed(job(i * 10 + 1, "success", "deploy"), -i * 3600 + 210, 80 + i * 10)});
        backend.applyJobs(50, {timed(job(500, "running", "test"), -15, -1)}, false);

        const auto estimate = backend.estimate();
        QCOMPARE(estimate["basis"].toString(), QString("jobs"));
        QCOMPARE(estimate["samples"].toInt(), 4);
        // test: 5 + 200, then deploy: its median of 90, 100, 110, 120.
        QVERIFY(qAbs(estimate["total"].toDouble() - (5 + 200 + 105)) < 2);
        QVERIFY(estimate["remaining"].toDouble() > 280);
        // deploy is on the list although GitHub hasn't created it.
        QCOMPARE(backend.jobs().size(), 2);
        QCOMPARE(backend.jobs()[1].toMap()["name"].toString(), QString("deploy"));
        QVERIFY(backend.jobs()[1].toMap()["expected"].toBool());
        // It can't be picked: there is nothing to show.
        backend.selectJob(backend.jobs()[1].toMap()["id"].toLongLong());
        QCOMPARE(backend.selectedJobId(), 500LL);
        // The running step knows how long it usually takes.
        QCOMPARE(backend.steps().first().toMap()["usual"].toDouble(), 200.0);
    }

    void backendLandsOnTheFailureOfARealJob()
    {
        Backend backend("acme/storefront");
        backend.setOffline(true);
        auto failed = run(45585201590LL, "failure", "4f1c2ab0e7d91c5a3b8f6240d17c9e2ab45f3068");
        failed["url"] = "https://github.com/acme/storefront/actions/runs/45585201590";
        backend.applyRuns({failed});
        backend.applyJobs(45585201590LL, GitHub::parseJobs(fixture("jobs.json")));
        QCOMPARE(backend.selectedJob()["name"].toString(), QString("test"));

        QSignalSpy reset(&backend, &Backend::logReset);
        backend.applyLog(139105231175LL, fixture("test-job.log"));
        QCOMPARE(reset.size(), 1);

        // By the time the window is told there is a log, it knows where to go.
        QCOMPARE(backend.failures().size(), 1);
        const auto failure = backend.failures().first().toMap();
        QCOMPARE(failure["location"].toString(), QString("spec/requests/admin/orders_spec.rb:107"));
        const auto &lines = backend.log()->lines();
        QVERIFY(lines[failure["line"].toInt()].text.contains("1) Admin::Orders"));
        QCOMPARE(backend.log()->data(backend.log()->index(failure["line"].toInt()), LogModel::MarkRole).toInt(),
                 int(LogModel::FailureLine));

        // Every step that ran is a place in the log; the failed one starts
        // where its command does, and the line there carries its name.
        const auto steps = backend.steps();
        QCOMPARE(steps.size(), 18);
        const auto rspec = steps[13].toMap();
        QVERIFY(lines[rspec["line"].toInt()].text.startsWith("Run bundle exec parallel_rspec"));
        QCOMPARE(backend.log()->data(backend.log()->index(rspec["line"].toInt()), LogModel::StepRole).toString(),
                 QString("RSpec tests"));
        QCOMPARE(rspec["seconds"].toInt(), 35);
        QCOMPARE(steps[14].toMap()["line"].toInt(), -1);

        // N and P walk the steps that have a place, skipping the skipped ones.
        QCOMPARE(backend.moveStep(2, 1), 4);
        QCOMPARE(backend.selectedStep(), 4);
        QCOMPARE(backend.moveStep(15, -1), 13);
    }

    void copiesTheLinkOfTheJobAndFallsBackToTheRun()
    {
        Backend backend("acme/storefront");
        backend.setOffline(true);
        auto failed = run(45585201590LL, "failure", "4f1c2ab0e7d91c5a3b8f6240d17c9e2ab45f3068");
        failed["url"] = "https://github.com/acme/storefront/actions/runs/45585201590";
        backend.applyRuns({failed});
        // No jobs yet: the run.
        QCOMPARE(backend.shareUrl(), QString("https://github.com/acme/storefront/actions/runs/45585201590"));

        backend.applyJobs(45585201590LL, GitHub::parseJobs(fixture("jobs.json")));
        const QString jobUrl = "https://github.com/acme/storefront/actions/runs/45585201590/job/139105231175";
        QCOMPARE(backend.shareUrl(), jobUrl);
        QCOMPARE(backend.copyLink(), jobUrl);
        QCOMPARE(QGuiApplication::clipboard()->text(), jobUrl);

        // Opening goes to the step that was picked; the link that is copied stays the job's.
        backend.applyLog(139105231175LL, fixture("test-job.log"));
        backend.selectStep(13);
        QCOMPARE(backend.openUrl(), jobUrl + "#step:14:1");
        QCOMPARE(backend.shareUrl(), jobUrl);

        // An address is made from the ids when the API gave none.
        QCOMPARE(GitHub::shareUrl("owner/repo", {{"id", 7}, {"runId", 3}}, {}),
                 QString("https://github.com/owner/repo/actions/runs/3/job/7"));
        QCOMPARE(GitHub::shareUrl("owner/repo", {}, {{"id", 3}}), QString("https://github.com/owner/repo/actions/runs/3"));
    }

    void copiesTheFailureForAnAgent()
    {
        Backend backend("acme/storefront");
        backend.setOffline(true);
        backend.applyRuns({run(45585201590LL, "failure", "4f1c2ab0e7d91c5a3b8f6240d17c9e2ab45f3068")});
        backend.applyJobs(45585201590LL, GitHub::parseJobs(fixture("jobs.json")));
        QVERIFY(!backend.copyFailure()); // No log yet, so nothing to copy.
        backend.applyLog(139105231175LL, fixture("test-job.log"));

        const auto report = backend.failureReport();
        const auto lines = report.split('\n');
        QCOMPARE(lines[0], QString("CI failed in acme/storefront: job \"test\", step \"RSpec tests\", on main at 4f1c2ab."));
        QCOMPARE(lines[1], QString("https://github.com/acme/storefront/actions/runs/45585201590/job/139105231175"));
        QVERIFY(report.contains("- spec/requests/admin/orders_spec.rb:107  Admin::Orders lists"));
        QVERIFY(report.contains("Failure/Error: expect(body.index("));
        QVERIFY(!report.contains("Process completed with exit code"));
        QVERIFY(backend.copyFailure());
        QCOMPARE(QGuiApplication::clipboard()->text(), report);
    }

    // --- The theme ---

    void themeFallsBack()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
        QVERIFY(!theme.light());
    }

    void themeReadsAccent()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        QFile colors(dir.path() + "/theme/colors.toml");
        QVERIFY(colors.open(QIODevice::WriteOnly));
        colors.write("accent = \"#112233\"\n");
        colors.close();

        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }

    void themeFollowsTheWholePaletteLightOnesToo()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        QFile colors(dir.path() + "/theme/colors.toml");
        QVERIFY(colors.open(QIODevice::WriteOnly));
        colors.write("mode = \"light\"\naccent = \"#1e66f5\"\nbackground = '#eff1f5'\nforeground = #4c4f69 # text\n"
                     "red = \"#d20f39\"\ngreen = \"#40a02b\"\n");
        colors.close();

        Theme theme(dir.path());
        QVERIFY(theme.light());
        QCOMPARE(theme.background(), QString("#eff1f5"));
        QCOMPARE(theme.foreground(), QString("#4c4f69"));
        QCOMPARE(theme.bad(), QColor("#d20f39"));
        QCOMPARE(theme.ok(), QColor("#40a02b"));
        // What the theme leaves out keeps the app's own colour.
        QCOMPARE(theme.warn(), QColor("#fbbf24"));
        // Surfaces sit between the background and the text.
        QVERIFY(theme.panel().lightnessF() < QColor("#eff1f5").lightnessF());
        QVERIFY(theme.panel().lightnessF() > theme.dim().lightnessF());
    }

    // --- The demo ---

    void theDemoBuildReallyRunsDeploysAndPasses()
    {
        auto jobsAt = [](int secondsAfterStart) {
            // The demo's running build began 192 seconds before the app did.
            const auto started = QDateTime::currentDateTimeUtc().addSecs(192 - secondsAfterStart);
            const auto build = GitHub::builds(GitHub::parseRuns(Demo::respond("repos/acme/storefront/actions/runs?per_page=100", started)))
                                   .first().toMap();
            QVariantMap states{{"build", build["state"]}};
            for (const auto &job : GitHub::parseJobs(Demo::respond("repos/acme/storefront/actions/runs/9028/jobs", started)))
                states[job.toMap()["name"].toString()] = job.toMap()["state"];
            return states;
        };
        auto states = jobsAt(200);
        QCOMPARE(states["build"].toString(), QString("running"));
        QCOMPARE(states["test"].toString(), QString("running"));
        QVERIFY(!states.contains("deploy")); // GitHub creates it when the others are done.

        // Its logs are there the way GitHub has them: a step's once the step
        // is done, the job's once the job is, and before that a 404.
        {
            const auto started = QDateTime::currentDateTimeUtc().addSecs(192 - 200);
            QVERIFY(Demo::respond("repos/acme/storefront/actions/jobs/90282/logs", started).isNull());
            QVERIFY(Demo::respond("repos/acme/storefront/actions/jobs/90282/steps/8/logs", started).isNull());
            const auto setUp = Demo::respond("repos/acme/storefront/actions/jobs/90282/steps/1/logs", started);
            QVERIFY(GitHub::parseLog(setUp).first().text.startsWith("Current runner version"));
            QVERIFY(!Demo::respond("repos/acme/storefront/actions/jobs/90281/logs", started).isNull());
        }
        states = jobsAt(360);
        QCOMPARE(states["test"].toString(), QString("success"));
        QCOMPARE(states["deploy"].toString(), QString("running"));
        states = jobsAt(600);
        QCOMPARE(states["build"].toString(), QString("success"));
        QCOMPARE(states["deploy"].toString(), QString("success"));
    }

    // --- The window ---

    void theWindowLoadsShowsTheDemoAndAnswersItsKeys()
    {
        QQuickStyle::setStyle("Basic");
        QTemporaryDir dir;
        Theme theme(dir.path());
        Backend backend("acme/storefront", true);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        backend.show("acme/storefront");

        // The running build is picked, with a forecast made from the jobs of
        // the earlier ones, and its log follows.
        QTRY_VERIFY(backend.builds().size() > 10);
        QCOMPARE(backend.selectedBuild()["state"].toString(), QString("running"));
        // The list sets main apart from the other branches, so it has to
        // know which branch main is.
        QCOMPARE(backend.property("mainBranch").toString(), QString("main"));
        QTRY_COMPARE(backend.estimate()["basis"].toString(), QString("jobs"));
        // Made of the steps that are done, with the one at work next.
        QTRY_VERIFY(backend.log()->rowCount() > 20);
        QTRY_COMPARE(backend.logPending(), QString("RSpec tests"));
        QVERIFY(window->property("headline").toString().startsWith("About "));
        bool deployExpected = false;
        for (const auto &job : backend.jobs())
            deployExpected |= job.toMap()["name"] == "deploy" && job.toMap()["expected"].toBool();
        QVERIFY(deployExpected);

        // J goes to the build that failed, which opens on its failures.
        QTest::keyClick(window, Qt::Key_J);
        QCOMPARE(backend.selectedBuild()["state"].toString(), QString("failure"));
        QTRY_COMPARE(backend.failures().size(), 2);
        QVERIFY(window->property("headline").toString().startsWith("Failed after "));
        QTRY_COMPARE(window->property("failureIndex").toInt(), 0);
        QTest::keyClick(window, Qt::Key_E);
        QCOMPARE(window->property("failureIndex").toInt(), 1);
        QTest::keyClick(window, Qt::Key_E, Qt::ShiftModifier);
        QCOMPARE(window->property("failureIndex").toInt(), 0);

        // C copies the address of the failed job.
        QTest::keyClick(window, Qt::Key_C);
        QCOMPARE(QGuiApplication::clipboard()->text(), QString("https://github.com/acme/storefront/actions/runs/9026/job/90262"));
        // Y copies the failure with that address on top of it.
        QTest::keyClick(window, Qt::Key_Y);
        QVERIFY(QGuiApplication::clipboard()->text().contains("OrderTotal includes tax in the total"));
        QVERIFY(QGuiApplication::clipboard()->text().contains("/job/90262"));

        // N walks the steps, and the picked step is what O would open.
        QTest::keyClick(window, Qt::Key_N);
        QVERIFY(backend.selectedStep() >= 0);
        QVERIFY(backend.openUrl().contains("#step:"));
        // It opens the step on its end, where the outcome is, at the bottom
        // of the view, and stops following the log.
        {
            auto *log = window->findChild<QObject *>("log");
            QVERIFY(log);
            QVariant end;
            QVERIFY(QMetaObject::invokeMethod(window, "stepEnd", Q_RETURN_ARG(QVariant, end),
                                              Q_ARG(QVariant, backend.selectedStep())));
            QVERIFY(end.toInt() > backend.steps()[backend.selectedStep()].toMap()["line"].toInt());
            QVERIFY(!log->property("follow").toBool());
            const auto lineAt = [log](qreal y) {
                int index = -1;
                QMetaObject::invokeMethod(log, "indexAt", Q_RETURN_ARG(int, index), Q_ARG(qreal, 20), Q_ARG(qreal, y));
                return index;
            };
            const qreal top = log->property("contentY").toReal();
            const qreal bottom = top + log->property("height").toReal() - 2;
            QTRY_VERIFY(lineAt(top) <= end.toInt() && end.toInt() <= lineAt(bottom));
        }

        // The failed pull request sits next to the main build before it, B goes there.
        QTRY_VERIFY(backend.previous()["ready"].toBool());
        QVERIFY(backend.previous()["notes"].toStringList().contains("test broke"));
        const auto failedKey = backend.selectedKey();
        QTest::keyClick(window, Qt::Key_B);
        QCOMPARE(backend.selectedKey(), backend.builds()[2].toMap()["key"].toString());
        QCOMPARE(backend.selectedBuild()["branch"].toString(), QString("main"));
        backend.selectBuild(failedKey);
        QTRY_COMPARE(backend.failures().size(), 2);

        // L moves to the next job, Z makes the log tall, ? shows the help and Esc closes it.
        const auto before = backend.selectedJobId();
        QTest::keyClick(window, Qt::Key_L);
        QVERIFY(backend.selectedJobId() != before);
        // W turns wrapping of long log lines off and on again.
        QVERIFY(window->property("wrap").toBool());
        QTest::keyClick(window, Qt::Key_W);
        QVERIFY(!window->property("wrap").toBool());
        QTest::keyClick(window, Qt::Key_W);
        QVERIFY(window->property("wrap").toBool());
        QTest::keyClick(window, Qt::Key_Z);
        QVERIFY(window->property("tall").toBool());
        QTest::keyClick(window, '?');
        QVERIFY(window->property("helpVisible").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(!window->property("helpVisible").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(!window->property("tall").toBool());

        // G opens the menu of repositories. What is typed there narrows the
        // list and never reaches the window's own keys: e would jump to a
        // failure and s would show the skipped builds.
        QTest::keyClick(window, Qt::Key_G);
        QVERIFY(window->property("repoMenuVisible").toBool());
        QTRY_VERIFY(backend.repositories().size() > 5);
        QCOMPARE(backend.repositories().last().toMap()["name"].toString(), QString("acme/checkout-v1")); // archived, last
        auto *menu = window->findChild<QObject *>("repositoryMenu");
        QVERIFY(menu);
        const auto failure = window->property("failureIndex").toInt();
        typeInto(window, "acme des");
        QCOMPARE(menu->property("shown").toList().size(), 1);
        QCOMPARE(window->property("failureIndex").toInt(), failure);
        QVERIFY(!backend.showSkipped());
        // Esc closes it on the repository it was on.
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(!window->property("repoMenuVisible").toBool());
        QCOMPARE(backend.repo(), QString("acme/storefront"));
        // A whole owner/name that isn't among them can be opened as typed.
        QTest::keyClick(window, Qt::Key_G);
        typeInto(window, "basecamp/omarchy");
        QCOMPARE(menu->property("typed").toString(), QString("basecamp/omarchy"));
        QVERIFY(menu->property("shown").toList().value(0).toMap()["typed"].toBool());
        // Opened again it starts empty; the arrows pick and Enter opens.
        QTest::keyClick(window, Qt::Key_Escape);
        QTest::keyClick(window, Qt::Key_G);
        QVERIFY(menu->property("typed").toString().isEmpty());
        typeInto(window, "acme/");
        QTest::keyClick(window, Qt::Key_Down);
        QCOMPARE(menu->property("index").toInt(), 1);
        QTest::keyClick(window, Qt::Key_Return);
        QVERIFY(!window->property("repoMenuVisible").toBool());
        QCOMPARE(backend.repo(), QString("acme/storefront-api"));
        QTRY_VERIFY(backend.builds().size() > 10);

        QTest::qWait(300);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }
};

QTEST_MAIN(OmaciTests)
#include "omaci_tests.moc"
