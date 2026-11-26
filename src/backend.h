#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVariantList>
#include <functional>

#include "estimate.h"
#include "failures.h"
#include "logmodel.h"

// Watches the builds of one repository through the gh CLI, which already
// holds the user's login. A build is a commit with all its workflow runs
// rolled up. Polls quickly while one is running and slowly when all are done.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString repo READ repo NOTIFY repoChanged)
    Q_PROPERTY(QVariantList builds READ builds NOTIFY buildsChanged)
    Q_PROPERTY(QString selectedKey READ selectedKey NOTIFY selectionChanged)
    Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap selectedBuild READ selectedBuild NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList jobs READ jobs NOTIFY jobsChanged)
    Q_PROPERTY(qlonglong selectedJobId READ selectedJobId NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap selectedJob READ selectedJob NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList steps READ steps NOTIFY stepsChanged)
    Q_PROPERTY(int selectedStep READ selectedStep NOTIFY stepsChanged)
    Q_PROPERTY(LogModel *log READ log CONSTANT)
    Q_PROPERTY(QString logStatus READ logStatus NOTIFY logStatusChanged)
    // The step a running job's log stops at. GitHub publishes the log of a
    // step once the step is done and the log of a job once the job is, so a
    // running job's log is the steps that are done, and this one is next.
    Q_PROPERTY(QString logPending READ logPending NOTIFY logStatusChanged)
    Q_PROPERTY(QVariantList failures READ failures NOTIFY failuresChanged)
    Q_PROPERTY(QVariantMap estimate READ estimate NOTIFY estimateChanged)
    Q_PROPERTY(QVariantList history READ history NOTIFY historyChanged)
    Q_PROPERTY(QVariantMap previous READ previous NOTIFY previousChanged)
    Q_PROPERTY(QString cameFrom READ cameFrom NOTIFY selectionChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QDateTime lastUpdated READ lastUpdated NOTIFY buildsChanged)
    Q_PROPERTY(QString branchFilter READ branchFilter NOTIFY buildsChanged)
    // The repository's default branch, so the window can set its builds apart.
    Q_PROPERTY(QString mainBranch READ baseBranch NOTIFY buildsChanged)
    Q_PROPERTY(bool mainOnly READ mainOnly NOTIFY buildsChanged)
    Q_PROPERTY(bool runningOnly READ runningOnly NOTIFY buildsChanged)
    Q_PROPERTY(bool showSkipped READ showSkipped NOTIFY buildsChanged)
    Q_PROPERTY(int activeCount READ activeCount NOTIFY buildsChanged)
    // Every repository the gh login can see, most recently pushed first, for
    // the menu that picks one.
    Q_PROPERTY(QVariantList repositories READ repositories NOTIFY repositoriesChanged)
    Q_PROPERTY(bool repositoriesLoading READ repositoriesLoading NOTIFY repositoriesChanged)
    Q_PROPERTY(QString repositoriesError READ repositoriesError NOTIFY repositoriesChanged)

public:
    explicit Backend(const QString &repo, bool demo = false, QObject *parent = nullptr);

    // Never starts gh: the tests hand the backend its answers themselves.
    void setOffline(bool offline) { m_offline = offline; }

    QString repo() const { return m_repo; }
    QVariantList builds() const { return m_builds; }
    QString selectedKey() const { return m_key; }
    int selectedIndex() const;
    QVariantMap selectedBuild() const;
    QVariantList jobs() const { return m_jobs; }
    qlonglong selectedJobId() const { return m_jobId; }
    QVariantMap selectedJob() const;
    QVariantList steps() const { return m_steps; }
    int selectedStep() const { return m_step; }
    LogModel *log() { return &m_log; }
    QString logStatus() const { return m_logStatus; }
    QString logPending() const { return m_logPending; }
    QVariantList failures() const { return m_failures; }
    QVariantMap estimate() const { return m_estimate; }
    QVariantList history() const { return m_history; }
    // The build main had before the picked one, and how the two compare.
    QVariantMap previous() const { return m_previous; }
    QString cameFrom() const { return buildByKey(m_cameFrom)["title"].toString(); }
    QString error() const { return m_error; }
    bool busy() const { return m_running > 0; }
    QDateTime lastUpdated() const { return m_lastUpdated; }
    QString branchFilter() const { return m_branch; }
    bool mainOnly() const { return !m_branch.isEmpty() && m_branch == baseBranch(); }
    bool runningOnly() const { return m_runningOnly; }
    bool showSkipped() const { return m_showSkipped; }
    int activeCount() const;
    QVariantList repositories() const { return m_repositories; }
    bool repositoriesLoading() const { return m_repositoriesLoading; }
    QString repositoriesError() const { return m_repositoriesError; }

    // The repository a window opened without one shows: the one whose runs
    // came in last, in any earlier window.
    static QString rememberedRepo();

    Q_INVOKABLE void refresh();
    // Shows one repository, and with a sha the build of that commit selected,
    // which is how the bar widget hands over a build. Another repository
    // starts over. Without one, a window that shows a repository keeps it and
    // an empty one takes the remembered one, if there is one.
    Q_INVOKABLE void show(const QString &repo, const QString &sha = {});
    // Asks GitHub for the repositories the menu offers, a page at a time,
    // unless that was done in the last few minutes.
    Q_INVOKABLE void fetchRepositories();
    Q_INVOKABLE void selectBuild(const QString &key);
    Q_INVOKABLE void selectJob(qlonglong id);
    Q_INVOKABLE void selectStep(int index);
    Q_INVOKABLE void selectPrevious();
    // Back to the build `selectPrevious` was pressed on.
    Q_INVOKABLE void selectCameFrom();
    Q_INVOKABLE void moveBuild(int delta);
    Q_INVOKABLE void moveJob(int delta);
    // The next step, counted from `from`, that has a place in the log; makes
    // it the picked step and returns its index, or -1 when there is none.
    Q_INVOKABLE int moveStep(int from, int delta);
    Q_INVOKABLE void toggleBranch();
    // Only the builds of the default branch, and back to all.
    Q_INVOKABLE void toggleMain();
    // Only the builds that are running or waiting for a runner, and back.
    Q_INVOKABLE void toggleRunning();
    Q_INVOKABLE void toggleSkipped();

    // The address of the picked job, or of its run when no job is picked.
    Q_INVOKABLE QString shareUrl() const;
    // The same, on the picked step when there is one: what `O` opens.
    Q_INVOKABLE QString openUrl() const;
    // What failed, where, and the text of it: something to paste to an agent.
    Q_INVOKABLE QString failureReport() const;
    Q_INVOKABLE QString copyLink();
    Q_INVOKABLE bool copyFailure();
    Q_INVOKABLE void copyLog();

    // Apply responses the way a poll does. Public so tests can drive the
    // selection, the estimate and the notifications without a network.
    void applyRuns(const QVariantList &runs);
    void applyJobs(qlonglong runId, const QVariantList &jobs, bool final = true);
    void applyLog(qlonglong jobId, const QByteArray &raw, bool final = true);
    // The log of one step of a running job, by its number.
    void applyStepLog(qlonglong jobId, int number, const QByteArray &raw);

signals:
    void repoChanged();
    void buildsChanged();
    void jobsChanged();
    void stepsChanged();
    void selectionChanged();
    void logStatusChanged();
    void logReset();
    void failuresChanged();
    void estimateChanged();
    void historyChanged();
    void previousChanged();
    void errorChanged();
    void busyChanged();
    void repositoriesChanged();
    void buildFinished(const QVariantMap &build);

private:
    using Done = std::function<void(bool ok, const QByteArray &out, const QString &err)>;
    struct Call {
        QStringList args;
        Done done;
    };
    struct Kept {
        QByteArray etag;
        QByteArray body;
    };
    struct RunJobs {
        QVariantList jobs;
        bool final = false;
        QString updatedAt;
    };
    // The logs of the steps of one job that are in, by step number.
    struct StepLogs {
        QHash<int, QList<GitHub::LogLine>> lines;
        QSet<int> inFlight;
        // A step GitHub had no log for yet: how often, and when to ask again.
        QHash<int, int> misses;
        QHash<int, qint64> retryAt;
    };

    void gh(const QStringList &args, Done done);
    void pump();
    void api(const QString &path, Done done);
    void rebuild();
    void filterBuilds();
    void fetchCommit();
    void fetchJobs();
    void fetchRunJobs(qlonglong runId, bool final, const QString &updatedAt);
    void fetchLog();
    void fetchStepLogs(const QVariantMap &job);
    void showStepLogs(qlonglong jobId);
    void applyLines(qlonglong jobId, const QList<GitHub::LogLine> &lines, bool final);
    void rebuildJobs();
    void computeEstimate();
    void computePrevious();
    void fetchDefaultBranch();
    void fetchRepositoryPage(int page, const QVariantList &found, bool showPages);
    void setRepositories(const QVariantList &repositories);
    void remember(const QString &repo);
    QString baseBranch() const;
    void annotateLog();
    void pickJob();
    void clearJob();
    QVariantMap buildByKey(const QString &key) const;
    QList<Estimate::Earlier> earlierBuilds(const QVariantList &like) const;
    QHash<QString, double> usualSteps(const QString &jobKey) const;
    void setError(const QString &error);
    void setLogStatus(const QString &status, const QString &pending = {});
    void schedule();
    void notify(const QVariantMap &build);

    QString m_repo;
    QString m_gh;
    bool m_demo = false;
    bool m_offline = false;
    QDateTime m_started = QDateTime::currentDateTimeUtc();

    QList<Call> m_queue;
    int m_running = 0;
    QHash<QString, Kept> m_kept;

    QVariantList m_listRuns;
    QVariantList m_extraRuns;
    QVariantList m_allBuilds;
    QVariantList m_builds;
    QString m_key;
    QString m_wantCommit;
    QString m_commitFetched;

    QHash<qlonglong, RunJobs> m_jobsOfRun;
    QSet<qlonglong> m_jobsInFlight;
    QVariantList m_realJobs;
    QVariantList m_jobs;
    QVariantList m_like;
    qlonglong m_jobId = 0;
    bool m_jobPinned = false;
    QVariantList m_steps;
    int m_step = -1;

    qlonglong m_logJobId = 0;
    bool m_logFinal = false;
    bool m_fetchingRuns = false;
    bool m_fetchingLog = false;
    LogModel m_log;
    QString m_logStatus;
    QString m_logPending;
    QHash<qlonglong, StepLogs> m_stepLogs;
    QList<Failures::Failure> m_found;
    QVariantList m_failures;

    QVariantMap m_estimate;
    QVariantMap m_previous;
    QString m_defaultBranch;
    QString m_defaultBranchOf;
    QString m_cameFrom;
    QVariantList m_history;
    QString m_error;
    QDateTime m_lastUpdated;
    QString m_branch;
    bool m_showSkipped = false;
    bool m_runningOnly = false;
    QVariantList m_repositories;
    bool m_repositoriesLoading = false;
    QString m_repositoriesError;
    QDateTime m_repositoriesFetched;
    QString m_remembered;
    QTimer m_poll;
    QTimer m_tick;
};
