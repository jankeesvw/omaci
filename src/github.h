#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QVariantList>

// Turns GitHub Actions API responses into the plain maps QML shows. Kept free
// of processes and timers so the tests can feed it recorded responses.
namespace GitHub {

enum LineKind { Plain = 0, Group, Error, Warning, Command, Notice };

struct LogLine {
    QString time;
    QString text;
    int kind = Plain;
    // Milliseconds since the epoch, or 0 for a line without a timestamp. The
    // log's own clock is finer than the one the jobs API gives a step.
    qint64 ms = 0;
};

// One word for a run, job, or step: running, queued, success, failure,
// cancelled, skipped, or neutral.
QString state(const QString &status, const QString &conclusion);
bool isActive(const QString &state);

// Milliseconds since the epoch for an API timestamp, or 0 when it is empty.
qint64 ms(const QString &iso);
// Whole seconds between two API timestamps, never negative.
int secondsBetween(const QString &from, const QString &to);

QVariantList parseRuns(const QByteArray &json, QString *error = nullptr);
QVariantList parseJobs(const QByteArray &json, QString *error = nullptr);
// A page of the repositories the user can see, as `user/repos` gives them.
QVariantList parseRepositories(const QByteArray &json, QString *error = nullptr);
// Repositories with recently updated PRs by the requested author, newest first.
QVariantList parseAuthoredRepositories(const QByteArray &json, const QString &author, QString *error);
// Attribute runs using pull request numbers and exact head commits.
void attachPullAuthors(QVariantList &runs, const QVariantList &pullRequests);

// owner/name for the URL of a git remote on github.com, in any of the forms
// git accepts (scp-like, ssh://, https://), or empty for anything else.
QString repoFromRemote(const QString &url);
QList<LogLine> parseLog(const QByteArray &raw);

// A build is a commit: every run a push, pull request or dispatch started for
// it, rolled up. It is running while any of them is, red when any failed, and
// lasts from the first run starting to the last one ending. Runs started by
// anything else (a schedule, a comment) attach to the head of the branch too,
// so each of those stays an entry of its own rather than stretching a build.
QVariantList builds(const QVariantList &runs);

// Builds that were going before and are done now, so the app can tell the
// user without them watching the window.
QVariantList finishedBuilds(const QVariantList &before, const QVariantList &after);

// The line each step starts on in its job's log, in step order, or -1 for a
// step that wrote nothing (a skipped one) or whose start can't be found.
QList<int> mapSteps(const QVariantList &steps, const QList<LogLine> &lines);

// What to paste to somebody (or to a coding agent) to point at this job: the
// job's page, or the run's when no job is picked.
QString shareUrl(const QString &repo, const QVariantMap &job, const QVariantMap &run);

}
