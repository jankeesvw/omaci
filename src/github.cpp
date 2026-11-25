#include "github.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>
#include <limits>

namespace GitHub {

QString state(const QString &status, const QString &conclusion) {
    if (status == "in_progress")
        return "running";
    if (status != "completed")
        return "queued";
    if (conclusion == "success")
        return "success";
    if (conclusion == "failure" || conclusion == "timed_out" || conclusion == "startup_failure")
        return "failure";
    if (conclusion == "cancelled")
        return "cancelled";
    if (conclusion == "skipped")
        return "skipped";
    if (conclusion == "action_required")
        return "queued";
    return "neutral";
}

bool isActive(const QString &state) {
    return state == "running" || state == "queued";
}

qint64 ms(const QString &iso) {
    if (iso.isEmpty())
        return 0;
    const auto when = QDateTime::fromString(iso, Qt::ISODateWithMs);
    return when.isValid() ? when.toMSecsSinceEpoch() : 0;
}

int secondsBetween(const QString &from, const QString &to) {
    const auto a = ms(from), b = ms(to);
    if (!a || !b)
        return 0;
    return int(qMax<qint64>(0, (b - a + 500) / 1000));
}

static QJsonArray arrayIn(const QByteArray &json, const char *key, QString *error) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = QString("GitHub sent a response that isn't JSON: %1").arg(parseError.errorString());
        return {};
    }
    return document.object().value(key).toArray();
}

static bool isPullRequestEvent(const QString &event) {
    return event == "pull_request" || event == "pull_request_target" ||
           event == "pull_request_review" || event == "pull_request_review_comment";
}

QVariantList parseRuns(const QByteArray &json, QString *error) {
    QVariantList runs;
    for (const auto &value : arrayIn(json, "workflow_runs", error)) {
        const auto run = value.toObject();
        const auto status = run["status"].toString();
        const auto conclusion = run["conclusion"].toString();
        const auto started = run["run_started_at"].toString();
        const auto event = run["event"].toString();
        const auto repositoryId = run["repository"].toObject()["id"].toVariant().toLongLong();
        const auto headRepositoryId = run["head_repository"].toObject()["id"].toVariant().toLongLong();
        const auto branch = run["head_branch"].toString();
        const auto sha = run["head_sha"].toString();
        const bool pullEvent = isPullRequestEvent(event);
        QVariantList pullNumbers;
        bool onPullHead = event == "pull_request_target" && repositoryId && headRepositoryId &&
                          headRepositoryId != repositoryId && !branch.isEmpty() && !sha.isEmpty();
        if (repositoryId && (pullEvent || event == "push" || event == "workflow_dispatch")) {
            for (const auto &value : run["pull_requests"].toArray()) {
                const auto pull = value.toObject();
                const auto base = pull["base"].toObject();
                const auto baseId = base["repo"].toObject()["id"].toVariant().toLongLong();
                const auto number = pull["number"].toInt();
                if (baseId != repositoryId || number <= 0)
                    continue;
                const auto head = pull["head"].toObject();
                const auto pullHeadRepo = head["repo"].toObject()["id"].toVariant().toLongLong();
                const bool sameBranch = headRepositoryId && pullHeadRepo == headRepositoryId &&
                                        !branch.isEmpty() && head["ref"].toString() == branch;
                if (!pullEvent && !sameBranch)
                    continue;
                if (!pullNumbers.contains(number))
                    pullNumbers << number;
                const auto baseRef = base["ref"].toString();
                const bool differentFromBase = headRepositoryId != baseId || (!baseRef.isEmpty() && baseRef != branch);
                // GitHub can update the embedded PR head after a run was created.
                onPullHead |= sameBranch && !sha.isEmpty() && (head["sha"].toString() == sha || differentFromBase);
            }
        }
        runs << QVariantMap{
            {"id", run["id"].toVariant().toLongLong()},
            {"workflow", run["name"].toString()},
            {"title", run["display_title"].toString()},
            {"branch", branch},
            {"event", event},
            {"actor", run["actor"].toObject()["login"].toString()},
            {"pullNumbers", pullNumbers},
            {"headRepositoryId", headRepositoryId},
            {"onPullHead", onPullHead},
            {"number", run["run_number"].toInt()},
            {"attempt", run["run_attempt"].toInt()},
            {"sha", sha.left(7)},
            {"fullSha", sha},
            {"url", run["html_url"].toString()},
            {"status", status},
            {"conclusion", conclusion},
            {"state", state(status, conclusion)},
            {"startedAt", started.isEmpty() ? run["created_at"].toString() : started},
            {"updatedAt", run["updated_at"].toString()},
        };
    }
    return runs;
}

QVariantList parseJobs(const QByteArray &json, QString *error) {
    QVariantList jobs;
    for (const auto &value : arrayIn(json, "jobs", error)) {
        const auto job = value.toObject();
        QVariantList steps;
        for (const auto &stepValue : job["steps"].toArray()) {
            const auto step = stepValue.toObject();
            steps << QVariantMap{
                {"number", step["number"].toInt()},
                {"name", step["name"].toString()},
                {"state", state(step["status"].toString(), step["conclusion"].toString())},
                {"startedAt", step["started_at"].toString()},
                {"completedAt", step["completed_at"].toString()},
            };
        }
        jobs << QVariantMap{
            {"id", job["id"].toVariant().toLongLong()},
            {"runId", job["run_id"].toVariant().toLongLong()},
            {"name", job["name"].toString()},
            {"workflow", job["workflow_name"].toString()},
            {"state", state(job["status"].toString(), job["conclusion"].toString())},
            {"url", job["html_url"].toString()},
            {"startedAt", job["started_at"].toString()},
            {"completedAt", job["completed_at"].toString()},
            {"steps", steps},
        };
    }
    return jobs;
}

static QJsonArray parseList(const QByteArray &json, const QString &name, QString *error) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        if (error)
            *error = QString("GitHub sent an invalid list of %1: %2").arg(name, parseError.errorString());
        return {};
    }
    return document.array();
}

QVariantList parseRepositories(const QByteArray &json, QString *error) {
    QVariantList repositories;
    for (const auto &value : parseList(json, "repositories", error)) {
        const auto repo = value.toObject();
        const auto owner = repo["owner"].toObject();
        repositories << QVariantMap{
            {"name", repo["full_name"].toString()},
            {"description", repo["description"].toString()},
            {"private", repo["private"].toBool()},
            {"archived", repo["archived"].toBool()},
            {"disabled", repo["disabled"].toBool()},
            {"defaultBranch", repo["default_branch"].toString()},
            {"organization", owner["type"].toString() == "Organization" ? owner["login"].toString() : QString()},
            {"pushedAt", repo["pushed_at"].toString()},
        };
    }
    return repositories;
}

QVariantList parseAuthoredRepositories(const QByteArray &json, const QString &author, QString *error) {
    const auto data = QJsonDocument::fromJson(json).object().value("data").toObject();
    const auto search = data.value("search").toObject();
    const auto result = search.value("nodes");
    if (!result.isArray()) {
        *error = "GitHub sent an invalid response to the PR author search.";
        return {};
    }
    QVariantList repositories;
    QHash<QString, qsizetype> indexes;
    for (const auto &value : result.toArray()) {
        const auto pull = value.toObject();
        const auto repo = pull["repository"].toObject();
        const auto name = repo["nameWithOwner"].toString();
        const auto login = pull["author"].toObject()["login"].toString();
        if (name.isEmpty() || login.compare(author, Qt::CaseInsensitive) != 0 ||
            repo["isArchived"].toBool() || repo["isDisabled"].toBool())
            continue;
        if (!indexes.contains(name)) {
            indexes.insert(name, repositories.size());
            repositories << QVariantMap{{"name", name},
                                        {"defaultBranch", repo["defaultBranchRef"].toObject()["name"].toString()}};
        }
        auto &item = repositories[indexes.value(name)];
        auto repository = item.toMap();
        auto pulls = repository["pullRequests"].toList();
        pulls << QVariantMap{{"number", pull["number"].toInt()},
                             {"login", login},
                             {"headSha", pull["headRefOid"].toString()},
                             {"headBranch", pull["headRefName"].toString()},
                             {"headRepositoryId", pull["headRepository"].toObject()["databaseId"].toVariant().toLongLong()}};
        repository["pullRequests"] = pulls;
        item = repository;
    }
    return repositories;
}

void attachPullAuthors(QVariantList &runs, const QVariantList &pullRequests) {
    struct PullHead {
        QString branch;
        qlonglong repositoryId;
        QString author;
    };
    QHash<int, QString> byNumber;
    QMultiHash<QString, PullHead> byHead;
    for (const auto &value : pullRequests) {
        const auto pull = value.toMap();
        const auto author = pull["login"].toString();
        const auto sha = pull["headSha"].toString();
        if (author.isEmpty())
            continue;
        byNumber.insert(pull["number"].toInt(), author);
        if (!sha.isEmpty())
            byHead.insert(sha, {pull["headBranch"].toString(), pull["headRepositoryId"].toLongLong(), author});
    }
    for (auto &value : runs) {
        auto run = value.toMap();
        QStringList authors;
        const auto numbers = run["pullNumbers"].toList();
        for (const auto &number : numbers) {
            const auto author = byNumber.value(number.toInt());
            if (!author.isEmpty())
                authors << author;
        }
        // Fork PR runs can have an empty pull_requests array. The commit is
        // also how branch pushes are linked to their PR.
        const auto event = run["event"].toString();
        if (numbers.isEmpty() && (isPullRequestEvent(event) || event == "push" || event == "workflow_dispatch")) {
            const auto repositoryId = run["headRepositoryId"].toLongLong();
            for (const auto &head : byHead.values(run["fullSha"].toString())) {
                if (repositoryId && repositoryId == head.repositoryId && head.branch == run["branch"]) {
                    authors << head.author;
                    run["onPullHead"] = true;
                }
            }
        }
        authors.removeDuplicates();
        authors.sort(Qt::CaseInsensitive);
        run["pullAuthors"] = authors;
        value = run;
    }
}

QString repoFromRemote(const QString &url) {
    static const QRegularExpression remote(
        R"(^(?:[a-z+]+://)?(?:[^@/]+@)?github\.com(?::\d+)?[:/]([A-Za-z0-9_.-]+)/([A-Za-z0-9_.-]+?)(?:\.git)?/?$)");
    const auto match = remote.match(url.trimmed());
    return match.hasMatch() ? match.captured(1) + "/" + match.captured(2) : QString();
}

QList<LogLine> parseLog(const QByteArray &raw) {
    static const QRegularExpression ansi(R"(\x1b\[[0-9;?]*[A-Za-z])");
    static const QRegularExpression stamp(R"(^(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(?:\.\d+)?Z) ?)");
    static const QList<QPair<QString, int>> markers{
        {"##[group]", Group},     {"##[error]", Error},   {"##[warning]", Warning},
        {"##[command]", Command}, {"##[notice]", Notice}, {"##[section]", Group},
        // Some actions, actions/checkout among them, echo commands without the ##.
        {"[command]", Command},
    };

    QString text = QString::fromUtf8(raw);
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    text.remove(ansi);
    text.remove('\r');

    QList<LogLine> lines;
    for (const auto &rawLine : text.split('\n')) {
        LogLine line;
        QString rest = rawLine;
        const auto match = stamp.match(rest);
        if (match.hasMatch()) {
            const auto when = QDateTime::fromString(match.captured(1), Qt::ISODateWithMs);
            if (when.isValid()) {
                line.time = when.toLocalTime().toString("HH:mm:ss");
                line.ms = when.toMSecsSinceEpoch();
            }
            rest = rest.mid(match.capturedLength());
        }
        if (rest.startsWith("##[endgroup]"))
            continue;
        for (const auto &[prefix, kind] : markers) {
            if (rest.startsWith(prefix)) {
                rest = rest.mid(prefix.size());
                line.kind = kind;
                break;
            }
        }
        line.text = rest;
        lines << line;
    }
    // The log ends in a newline, which split() turns into one empty line.
    if (!lines.isEmpty() && lines.last().text.isEmpty() && lines.last().time.isEmpty())
        lines.removeLast();
    return lines;
}

static bool isBuildEvent(const QString &event) {
    static const QStringList events{"push", "pull_request", "pull_request_target", "workflow_dispatch",
                                    "merge_group"};
    return events.contains(event);
}

// The state of several runs taken together. Skipped runs say nothing unless
// every run was skipped.
static QString rollUp(const QVariantList &runs, bool *failing) {
    int running = 0, queued = 0, failed = 0, cancelled = 0, done = 0, counted = 0;
    for (const auto &value : runs) {
        const auto state = value.toMap()["state"].toString();
        if (state == "skipped")
            continue;
        ++counted;
        running += state == "running";
        queued += state == "queued";
        failed += state == "failure";
        cancelled += state == "cancelled";
        done += !isActive(state);
    }
    *failing = failed > 0;
    if (!counted)
        return "skipped";
    if (running || (queued && done))
        return "running";
    if (queued)
        return "queued";
    if (failed)
        return "failure";
    if (cancelled)
        return "cancelled";
    return "success";
}

QVariantList builds(const QVariantList &runs) {
    QStringList order;
    QHash<QString, QVariantList> grouped;
    for (const auto &value : runs) {
        const auto run = value.toMap();
        auto key = isBuildEvent(run["event"].toString()) && !run["fullSha"].toString().isEmpty()
                       ? run["fullSha"].toString()
                       : QString("run:%1").arg(run["id"].toLongLong());
        const auto repository = run["repository"].toString();
        if (!repository.isEmpty()) {
            // Some pull_request_target runs identify the PR head. When the
            // relationship is unproven, the SHA can be a shared base commit.
            if (run["event"] == "pull_request_target" && !run["onPullHead"].toBool()) {
                const auto numbers = run["pullNumbers"].toList();
                key += numbers.size() == 1 ? QString(":pull:%1").arg(numbers.first().toInt())
                                           : QString(":run:%1").arg(run["id"].toLongLong());
            } else if (!key.startsWith("run:")) {
                key += QString(":%1:%2").arg(run["headRepositoryId"].toLongLong()).arg(run["branch"].toString());
            }
            key.prepend(repository + ':');
        }
        if (!grouped.contains(key))
            order << key;
        grouped[key] << run;
    }

    QVariantList out;
    for (const auto &key : order) {
        auto members = grouped[key];
        // Oldest first, so the jobs of a build read in the order they started.
        std::stable_sort(members.begin(), members.end(), [](const QVariant &a, const QVariant &b) {
            return ms(a.toMap()["startedAt"].toString()) < ms(b.toMap()["startedAt"].toString());
        });
        bool failing = false;
        const auto state = rollUp(members, &failing);

        QVariantMap lead;
        QString started, ended;
        QStringList workflows;
        QStringList pullAuthors;
        QVariantList ids;
        for (const auto &value : members) {
            const auto run = value.toMap();
            ids << run["id"];
            pullAuthors << run["pullAuthors"].toStringList();
            const bool skipped = run["state"] == "skipped" && state != "skipped";
            if (skipped)
                continue;
            if (!workflows.contains(run["workflow"].toString()))
                workflows << run["workflow"].toString();
            // The run to open and to name the build after: the one that
            // failed, else the one still going, else the first.
            const auto runState = run["state"].toString();
            if (lead.isEmpty() || (runState == "failure" && lead["state"] != "failure") ||
                (isActive(runState) && !isActive(lead["state"].toString()) && lead["state"] != "failure"))
                lead = run;
            const auto from = run["startedAt"].toString(), to = run["updatedAt"].toString();
            if (started.isEmpty() || ms(from) < ms(started))
                started = from;
            if (ended.isEmpty() || ms(to) > ms(ended))
                ended = to;
        }
        const bool active = isActive(state);
        const auto first = members.first().toMap();
        pullAuthors.removeDuplicates();
        pullAuthors.sort(Qt::CaseInsensitive);
        out << QVariantMap{
            {"key", key},
            {"isBuild", isBuildEvent(first["event"].toString()) && !first["fullSha"].toString().isEmpty()},
            {"repository", first["repository"].toString()},
            {"defaultBranch", first["defaultBranch"].toString()},
            {"pullAuthors", pullAuthors},
            {"sha", first["sha"]},
            {"fullSha", first["fullSha"]},
            {"title", lead["title"]},
            {"branch", lead["branch"]},
            {"event", lead["event"]},
            {"actor", lead["actor"]},
            {"url", lead["url"]},
            {"state", state},
            {"failing", failing && active},
            {"startedAt", started},
            {"endedAt", active ? QString() : ended},
            {"seconds", active ? 0 : secondsBetween(started, ended)},
            {"workflows", workflows},
            {"runIds", ids},
            {"runs", members},
        };
    }
    return out;
}

QVariantList finishedBuilds(const QVariantList &before, const QVariantList &after) {
    QHash<QString, QString> previous;
    for (const auto &build : before)
        previous.insert(build.toMap()["key"].toString(), build.toMap()["state"].toString());

    QVariantList finished;
    for (const auto &value : after) {
        const auto build = value.toMap();
        const auto key = build["key"].toString();
        const auto now = build["state"].toString();
        if (previous.contains(key) && isActive(previous[key]) && !isActive(now) && now != "skipped")
            finished << build;
    }
    return finished;
}

namespace {

enum StepKind { SetUp, Containers, StopContainers, Pre, Post, Complete, User };

StepKind stepKind(const QString &name, bool first) {
    if (first || name == "Set up job")
        return SetUp;
    if (name == "Initialize containers")
        return Containers;
    if (name == "Stop containers")
        return StopContainers;
    if (name == "Complete job")
        return Complete;
    if (name.startsWith("Post "))
        return Post;
    if (name.startsWith("Pre "))
        return Pre;
    return User;
}

bool marks(const LogLine &line, StepKind kind) {
    switch (kind) {
    case User: return line.kind == Group && line.text.startsWith("Run ");
    case Post: return line.text == "Post job cleanup.";
    case Containers: return line.kind == Group && line.text == "Checking docker version";
    case StopContainers:
        return line.text.startsWith("Print service container logs") ||
               line.text.startsWith("Stop and remove container");
    case Complete:
        return line.text == "Cleaning up orphan processes" || line.text == "Evaluate and set job outputs";
    // A pre step writes no line of its own that says it starts.
    case Pre:
    case SetUp: return false;
    }
    return false;
}

}

QList<int> mapSteps(const QVariantList &steps, const QList<LogLine> &lines) {
    QList<int> starts(steps.size(), -1);
    // The API gives a step's times in whole seconds, cut off rather than
    // rounded, so a step that "ended at :08" may have written until :08.999.
    static constexpr qint64 Second = 1000;
    int cursor = 0;
    for (int i = 0; i < steps.size() && cursor < lines.size(); ++i) {
        const auto step = steps[i].toMap();
        const auto state = step["state"].toString();
        if (state == "skipped" || state == "queued")
            continue;
        const auto kind = stepKind(step["name"].toString(), i == 0);
        if (kind == SetUp) {
            starts[i] = 0;
            cursor = 1;
            continue;
        }
        const qint64 from = ms(step["startedAt"].toString());
        const qint64 until = ms(step["completedAt"].toString());
        if (!from)
            continue;
        const qint64 earliest = from;
        const qint64 latest = until ? until + Second : std::numeric_limits<qint64>::max();

        int found = -1, fallback = -1;
        for (int at = cursor; at < lines.size(); ++at) {
            const auto &line = lines.at(at);
            if (!line.ms)
                continue;
            if (line.ms >= latest)
                break;
            if (line.ms < earliest)
                continue;
            if (fallback < 0 && line.ms >= from)
                fallback = at;
            if (marks(line, kind)) {
                found = at;
                break;
            }
        }
        if (found < 0)
            found = fallback;
        if (found < 0)
            continue;
        starts[i] = found;
        cursor = found + 1;
    }
    return starts;
}

QString shareUrl(const QString &repo, const QVariantMap &job, const QVariantMap &run) {
    const auto jobUrl = job["url"].toString();
    if (!jobUrl.isEmpty())
        return jobUrl;
    const auto runId = job["runId"].toLongLong() ? job["runId"].toLongLong() : run["id"].toLongLong();
    const auto jobId = job["id"].toLongLong();
    if (runId && jobId > 0)
        return QString("https://github.com/%1/actions/runs/%2/job/%3").arg(repo).arg(runId).arg(jobId);
    const auto runUrl = run["url"].toString();
    if (!runUrl.isEmpty())
        return runUrl;
    if (runId)
        return QString("https://github.com/%1/actions/runs/%2").arg(repo).arg(runId);
    return {};
}

}
