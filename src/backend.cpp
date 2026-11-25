#include "backend.h"

#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>
#include <limits>
#include <utility>

#include "compare.h"
#include "demo.h"
#include "github.h"

// Poll every few seconds while a build is moving, and back off when it's
// quiet. The list of runs and the jobs are asked for with the ETag of the
// last answer, and GitHub doesn't count "not modified" against the hourly
// limit, so most polls cost one request: the log.
static constexpr int ActiveInterval = 6000;
static constexpr int IdleInterval = 60000;
static constexpr int MaxProcesses = 4;
static constexpr int OrganizationConcurrency = MaxProcesses;
static constexpr int OrganizationRunsPerRepo = 30;
static constexpr int OrganizationBatchSize = 24;
static constexpr int OrganizationPublishInterval = 1000;
static constexpr int OrganizationActiveInterval = 15000;
static constexpr int OrganizationCatalogInterval = 5 * 60 * 1000;
static constexpr int OrganizationQuietInterval = 10 * 60 * 1000;
static constexpr int RecentActivity = 24 * 60 * 60 * 1000;
// How many earlier builds' jobs the forecast is made from. Each costs a
// request per run, once.
static constexpr int EarlierBuilds = 6;

static QString findGh() {
    auto path = QStandardPaths::findExecutable("gh");
    if (!path.isEmpty())
        return path;
    // The launcher may start the app without the shell's PATH, and mise keeps
    // gh outside the system directories.
    for (const auto &candidate : {QDir::homePath() + "/.local/share/mise/shims/gh",
                                  QDir::homePath() + "/.local/bin/gh"})
        if (QFileInfo(candidate).isExecutable())
            return candidate;
    return {};
}

static QString jobKey(const QVariantMap &job, const QVariantMap &run) {
    const auto workflow = job["workflow"].toString().isEmpty() ? run["workflow"].toString()
                                                                : job["workflow"].toString();
    return workflow + "/" + job["name"].toString();
}

static double offset(const QString &iso, qint64 origin) {
    const auto at = GitHub::ms(iso);
    return at && origin ? qMax(0.0, (at - origin) / 1000.0) : -1;
}

static QString stateFile() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + "/omaci/state.ini";
}

static bool validPullAuthor(const QString &login) {
    static const QRegularExpression pattern(R"(^[A-Za-z0-9][A-Za-z0-9_-]{0,99}(?:\[bot\])?$)",
                                             QRegularExpression::CaseInsensitiveOption);
    return login.isEmpty() || pattern.match(login).hasMatch();
}

struct HttpReply {
    int status = 0;
    QByteArray headers;
    QByteArray body;

    QByteArray header(const QByteArray &name) const {
        for (const auto &line : headers.split('\n')) {
            const auto separator = line.indexOf(':');
            if (separator > 0 && line.left(separator).toLower() == name)
                return line.mid(separator + 1).trimmed();
        }
        return {};
    }
};

static HttpReply httpReply(const QByteArray &output) {
    if (!output.startsWith("HTTP/"))
        return {0, {}, output};
    auto split = output.indexOf("\r\n\r\n");
    const int separatorLength = split >= 0 ? 4 : 2;
    if (split < 0)
        split = output.indexOf("\n\n");
    const auto headers = split < 0 ? output : output.left(split);
    const auto status = headers.left(headers.indexOf('\n')).split(' ').value(1).toInt();
    return {status, headers, split < 0 ? QByteArray() : output.mid(split + separatorLength)};
}

Backend::Backend(const QString &repo, bool demo, QObject *parent)
    : QObject(parent), m_repo(repo), m_gh(findGh()), m_demo(demo) {
    m_poll.setSingleShot(true);
    connect(&m_poll, &QTimer::timeout, this, [this] {
        refreshRuns(std::exchange(m_orgRefreshRequested, false));
    });
    m_requestWake.setSingleShot(true);
    connect(&m_requestWake, &QTimer::timeout, this, [this] {
        pump();
        emit organizationProgressChanged();
    });
    m_orgPublishTimer.setSingleShot(true);
    connect(&m_orgPublishTimer, &QTimer::timeout, this, [this] {
        if (m_orgPoll.receivedRuns > 0)
            markUpdated();
        flushOrganizationChanges();
    });
    // The time left changes with the clock, not only with what GitHub says.
    m_tick.setInterval(1000);
    connect(&m_tick, &QTimer::timeout, this, &Backend::computeEstimate);
    connect(this, &Backend::buildFinished, this, &Backend::notify);
}

int Backend::selectedIndex() const {
    for (int i = 0; i < m_builds.size(); ++i)
        if (m_builds[i].toMap()["key"] == m_key)
            return i;
    return -1;
}

QVariantMap Backend::selectedBuild() const {
    const int index = selectedIndex();
    return index < 0 ? QVariantMap{} : m_builds[index].toMap();
}

QVariantMap Backend::buildByKey(const QString &key) const {
    for (const auto &build : m_allBuilds)
        if (build.toMap()["key"] == key)
            return build.toMap();
    return {};
}

bool Backend::hasVisibleBuild(const QString &key) const {
    return std::any_of(m_builds.cbegin(), m_builds.cend(),
                       [&key](const QVariant &build) { return build.toMap()["key"] == key; });
}

QVariantList Backend::comparisonBuilds(const QVariantMap &build) const {
    return m_buildsByRepository.value(build["repository"].toString());
}

QVariantMap Backend::selectedJob() const {
    for (const auto &job : m_jobs)
        if (job.toMap()["id"].toLongLong() == m_jobId)
            return job.toMap();
    return {};
}

int Backend::activeCount() const {
    int count = 0;
    for (const auto &value : m_allBuilds) {
        const auto build = value.toMap();
        count += GitHub::isActive(build["state"].toString()) && matchesPullAuthor(build);
    }
    return count;
}

bool Backend::matchesPullAuthor(const QVariantMap &build) const {
    return m_pullAuthor.isEmpty() || build["pullAuthors"].toStringList().contains(m_pullAuthor, Qt::CaseInsensitive);
}

QString Backend::organizationProgress() const {
    if (organization().isEmpty())
        return {};
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto rest = requestAvailableAt(false);
    const auto search = !m_pullAuthor.isEmpty() && m_orgPoll.repositories.isEmpty() ? requestAvailableAt(true) : 0;
    const auto retry = qMax(rest, search);
    if (m_fetchingRuns && retry > now)
        return QString("GitHub rate limit · retry at %1").arg(QDateTime::fromMSecsSinceEpoch(retry).toString("HH:mm:ss"));
    if (!m_orgManualRemaining.isEmpty())
        return QString("Checking repositories %1/%2").arg(m_orgManualTotal - m_orgManualRemaining.size()).arg(m_orgManualTotal);
    if (m_pullAuthor.isEmpty() && m_orgActivity.size() < m_orgCatalog.size())
        return QString("Checking repositories %1/%2").arg(m_orgActivity.size()).arg(m_orgCatalog.size());
    if (!m_fetchingRuns)
        return {};
    if (m_orgPoll.repositories.isEmpty())
        return m_pullAuthor.isEmpty() ? "Looking up repositories…" : "Looking up pull requests…";
    return QString("Checking repositories %1/%2").arg(m_orgPoll.next - m_orgPoll.pending).arg(m_orgPoll.repositories.size());
}

void Backend::gh(const QStringList &args, Done done, quint64 organizationPoll) {
    if (m_demo) {
        // Answered a moment later, the way a real call is, so the window goes
        // through the same states.
        const auto out = Demo::respond(args.last(), m_started);
        QTimer::singleShot(40, this, [done, out] { done(true, out, {}); });
        return;
    }
    if (m_offline)
        return;
    if (m_gh.isEmpty()) {
        done(false, {}, "The GitHub CLI (gh) isn't installed. Install it with `omarchy-pkg-add github-cli` "
                        "and log in with `gh auth login`.");
        return;
    }
    m_queue.append({args, done, organizationPoll});
    pump();
}

// A handful of gh processes at a time: opening a build can ask for the jobs
// of a dozen earlier runs at once.
void Backend::pump() {
    m_requestWake.stop();
    m_queue.removeIf([this](const Call &call) {
        return call.organizationPoll && call.organizationPoll != m_orgPollId;
    });
    while (m_running < MaxProcesses && !m_queue.isEmpty()) {
        const auto now = QDateTime::currentMSecsSinceEpoch();
        qint64 wake = std::numeric_limits<qint64>::max();
        auto next = std::find_if(m_queue.begin(), m_queue.end(), [this, now, &wake](const Call &call) {
            const auto available = requestAvailableAt(call.args.value(1) == "graphql");
            wake = qMin(wake, available);
            return available <= now;
        });
        if (next == m_queue.end()) {
            m_requestWake.start(int(std::clamp<qint64>(wake - now, 1, std::numeric_limits<int>::max())));
            return;
        }
        const auto call = *next;
        m_queue.erase(next);
        auto *process = new QProcess(this);
        ++m_running;
        emit busyChanged();
        auto finish = [this, process, done = call.done, graphql = call.args.value(1) == "graphql",
                       includedHeaders = call.args.contains("--include")]
                      (bool ok, const QString &err) {
            const auto out = process->readAllStandardOutput();
            process->deleteLater();
            --m_running;
            noteRateLimit(graphql, includedHeaders ? out : QByteArray(), err);
            emit busyChanged();
            done(ok, out, err);
            pump();
        };
        connect(process, &QProcess::finished, this, [process, finish](int code, QProcess::ExitStatus status) {
            const auto err = QString::fromUtf8(process->readAllStandardError()).trimmed();
            finish(status == QProcess::NormalExit && code == 0, err);
        });
        connect(process, &QProcess::errorOccurred, this, [this, process, finish](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                finish(false, QString("Couldn't start %1: %2").arg(m_gh, process->errorString()));
        });
        process->start(m_gh, call.args);
    }
}

qint64 Backend::requestAvailableAt(bool graphql) const {
    return qMax(m_secondaryRetryAt, graphql ? m_graphqlRetryAt : m_restRetryAt);
}

void Backend::noteRateLimit(bool graphql, const QByteArray &output, const QString &error) {
    const auto response = httpReply(output);
    const auto now = QDateTime::currentMSecsSinceEpoch();
    auto &primaryRetry = graphql ? m_graphqlRetryAt : m_restRetryAt;
    const auto reset = response.header("x-ratelimit-reset").toLongLong() * 1000;
    const bool primary = response.header("x-ratelimit-remaining") == "0" && reset > now;
    const bool rejected = response.status == 403 || response.status == 429;
    bool hasRetryAfter = false;
    const auto retryAfter = response.header("retry-after").toInt(&hasRetryAfter);
    const bool secondary = (rejected && hasRetryAfter) || (response.status == 429 && !primary) ||
                           error.contains("secondary rate limit", Qt::CaseInsensitive);
    if (primary)
        primaryRetry = qMax(primaryRetry, reset + 1000);
    else if (!secondary && error.contains("rate limit exceeded", Qt::CaseInsensitive))
        primaryRetry = qMax(primaryRetry, now + IdleInterval);
    if (secondary) {
        if (hasRetryAfter) {
            m_secondaryRetryAt = qMax(m_secondaryRetryAt, now + qint64(qMax(1, retryAfter)) * 1000);
        } else if (m_secondaryRetryAt <= now) {
            m_secondaryBackoff = now - m_secondaryRetryAt < IdleInterval
                                     ? std::clamp<qint64>(m_secondaryBackoff * 2, IdleInterval, 60 * IdleInterval)
                                     : IdleInterval;
            m_secondaryRetryAt = now + m_secondaryBackoff;
        }
    }
    if (primary || secondary || primaryRetry > now) {
        if (m_orgPublishTimer.isActive())
            m_orgPublishTimer.start(0);
        emit organizationProgressChanged();
    }
}

// A JSON request that sends along the ETag of the answer it got last time.
// "Not modified" comes back as the answer already here.
void Backend::api(const QString &path, Done done, const QString &projection, quint64 organizationPoll) {
    if (m_demo) {
        gh({"api", path}, done);
        return;
    }
    QStringList args{"api", "--include"};
    const auto cached = m_kept.value(path);
    if (!cached.etag.isEmpty())
        args << "-H" << "If-None-Match: " + QString::fromLatin1(cached.etag);
    if (!projection.isEmpty())
        args << "--jq" << projection;
    args << path;
    gh(args, [this, path, done, cached, scope = m_scope, organizationPoll]
             (bool ok, const QByteArray &out, const QString &err) {
        const auto response = httpReply(out);
        if (response.status == 304 && !cached.etag.isEmpty()) {
            done(true, cached.body, {});
            return;
        }
        if (!ok) {
            done(false, response.body, err);
            return;
        }
        const auto etag = response.header("etag");
        if (scope == m_scope && (!organizationPoll || organizationPoll == m_orgPollId) && !etag.isEmpty())
            m_kept[path] = {etag, response.body};
        done(true, response.body, {});
    }, organizationPoll);
}

// Which branch "main" is here, asked once per repository. Until the answer
// is in, and when it never comes, main or master is taken for it.
void Backend::fetchDefaultBranch() {
    if (m_defaultBranchOf == m_repo)
        return;
    m_defaultBranchOf = m_repo;
    const auto repo = m_repo;
    api(QString("repos/%1").arg(m_repo), [this, repo, scope = m_scope](bool ok, const QByteArray &out, const QString &) {
        m_kept.remove(QString("repos/%1").arg(repo));
        if (!ok || scope != m_scope)
            return;
        m_defaultBranch = QJsonDocument::fromJson(out).object().value("default_branch").toString();
        emit buildsChanged();
        rebuildJobs();
        fetchJobs();
    });
}

QString Backend::repositoryOf(const QVariantMap &value) const {
    const auto repository = value["repository"].toString();
    return repository.isEmpty() ? m_repo : repository;
}

QString Backend::baseBranch(const QVariantMap &build) const {
    if (!organization().isEmpty())
        return build["defaultBranch"].toString();
    if (!m_defaultBranch.isEmpty())
        return m_defaultBranch;
    for (const auto &name : {"main", "master"})
        for (const auto &build : m_allBuilds)
            if (build.toMap()["branch"] == name)
                return name;
    return build["branch"].toString();
}

void Backend::refresh() {
    if (!organization().isEmpty() && m_pullAuthor.isEmpty()) {
        if (!m_orgManualRemaining.isEmpty() || (m_fetchingRuns && m_orgPoll.force))
            return;
        if (m_fetchingRuns) {
            m_orgRefreshRequested = true;
            return;
        }
    }
    refreshRuns(true);
}

void Backend::refreshRuns(bool force) {
    if (m_fetchingRuns || m_repo.isEmpty() || m_offline)
        return;
    m_fetchingRuns = true;
    m_poll.stop();
    if (!organization().isEmpty()) {
        ++m_orgPollId;
        m_orgPublishTimer.stop();
        m_orgPoll = {};
        m_orgPoll.force = force;
        emit organizationProgressChanged();
        const auto now = QDateTime::currentMSecsSinceEpoch();
        if (!m_pullAuthor.isEmpty())
            fetchAuthoredRepositories();
        else if (force || now - m_orgCatalogCheckedAt >= OrganizationCatalogInterval)
            fetchOrganizationPage(1);
        else
            startOrganizationRuns(m_orgCatalog);
        return;
    }
    fetchDefaultBranch();
    const auto repo = m_repo;
    // A hundred runs is about thirty-five builds: the list, and the history
    // the forecast is made from, in one request.
    api(QString("repos/%1/actions/runs?per_page=100").arg(m_repo),
        [this, repo, scope = m_scope](bool ok, const QByteArray &out, const QString &err) {
            if (scope != m_scope)
                return;
            m_fetchingRuns = false;
            if (!ok) {
                setError(err.isEmpty() ? "Fetching runs from GitHub failed." : err);
                schedule();
                return;
            }
            QString parseError;
            const auto runs = GitHub::parseRuns(out, &parseError);
            if (!parseError.isEmpty()) {
                setError(parseError);
                schedule();
                return;
            }
            setError({});
            remember(repo);
            applyRuns(runs);
            fetchCommit();
            fetchJobs();
            schedule();
        });
}

void Backend::fetchOrganizationPage(int page) {
    const auto path = QString("orgs/%1/repos?type=all&sort=full_name&per_page=100&page=%2")
                          .arg(organization()).arg(page);
    api(path, [this, page, poll = m_orgPollId](bool ok, const QByteArray &out, const QString &err) {
        if (poll != m_orgPollId)
            return;
        QString error;
        const auto repositories = ok ? GitHub::parseRepositories(out, &error) : QVariantList{};
        if (!ok || !error.isEmpty()) {
            m_orgCatalogError = error.isEmpty() ? (err.isEmpty() ? "Fetching organization repositories failed." : err)
                                               : error;
            if (m_orgCatalogCheckedAt == 0) {
                failOrganizationPoll(m_orgCatalogError);
                return;
            }
            m_orgCatalogCheckedAt = QDateTime::currentMSecsSinceEpoch();
            startOrganizationRuns(m_orgCatalog);
            return;
        }
        for (const auto &value : repositories) {
            const auto repo = value.toMap();
            if (!repo["archived"].toBool() && !repo["disabled"].toBool() && !repo["name"].toString().isEmpty())
                m_orgPoll.repositories << repo;
        }
        if (repositories.size() == 100) {
            fetchOrganizationPage(page + 1);
            return;
        }
        m_orgCatalog = std::move(m_orgPoll.repositories);
        m_orgCatalogCheckedAt = QDateTime::currentMSecsSinceEpoch();
        if (!m_orgCatalogFirstCheckedAt)
            m_orgCatalogFirstCheckedAt = m_orgCatalogCheckedAt;
        m_orgCatalogError.clear();
        m_orgPoll.catalogLoaded = true;
        startOrganizationRuns(m_orgCatalog);
    }, "[.[] | {full_name, archived, disabled, default_branch, pushed_at}]", m_orgPollId);
}

void Backend::fetchAuthoredRepositories() {
    const QString query = R"(
        query($search: String!) {
          search(query: $search, type: ISSUE, first: 100) {
            nodes {
              ... on PullRequest {
                number
                author { login }
                headRefOid
                headRefName
                headRepository { databaseId }
                repository {
                  nameWithOwner
                  isArchived
                  isDisabled
                  defaultBranchRef { name }
                }
              }
            }
          }
        }
    )";
    const auto author = m_pullAuthor;
    const auto searchAuthor = author.endsWith("[bot]", Qt::CaseInsensitive)
                                  ? "app/" + author.chopped(5) : author;
    const auto search = QString("org:%1 is:pr author:%2 archived:false sort:updated-desc")
                            .arg(organization(), searchAuthor);
    gh({"api", "graphql", "--include", "-f", "query=" + query, "-f", "search=" + search},
       [this, author, poll = m_orgPollId](bool ok, const QByteArray &out, const QString &err) {
           if (poll != m_orgPollId)
               return;
           QString error;
           const auto repositories = ok ? GitHub::parseAuthoredRepositories(httpReply(out).body, author, &error)
                                        : QVariantList{};
           if (!ok || !error.isEmpty()) {
               failOrganizationPoll(error.isEmpty() ? (err.isEmpty() ? "Looking up pull requests failed." : err)
                                                     : error);
               return;
           }
           m_orgPoll.catalogLoaded = true;
           startOrganizationRuns(repositories);
       }, m_orgPollId);
}

Backend::RepositoryRuns &Backend::organizationRuns() {
    return m_pullAuthor.isEmpty() ? m_orgRuns : m_orgAuthorRuns;
}

QHash<QString, QString> &Backend::organizationErrors() {
    return m_pullAuthor.isEmpty() ? m_orgErrors : m_orgAuthorErrors;
}

qint64 Backend::organizationRepositoryDue(const QVariantMap &repository, qint64 now) const {
    const auto name = repository["name"].toString();
    const auto activity = m_orgActivity.value(name);
    const auto pushedAt = GitHub::ms(repository["pushedAt"].toString());
    if (!activity.checkedAt)
        return m_orgCatalogFirstCheckedAt ? m_orgCatalogFirstCheckedAt : now;
    const auto lastActivity = qMax(pushedAt, activity.lastRun);
    const int interval = activity.failed ? IdleInterval
                       : activity.active ? OrganizationActiveInterval
                       : now - lastActivity <= RecentActivity ? IdleInterval
                       : OrganizationQuietInterval;
    const auto next = activity.checkedAt + interval;
    const auto due = pushedAt > activity.checkedAt ? qMin(pushedAt, next) : next;
    return m_orgManualRemaining.contains(name) ? qMin(due, m_orgManualStartedAt) : due;
}

void Backend::startOrganizationRuns(const QVariantList &repositories) {
    QSet<QString> current;
    for (const auto &value : repositories)
        current.insert(value.toMap()["name"].toString());
    const auto outsideScope = [&current](const auto &entry) { return !current.contains(entry.key()); };
    m_orgPoll.changed = organizationRuns().removeIf(outsideScope) > 0;
    organizationErrors().removeIf(outsideScope);
    m_orgPoll.emptyScope = repositories.isEmpty();
    m_orgPoll.repositories = repositories;
    if (m_orgPoll.changed)
        m_orgPublishTimer.start(OrganizationPublishInterval);
    if (m_pullAuthor.isEmpty()) {
        m_orgActivity.removeIf([&current](const auto &entry) { return !current.contains(entry.key()); });
        const auto now = QDateTime::currentMSecsSinceEpoch();
        if (m_orgPoll.force) {
            m_orgManualRemaining = current;
            m_orgManualTotal = current.size();
            m_orgManualStartedAt = now;
        } else {
            m_orgManualRemaining.intersect(current);
        }
        auto &due = m_orgPoll.repositories;
        due.removeIf([this, now](const auto &value) { return organizationRepositoryDue(value.toMap(), now) > now; });
        std::stable_sort(due.begin(), due.end(), [this, now](const auto &first, const auto &second) {
            const auto firstRepo = first.toMap(), secondRepo = second.toMap();
            const auto firstDue = organizationRepositoryDue(firstRepo, now);
            const auto secondDue = organizationRepositoryDue(secondRepo, now);
            if (firstDue != secondDue)
                return firstDue < secondDue;
            return firstRepo["pushedAt"].toString() > secondRepo["pushedAt"].toString();
        });
        const auto firstIdle = std::stable_partition(due.begin(), due.end(), [this](const auto &value) {
            return m_orgActivity.value(value.toMap()["name"].toString()).active;
        });
        if (due.size() > OrganizationBatchSize) {
            // Leave room for both active rechecks and initial/quiet checks.
            const auto active = firstIdle - due.begin();
            const auto idle = due.size() - active;
            const auto activeSlots = std::min<qsizetype>(active, std::max<qsizetype>(
                OrganizationBatchSize / 2, OrganizationBatchSize - idle));
            due = due.first(activeSlots) + due.mid(active, OrganizationBatchSize - activeSlots);
        }
    }
    emit organizationProgressChanged();
    fetchOrganizationRuns();
}

void Backend::failOrganizationPoll(const QString &error) {
    m_fetchingRuns = false;
    setError(error);
    emit organizationProgressChanged();
    schedule();
}

void Backend::fetchOrganizationRuns() {
    while (m_orgPoll.pending < OrganizationConcurrency && m_orgPoll.next < m_orgPoll.repositories.size()) {
        const auto repository = m_orgPoll.repositories.at(m_orgPoll.next++).toMap();
        ++m_orgPoll.pending;
        fetchOrganizationRepository(repository);
    }
    if (m_orgPoll.pending > 0)
        return;

    m_fetchingRuns = false;
    m_orgPublishTimer.stop();
    QStringList errors;
    if (m_pullAuthor.isEmpty() && !m_orgCatalogError.isEmpty())
        errors << m_orgCatalogError;
    const auto &repositoryErrors = organizationErrors();
    if (!repositoryErrors.isEmpty())
        errors << QString("GitHub data could not be refreshed for %1 repositories. %2 · %3")
                      .arg(repositoryErrors.size()).arg(repositoryErrors.constBegin().key(), repositoryErrors.constBegin().value());
    setError(errors.join('\n'));
    if (m_orgPoll.receivedRuns > 0 || (m_orgPoll.catalogLoaded && m_orgPoll.emptyScope)) {
        markUpdated();
        remember(m_repo);
    }
    flushOrganizationChanges();
    if (std::exchange(m_orgPoll.checkedSelection, false))
        fetchJobs();
    emit organizationProgressChanged();
    schedule();
}

void Backend::flushOrganizationChanges() {
    if (!std::exchange(m_orgPoll.changed, false))
        return;
    const auto previousKey = m_key;
    publishOrganizationRuns();
    if (std::exchange(m_orgPoll.checkedSelection, false) || m_key != previousKey)
        fetchJobs();
}

void Backend::publishOrganizationRuns() {
    QVariantList runs;
    for (const auto &repositoryRuns : std::as_const(organizationRuns()))
        runs += repositoryRuns;
    std::sort(runs.begin(), runs.end(), [](const QVariant &a, const QVariant &b) {
        const auto first = a.toMap(), second = b.toMap();
        const auto firstStart = first["startedAt"].toString(), secondStart = second["startedAt"].toString();
        return firstStart == secondStart ? first["id"].toLongLong() > second["id"].toLongLong()
                                        : firstStart > secondStart;
    });
    applySnapshot(runs);
}

void Backend::fetchOrganizationRepository(const QVariantMap &repository) {
    const auto repo = repository["name"].toString();
    const int limit = m_pullAuthor.isEmpty() ? OrganizationRunsPerRepo : 100;
    const auto path = QString("repos/%1/actions/runs?per_page=%2").arg(repo).arg(limit);
    api(path, [this, repo, repository, poll = m_orgPollId]
              (bool ok, const QByteArray &out, const QString &err) {
        if (poll != m_orgPollId)
            return;
        QString error;
        auto runs = ok ? GitHub::parseRuns(out, &error) : QVariantList{};
        if (!ok || !error.isEmpty()) {
            finishOrganizationRepository(repo, {}, error.isEmpty() ? err : error, false);
            return;
        }
        for (auto &value : runs) {
            auto run = value.toMap();
            run["repository"] = repo;
            run["defaultBranch"] = repository["defaultBranch"];
            value = run;
        }
        if (!m_pullAuthor.isEmpty()) {
            const auto unfiltered = runs.mid(0, OrganizationRunsPerRepo);
            // Both views use the same underlying run data. Keep the cached
            // organization view current when a filtered refresh sees a finish.
            m_orgRuns.insert(repo, unfiltered);
            recordOrganizationActivity(repo, unfiltered, true);
            m_orgErrors.remove(repo);
            m_orgUnfilteredUpdated = QDateTime::currentDateTime();
            GitHub::attachPullAuthors(runs, repository["pullRequests"].toList());
        }
        finishOrganizationRepository(repo, runs, {}, true);
    }, "{workflow_runs: [.workflow_runs[] | {id, name, display_title, head_branch, event, "
       "actor: {login: .actor.login}, head_repository: {id: .head_repository.id}, repository: {id: .repository.id}, "
       "pull_requests: [.pull_requests[]? | {number, base: {ref: .base.ref, repo: {id: .base.repo.id}}, "
       "head: {sha: .head.sha, ref: .head.ref, repo: {id: .head.repo.id}}}], "
       "run_number, run_attempt, head_sha, html_url, status, conclusion, run_started_at, created_at, updated_at}]}",
       m_orgPollId);
}

void Backend::recordOrganizationActivity(const QString &repository, const QVariantList &runs, bool receivedRuns) {
    auto &activity = m_orgActivity[repository];
    activity.checkedAt = QDateTime::currentMSecsSinceEpoch();
    activity.failed = !receivedRuns;
    if (!receivedRuns)
        return;
    activity.active = false;
    activity.lastRun = 0;
    for (const auto &value : runs) {
        const auto run = value.toMap();
        activity.active |= GitHub::isActive(run["state"].toString());
        activity.lastRun = qMax(activity.lastRun, GitHub::ms(run["updatedAt"].toString()));
    }
}

void Backend::finishOrganizationRepository(const QString &repository, const QVariantList &runs,
                                           const QString &error, bool receivedRuns) {
    if (m_pullAuthor.isEmpty()) {
        m_orgManualRemaining.remove(repository);
        recordOrganizationActivity(repository, runs, receivedRuns);
    }
    if (receivedRuns) {
        auto &snapshot = organizationRuns()[repository];
        if (snapshot != runs) {
            snapshot = runs;
            m_orgPoll.changed = true;
        }
        m_orgPoll.checkedSelection |= selectedBuild()["repository"] == repository;
        organizationErrors().remove(repository);
        ++m_orgPoll.receivedRuns;
    } else {
        organizationErrors()[repository] = error.isEmpty() ? "Fetching workflow runs failed." : error;
    }
    --m_orgPoll.pending;
    const auto checked = m_orgPoll.next - m_orgPoll.pending;
    if (checked < m_orgPoll.repositories.size()) {
        const bool firstMatch = m_builds.isEmpty() && std::any_of(runs.cbegin(), runs.cend(), [this](const auto &value) {
            const auto run = value.toMap();
            return matchesPullAuthor(run) && (!m_runningOnly || GitHub::isActive(run["state"].toString()));
        });
        if (m_orgPoll.changed) {
            if (firstMatch) {
                m_orgPublishTimer.stop();
                markUpdated();
                flushOrganizationChanges();
            } else if (!m_orgPublishTimer.isActive()) {
                m_orgPublishTimer.start(OrganizationPublishInterval);
            }
        }
        emit organizationProgressChanged();
    }
    fetchOrganizationRuns();
}

void Backend::applyRuns(const QVariantList &runs) {
    markUpdated();
    applySnapshot(runs);
}

void Backend::markUpdated() {
    m_lastUpdated = QDateTime::currentDateTime();
    if (!organization().isEmpty() && m_pullAuthor.isEmpty())
        m_orgUnfilteredUpdated = m_lastUpdated;
    emit lastUpdatedChanged();
}

void Backend::applySnapshot(const QVariantList &runs) {
    const auto before = m_allBuilds;
    m_listRuns = runs;
    rebuild();
    if (!before.isEmpty())
        for (const auto &build : GitHub::finishedBuilds(before, m_allBuilds))
            if (matchesPullAuthor(build.toMap()))
                emit buildFinished(build.toMap());
}

// Builds from every run the app knows: the latest ones, and the runs of a
// commit it was asked for that is older than those.
void Backend::rebuild() {
    QVariantList runs = m_listRuns;
    QSet<qlonglong> ids;
    for (const auto &run : runs)
        ids << run.toMap()["id"].toLongLong();
    for (const auto &run : m_extraRuns)
        if (!ids.contains(run.toMap()["id"].toLongLong()))
            runs << run;

    const auto history = GitHub::builds(runs);
    m_buildsByRepository.clear();
    for (const auto &value : history)
        m_buildsByRepository[value.toMap()["repository"].toString()] << value;

    // Only runs attributed to the requested author contribute to the build's
    // state and jobs. Other runs on that commit can be independent workflows.
    if (m_pullAuthor.isEmpty()) {
        m_allBuilds = history;
    } else {
        runs.removeIf([this](const QVariant &run) { return !matchesPullAuthor(run.toMap()); });
        m_allBuilds = GitHub::builds(runs);
    }
    // A running build's row says how far along it is, which only needs how
    // long builds like it usually take.
    for (auto &value : m_allBuilds) {
        auto build = value.toMap();
        if (!GitHub::isActive(build["state"].toString()))
            continue;
        const auto usual = Estimate::spread(Estimate::likeBuilds(comparisonBuilds(build), build));
        build["usual"] = usual.known() ? usual.typical : 0.0;
        value = build;
    }
    filterBuilds();
}

void Backend::filterBuilds() {
    m_builds.clear();
    // Narrowed to main, the list is main's builds: a scheduled job that
    // happens to run on main is not one of them.
    const bool mainOnly = this->mainOnly();
    for (const auto &value : m_allBuilds) {
        const auto build = value.toMap();
        if (!matchesPullAuthor(build))
            continue;
        if (!m_showSkipped && build["state"] == "skipped")
            continue;
        if (!m_branch.isEmpty() && build["branch"] != m_branch)
            continue;
        if (mainOnly && (!build["isBuild"].toBool() ||
                         (!organization().isEmpty() && build["branch"] != build["defaultBranch"])))
            continue;
        // The build you are watching stays when it finishes, so it doesn't
        // vanish from under you at the moment it has news.
        if (m_runningOnly && !GitHub::isActive(build["state"].toString()) && build["key"] != m_key)
            continue;
        m_builds << build;
    }

    QString key = m_key;
    // Handed one commit: its build, as soon as it is here.
    if (!m_wantCommit.isEmpty()) {
        for (const auto &build : m_builds) {
            if (build.toMap()["fullSha"].toString().startsWith(m_wantCommit)) {
                key = build.toMap()["key"].toString();
                m_wantCommit.clear();
                break;
            }
        }
    }
    if (!hasVisibleBuild(key))
        key = m_builds.isEmpty() ? QString() : m_builds.first().toMap()["key"].toString();

    emit buildsChanged();
    if (key != m_key) {
        m_key = key;
        clearJob();
    }
    rebuildJobs();
}

void Backend::clearJob() {
    m_jobId = 0;
    m_jobPinned = false;
    m_step = -1;
    m_logJobId = 0;
    m_log.clear();
    m_found.clear();
    m_failures.clear();
    setLogStatus({});
    emit failuresChanged();
    emit logReset();
}

void Backend::schedule() {
    const auto interval = activeCount() > 0 ? ActiveInterval : IdleInterval;
    if (m_orgRefreshRequested) {
        m_poll.start(1000);
        return;
    }
    if (organization().isEmpty() || !m_pullAuthor.isEmpty() || !m_orgCatalogCheckedAt) {
        m_poll.start(organization().isEmpty() ? interval : IdleInterval);
        return;
    }
    const auto now = QDateTime::currentMSecsSinceEpoch();
    auto next = m_orgCatalogCheckedAt + OrganizationCatalogInterval;
    for (const auto &repository : std::as_const(m_orgCatalog))
        next = qMin(next, organizationRepositoryDue(repository.toMap(), now));
    m_poll.start(qMax<qint64>(1000, next - now));
}

// A commit older than the latest hundred runs is asked for by its sha.
void Backend::fetchCommit() {
    if (!m_wantCommit.isEmpty() && m_wantCommit.size() != 40) {
        // The API only finds a commit's runs by its whole sha.
        setError(QString("Commit %1 isn't among the latest builds. Pass the whole sha to find an older one.")
                     .arg(m_wantCommit));
        m_wantCommit.clear();
        return;
    }
    if (m_wantCommit.isEmpty() || m_commitFetched == m_wantCommit)
        return;
    m_commitFetched = m_wantCommit;
    const auto repo = m_repo;
    const auto commit = m_wantCommit;
    api(QString("repos/%1/actions/runs?per_page=100&head_sha=%2").arg(repo, commit),
        [this, scope = m_scope, commit](bool ok, const QByteArray &out, const QString &err) {
            if (scope != m_scope || commit != m_wantCommit)
                return;
            if (!ok) {
                setError(err.isEmpty() ? "Fetching the runs of that commit failed." : err);
                return;
            }
            m_extraRuns = GitHub::parseRuns(out);
            if (m_extraRuns.isEmpty())
                setError(QString("GitHub has no runs for commit %1.").arg(commit.left(7)));
            rebuild();
            fetchJobs();
        });
}

void Backend::fetchRunJobs(const QVariantMap &run, bool final) {
    const auto runId = run["id"].toLongLong();
    if (m_jobsInFlight.contains(runId))
        return;
    m_jobsInFlight << runId;
    const auto path = QString("repos/%1/actions/runs/%2/jobs?per_page=100").arg(repositoryOf(run)).arg(runId);
    api(path, [this, scope = m_scope, path, runId, final, updatedAt = run["updatedAt"].toString()]
              (bool ok, const QByteArray &out, const QString &err) {
        if (scope != m_scope)
            return;
        m_jobsInFlight.remove(runId);
        if (!ok) {
            setError(err.isEmpty() ? "Fetching jobs from GitHub failed." : err);
            return;
        }
        // A finished run is never asked for again, so its answer needn't be kept.
        if (final)
            m_kept.remove(path);
        m_jobsOfRun[runId].updatedAt = updatedAt;
        applyJobs(runId, GitHub::parseJobs(out), final);
        fetchLog();
    });
}

void Backend::fetchJobs() {
    const auto build = selectedBuild();
    if (build.isEmpty())
        return;
    for (const auto &value : build["runs"].toList()) {
        const auto run = value.toMap();
        const auto id = run["id"].toLongLong();
        const auto state = run["state"].toString();
        if (state == "skipped" && build["state"] != "skipped")
            continue;
        // A finished run's jobs don't change, so only ask again when the run has.
        const auto kept = m_jobsOfRun.value(id);
        if (kept.final && kept.updatedAt == run["updatedAt"].toString())
            continue;
        fetchRunJobs(run, !GitHub::isActive(state));
    }
    // The jobs of the build main had before this one, to compare with. It
    // is finished, so this is asked once.
    for (const auto &value : Compare::previous(comparisonBuilds(build), build, baseBranch(build))["runs"].toList()) {
        const auto run = value.toMap();
        if (run["state"] != "skipped" && !m_jobsOfRun.value(run["id"].toLongLong()).final)
            fetchRunJobs(run, true);
    }
    // The jobs of earlier builds, for the forecast. Only a build that is
    // still going needs one.
    if (GitHub::isActive(build["state"].toString())) {
        int taken = 0;
        for (const auto &value : m_like) {
            const auto earlier = value.toMap();
            if (earlier["state"] != "success")
                continue;
            if (++taken > EarlierBuilds)
                break;
            for (const auto &runValue : earlier["runs"].toList()) {
                const auto run = runValue.toMap();
                if (run["state"] != "skipped" && !m_jobsOfRun.value(run["id"].toLongLong()).final)
                    fetchRunJobs(run, true);
            }
        }
    }
    fetchLog();
}

void Backend::applyJobs(qlonglong runId, const QVariantList &jobs, bool final) {
    auto &kept = m_jobsOfRun[runId];
    kept.jobs = jobs;
    kept.final = final;
    rebuildJobs();
}

QList<Estimate::Earlier> Backend::earlierBuilds(const QVariantList &like) const {
    QList<Estimate::Earlier> out;
    int taken = 0;
    for (const auto &value : like) {
        const auto build = value.toMap();
        if (build["state"] != "success")
            continue;
        if (++taken > EarlierBuilds)
            break;
        const auto origin = GitHub::ms(build["startedAt"].toString());
        Estimate::Earlier earlier;
        bool whole = true;
        for (const auto &runValue : build["runs"].toList()) {
            const auto run = runValue.toMap();
            if (run["state"] == "skipped")
                continue;
            const auto kept = m_jobsOfRun.value(run["id"].toLongLong());
            if (!kept.final) {
                whole = false;
                break;
            }
            for (const auto &jobValue : kept.jobs) {
                const auto job = jobValue.toMap();
                Estimate::Job sample;
                sample.key = jobKey(job, run);
                sample.state = job["state"].toString();
                sample.start = offset(job["startedAt"].toString(), origin);
                sample.end = offset(job["completedAt"].toString(), origin);
                earlier.insert(sample.key, sample);
            }
        }
        if (whole && !earlier.isEmpty())
            out << earlier;
    }
    return out;
}

// How long each step of a job usually takes, by name.
QHash<QString, double> Backend::usualSteps(const QString &key) const {
    QHash<QString, QList<double>> seconds;
    int taken = 0;
    for (const auto &value : m_like) {
        const auto build = value.toMap();
        if (build["state"] != "success")
            continue;
        if (++taken > EarlierBuilds)
            break;
        for (const auto &runValue : build["runs"].toList()) {
            const auto run = runValue.toMap();
            for (const auto &jobValue : m_jobsOfRun.value(run["id"].toLongLong()).jobs) {
                const auto job = jobValue.toMap();
                if (jobKey(job, run) != key)
                    continue;
                for (const auto &stepValue : job["steps"].toList()) {
                    const auto step = stepValue.toMap();
                    if (step["state"] == "success")
                        seconds[step["name"].toString()]
                            << GitHub::secondsBetween(step["startedAt"].toString(), step["completedAt"].toString());
                }
            }
        }
    }
    QHash<QString, double> usual;
    for (auto it = seconds.cbegin(); it != seconds.cend(); ++it)
        usual.insert(it.key(), Estimate::quantile(it.value(), .5));
    return usual;
}

// The jobs of the picked build: those of all its runs, in the order the runs
// started, and after them the ones earlier builds say are still to come.
void Backend::rebuildJobs() {
    const auto build = selectedBuild();
    const auto origin = GitHub::ms(build["startedAt"].toString());
    const bool several = build["workflows"].toStringList().size() > 1;

    m_like = build.isEmpty() ? QVariantList{} : Estimate::likeBuilds(comparisonBuilds(build), build);
    QVariantList history;
    for (auto it = m_like.crbegin(); it != m_like.crend(); ++it) {
        const auto earlier = it->toMap();
        history << QVariantMap{{"key", earlier["key"]},     {"seconds", earlier["seconds"]},
                               {"state", earlier["state"]}, {"title", earlier["title"]},
                               {"sha", earlier["sha"]},     {"current", false},
                               {"selectable", hasVisibleBuild(earlier["key"].toString())}};
    }
    if (!build.isEmpty())
        history << QVariantMap{{"key", build["key"]},     {"seconds", build["seconds"]},
                               {"state", build["state"]}, {"title", build["title"]},
                               {"sha", build["sha"]},     {"current", true}};
    if (history != m_history) {
        m_history = history;
        emit historyChanged();
    }

    m_realJobs.clear();
    for (const auto &runValue : build["runs"].toList()) {
        const auto run = runValue.toMap();
        for (const auto &jobValue : m_jobsOfRun.value(run["id"].toLongLong()).jobs) {
            auto job = jobValue.toMap();
            const auto state = job["state"].toString();
            const bool started = state != "queued" && state != "skipped";
            job["key"] = jobKey(job, run);
            job["workflow"] = several ? run["workflow"] : QVariant(QString());
            job["runId"] = run["id"];
            job["start"] = started ? offset(job["startedAt"].toString(), origin) : -1.0;
            job["end"] = started ? offset(job["completedAt"].toString(), origin) : -1.0;
            job["expected"] = false;
            m_realJobs << job;
        }
    }

    // In the order they started, which is the order a timeline reads in;
    // the ones still waiting come last.
    std::stable_sort(m_realJobs.begin(), m_realJobs.end(), [](const QVariant &a, const QVariant &b) {
        const double x = a.toMap()["start"].toDouble(), y = b.toMap()["start"].toDouble();
        return (x < 0 ? 1e12 : x) < (y < 0 ? 1e12 : y);
    });

    computePrevious();
    computeEstimate();

    auto jobs = m_realJobs;
    int phantom = 0;
    for (const auto &value : m_estimate["expectedJobs"].toList()) {
        const auto key = value.toString();
        jobs << QVariantMap{{"id", qlonglong(--phantom)},
                            {"key", key},
                            {"name", key.section('/', 1)},
                            {"workflow", several ? key.section('/', 0, 0) : QString()},
                            {"state", "pending"},
                            {"expected", true},
                            {"start", -1.0},
                            {"end", -1.0},
                            {"steps", QVariantList{}}};
    }
    if (jobs != m_jobs) {
        m_jobs = jobs;
        emit jobsChanged();
    }
    pickJob();
    annotateLog();
    emit selectionChanged();

    const bool active = GitHub::isActive(build["state"].toString());
    if (active != m_tick.isActive())
        active ? m_tick.start() : m_tick.stop();
}

// The picked build next to the one main had before it.
void Backend::computePrevious() {
    const auto build = selectedBuild();
    const auto before = build.isEmpty() ? QVariantMap{}
                                        : Compare::previous(comparisonBuilds(build), build, baseBranch(build));
    QVariantMap map;
    if (!before.isEmpty()) {
        const auto origin = GitHub::ms(build["startedAt"].toString());
        const auto thenOrigin = GitHub::ms(before["startedAt"].toString());
        const bool active = GitHub::isActive(build["state"].toString());
        const double elapsed = qMax(0.0, (QDateTime::currentMSecsSinceEpoch() - origin) / 1000.0);

        QList<Estimate::Job> now, then;
        for (const auto &value : m_realJobs) {
            const auto job = value.toMap();
            Estimate::Job entry;
            entry.key = job["key"].toString();
            entry.state = job["state"].toString();
            entry.start = job["start"].toDouble();
            entry.end = entry.state == "running" ? elapsed : job["end"].toDouble();
            now << entry;
        }
        bool ready = true;
        for (const auto &runValue : before["runs"].toList()) {
            const auto run = runValue.toMap();
            if (run["state"] == "skipped")
                continue;
            const auto kept = m_jobsOfRun.value(run["id"].toLongLong());
            ready = ready && kept.final;
            for (const auto &jobValue : kept.jobs) {
                const auto job = jobValue.toMap();
                Estimate::Job entry;
                entry.key = jobKey(job, run);
                entry.state = job["state"].toString();
                entry.start = offset(job["startedAt"].toString(), thenOrigin);
                entry.end = offset(job["completedAt"].toString(), thenOrigin);
                then << entry;
            }
        }
        const auto rows = ready ? Compare::jobs(now, then, !active) : QList<Compare::Row>{};
        QVariantMap perJob;
        double reach = 0;
        for (const auto &row : rows) {
            perJob[row.key] = QVariantMap{{"then", row.then},           {"thenState", row.thenState},
                                          {"thenStart", row.thenStart}, {"thenEnd", row.thenEnd},
                                          {"delta", row.delta},         {"verdict", row.verdict}};
            reach = qMax(reach, row.thenEnd);
        }
        map = QVariantMap{{"key", before["key"]},
                          {"selectable", matchesPullAuthor(before) && !m_runningOnly},
                          {"title", before["title"]},
                          {"sha", before["sha"]},
                          {"branch", before["branch"]},
                          {"state", before["state"]},
                          {"seconds", before["seconds"]},
                          {"startedAt", before["startedAt"]},
                          {"ready", ready},
                          {"jobs", perJob},
                          {"reach", reach},
                          {"notes", Compare::notes(rows)},
                          // How the whole build compares, once it is done.
                          {"delta", active ? 0.0 : build["seconds"].toDouble() - before["seconds"].toDouble()},
                          {"notable", !active && Compare::notable(build["seconds"].toDouble(), before["seconds"].toDouble())}};
    }
    if (map != m_previous) {
        m_previous = map;
        emit previousChanged();
    }
}

void Backend::selectPrevious() {
    const auto key = m_previous.value("key").toString();
    if (key.isEmpty() || !m_previous.value("selectable").toBool())
        return;
    // It has to be in the list to be picked.
    if (!m_branch.isEmpty() && m_previous["branch"] != m_branch) {
        m_branch.clear();
        filterBuilds();
    }
    const auto from = m_cameFrom.isEmpty() ? m_key : m_cameFrom;
    selectBuild(key);
    // Walking further back keeps the way home to where the walk began.
    m_cameFrom = from;
    emit selectionChanged();
}

void Backend::selectCameFrom() {
    const auto key = m_cameFrom;
    const auto build = buildByKey(key);
    if (build.isEmpty() || !matchesPullAuthor(build))
        return;
    if (!m_branch.isEmpty() && buildByKey(key)["branch"] != m_branch) {
        m_branch.clear();
        filterBuilds();
    }
    selectBuild(key);
}

void Backend::computeEstimate() {
    const auto build = selectedBuild();
    QVariantMap map;
    if (build.isEmpty()) {
        if (!m_estimate.isEmpty()) {
            m_estimate.clear();
            emit estimateChanged();
        }
        return;
    }
    const auto usual = Estimate::spread(m_like);
    const bool active = GitHub::isActive(build["state"].toString());
    const auto origin = GitHub::ms(build["startedAt"].toString());
    const double elapsed = active ? qMax(0.0, (QDateTime::currentMSecsSinceEpoch() - origin) / 1000.0)
                                  : build["seconds"].toDouble();
    map["active"] = active;
    map["elapsed"] = elapsed;
    map["usualKnown"] = usual.known();
    map["usualSamples"] = usual.samples;
    map["usualLow"] = usual.low;
    map["usual"] = usual.typical;
    map["usualHigh"] = usual.high;
    map["known"] = false;

    double axis = elapsed;
    for (const auto &job : m_realJobs)
        axis = qMax(axis, job.toMap()["end"].toDouble());
    // Room for where the same jobs ran in the build before, drawn under them.
    axis = qMax(axis, qMin(m_previous.value("reach").toDouble(), axis * 2.5));

    QVariantMap perJob;
    QVariantList expected;
    if (active) {
        QList<Estimate::Job> now;
        for (const auto &value : m_realJobs) {
            const auto job = value.toMap();
            Estimate::Job entry;
            entry.key = job["key"].toString();
            entry.state = job["state"].toString();
            entry.start = job["start"].toDouble();
            entry.end = job["end"].toDouble();
            now << entry;
        }
        const auto forecast = Estimate::forecast(now, elapsed, usual, earlierBuilds(m_like));
        map["known"] = forecast.known;
        map["basis"] = forecast.basis;
        map["samples"] = forecast.samples;
        map["total"] = forecast.total;
        map["low"] = forecast.low;
        map["high"] = forecast.high;
        map["late"] = forecast.late;
        map["over"] = forecast.over;
        map["remaining"] = qMax(0.0, forecast.total - elapsed);
        map["remainingRounded"] = Estimate::rounded(qMax(0.0, forecast.total - elapsed));
        if (forecast.known)
            axis = qMax(axis, forecast.high);
        for (const auto &job : forecast.jobs) {
            perJob[job.key] = QVariantMap{{"start", job.start}, {"end", job.end},         {"low", job.low},
                                          {"high", job.high},   {"typical", job.typical}, {"late", job.late}};
            if (job.expected)
                expected << job.key;
            if (forecast.basis == "jobs")
                axis = qMax(axis, job.high);
        }
    } else {
        map["total"] = elapsed;
    }
    map["jobs"] = perJob;
    map["expectedJobs"] = expected;
    map["axis"] = qMax(axis, 1.0);
    m_estimate = map;
    emit estimateChanged();
}

// Shows the job worth looking at: the one that failed, else the one running,
// else the last one that did any work. A job the user picked stays picked.
void Backend::pickJob() {
    if (m_jobPinned && !selectedJob().isEmpty())
        return;
    auto find = [this](auto predicate) -> qlonglong {
        for (const auto &job : m_realJobs)
            if (predicate(job.toMap()))
                return job.toMap()["id"].toLongLong();
        return 0;
    };
    qlonglong id = find([](const QVariantMap &job) { return job["state"] == "failure"; });
    if (!id)
        id = find([](const QVariantMap &job) { return job["state"] == "running"; });
    if (!id)
        for (const auto &job : m_realJobs)
            if (!job.toMap()["steps"].toList().isEmpty() && job.toMap()["state"] != "skipped")
                id = job.toMap()["id"].toLongLong();
    if (!id && !m_realJobs.isEmpty())
        id = m_realJobs.first().toMap()["id"].toLongLong();
    if (id != m_jobId) {
        m_jobId = id;
        m_step = -1;
        m_log.clear();
        m_logJobId = 0;
        emit logReset();
    }
}

// Ties the log to the steps: where each one starts, how long it took and
// usually takes, and where in the failed one the failure is.
void Backend::annotateLog() {
    const auto job = selectedJob();
    const auto steps = job["steps"].toList();
    const bool mine = m_logJobId == m_jobId && m_jobId != 0;

    QStringList names;
    for (const auto &step : steps)
        names << step.toMap()["name"].toString();
    const auto starts = mine ? GitHub::mapSteps(steps, m_log.lines()) : QList<int>(steps.size(), -1);
    m_log.setSteps(starts, names);

    const auto usual = usualSteps(job["key"].toString());
    QVariantList out;
    int failedStep = -1;
    for (int i = 0; i < steps.size(); ++i) {
        auto step = steps[i].toMap();
        const auto state = step["state"].toString();
        step["line"] = starts.value(i, -1);
        step["seconds"] = GitHub::isActive(state) || state == "skipped"
                              ? 0
                              : GitHub::secondsBetween(step["startedAt"].toString(), step["completedAt"].toString());
        step["usual"] = usual.value(step["name"].toString(), -1);
        if (state == "failure" && failedStep < 0)
            failedStep = i;
        out << step;
    }
    if (out != m_steps) {
        m_steps = out;
        emit stepsChanged();
    }

    QList<Failures::Failure> found;
    const auto state = job["state"].toString();
    if (mine && state != "success" && state != "skipped") {
        // Inside the failed step when the log says where it is, so the noise
        // of a clean-up step (a database's own error log, say) stays out.
        if (failedStep >= 0 && starts.value(failedStep, -1) >= 0)
            found = Failures::find(m_log.lines(), starts[failedStep], m_log.stepEnd(failedStep));
        else
            found = Failures::find(m_log.lines());
    }
    m_log.setFailures(found);
    m_found = found;
    QVariantList failures;
    for (const auto &failure : found)
        failures << QVariantMap{{"line", failure.line},   {"first", failure.first},
                                {"last", failure.last},   {"title", failure.title},
                                {"location", failure.location}, {"detail", failure.detail}};
    if (failures != m_failures) {
        m_failures = failures;
        emit failuresChanged();
    }
}

void Backend::fetchLog() {
    const auto job = selectedJob();
    if (job.isEmpty() || m_fetchingLog) {
        if (job.isEmpty())
            setLogStatus(m_jobs.isEmpty() ? "" : "Pick a job to see its log.");
        return;
    }
    const auto state = job["state"].toString();
    if (state == "skipped" || (state == "queued" && job["steps"].toList().isEmpty())) {
        m_log.clear();
        setLogStatus(state == "skipped" ? "This job was skipped, so it has no log."
                                        : "This job is waiting for a runner.");
        return;
    }
    if (m_logJobId == m_jobId && m_logFinal)
        return;

    const auto jobId = m_jobId;
    const bool final = !GitHub::isActive(state);
    if (m_logJobId != jobId)
        setLogStatus("Loading log…");
    m_fetchingLog = true;
    gh({"api", "--allow-escape-sequences",
        QString("repos/%1/actions/jobs/%2/logs").arg(repositoryOf(selectedBuild())).arg(jobId)},
       [this, jobId, final, scope = m_scope](bool ok, const QByteArray &out, const QString &err) {
           m_fetchingLog = false;
           if (jobId != m_jobId || scope != m_scope) {
               fetchLog();
               return;
           }
           if (!ok) {
               // GitHub only serves a job's log once the runner has uploaded
               // some of it, which lags behind a job that has just started.
               if (!final)
                   setLogStatus("Waiting for GitHub to publish the log of this running job…");
               else
                   setLogStatus(err.isEmpty() ? "GitHub didn't return a log for this job." : err);
               return;
           }
           applyLog(jobId, out, final);
       });
}

void Backend::applyLog(qlonglong jobId, const QByteArray &raw, bool final) {
    if (jobId != m_jobId)
        return;
    const bool reset = m_logJobId != jobId;
    m_logJobId = jobId;
    m_logFinal = final;
    m_log.setLines(GitHub::parseLog(raw));
    setLogStatus({});
    annotateLog();
    if (reset)
        emit logReset();
}

void Backend::selectBuild(const QString &key) {
    if (key == m_key || !hasVisibleBuild(key))
        return;
    m_cameFrom.clear();
    m_key = key;
    clearJob();
    rebuildJobs();
    fetchJobs();
}

void Backend::selectJob(qlonglong id) {
    // A job GitHub hasn't created yet has nothing to show.
    if (id <= 0)
        return;
    m_jobPinned = true;
    if (id == m_jobId)
        return;
    m_jobId = id;
    m_step = -1;
    m_logJobId = 0;
    m_log.clear();
    annotateLog();
    emit selectionChanged();
    emit logReset();
    fetchLog();
}

void Backend::selectStep(int index) {
    if (index == m_step)
        return;
    m_step = index;
    emit stepsChanged();
}

void Backend::moveBuild(int delta) {
    if (m_builds.isEmpty())
        return;
    const int index = qBound(0, selectedIndex() + delta, int(m_builds.size()) - 1);
    selectBuild(m_builds[index].toMap()["key"].toString());
}

void Backend::moveJob(int delta) {
    // Through the jobs that have something to show; a skipped one doesn't.
    QList<qlonglong> ids;
    for (const auto &job : m_realJobs)
        if (job.toMap()["state"] != "skipped")
            ids << job.toMap()["id"].toLongLong();
    if (ids.isEmpty())
        return;
    const int index = qMax<int>(0, ids.indexOf(m_jobId));
    selectJob(ids[(index + delta + ids.size()) % ids.size()]);
}

int Backend::moveStep(int from, int delta) {
    for (int i = from + delta; i >= 0 && i < m_steps.size(); i += delta) {
        if (m_steps[i].toMap()["line"].toInt() >= 0) {
            selectStep(i);
            return i;
        }
    }
    return -1;
}

void Backend::show(const QString &asked, const QString &sha) {
    static const QRegularExpression validRepo(R"(^[A-Za-z0-9_.-]{1,100}/[A-Za-z0-9_.-]{1,100}$)");
    static const QRegularExpression validOrg(R"(^org:[A-Za-z0-9](?:[A-Za-z0-9-]{0,37}[A-Za-z0-9])?$)",
                                             QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression validSha(R"(^[0-9a-f]{7,40}$)");
    auto repo = asked.trimmed();
    if (repo.isEmpty()) {
        // Started again from the launcher: the window that is open stays on
        // its repository. A new one opens on the last; without one, the
        // window asks which (it shows the menu while the repository is empty).
        if (!m_repo.isEmpty() || m_demo)
            return;
        repo = rememberedRepo();
        if (repo.isEmpty())
            return;
    }
    const bool isOrg = validOrg.match(repo).hasMatch();
    if (!isOrg && !validRepo.match(repo).hasMatch()) {
        setError(QString("Enter a repository as owner/name or an organization as org:name. Received \"%1\".").arg(repo));
        return;
    }
    if (isOrg && m_demo) {
        setError("The demo shows an example repository. Organization watching uses live GitHub data.");
        return;
    }
    if (isOrg)
        repo = repo.toLower();
    if (repo != m_repo) {
        ++m_scope;
        ++m_orgPollId;
        m_orgPublishTimer.stop();
        m_repo = repo;
        m_fetchingRuns = false;
        m_listRuns.clear();
        m_extraRuns.clear();
        m_allBuilds.clear();
        m_buildsByRepository.clear();
        m_jobsOfRun.clear();
        m_jobsInFlight.clear();
        m_defaultBranch.clear();
        m_defaultBranchOf.clear();
        m_kept.clear();
        m_branch.clear();
        m_orgMainOnly = false;
        m_orgRuns.clear();
        m_orgAuthorRuns.clear();
        m_orgCatalog.clear();
        m_orgCatalogCheckedAt = 0;
        m_orgCatalogFirstCheckedAt = 0;
        m_orgCatalogError.clear();
        m_orgUnfilteredUpdated = {};
        m_orgRefreshRequested = false;
        m_orgManualRemaining.clear();
        m_orgActivity.clear();
        m_orgErrors.clear();
        m_orgAuthorErrors.clear();
        m_orgPoll = {};
        m_key.clear();
        m_cameFrom.clear();
        m_commitFetched.clear();
        m_lastUpdated = {};
        m_pullAuthor = isOrg && !m_demo ? QSettings(stateFile(), QSettings::IniFormat)
                                             .value("pullAuthor/" + organization()).toString()
                                       : QString();
        if (!validPullAuthor(m_pullAuthor))
            m_pullAuthor.clear();
        clearJob();
        setError({});
        emit repoChanged();
        emit lastUpdatedChanged();
        emit organizationProgressChanged();
    }
    m_wantCommit = !isOrg && validSha.match(sha).hasMatch() ? sha : QString();
    // The build that was asked for has to be in the list to be picked.
    if (!m_wantCommit.isEmpty())
        m_branch.clear();
    // Picks the commit's build if it is already here; otherwise that happens
    // when its runs arrive.
    filterBuilds();
    refreshRuns(false);
}

void Backend::toggleBranch() {
    if (!organization().isEmpty())
        return;
    m_branch = m_branch.isEmpty() ? selectedBuild()["branch"].toString() : QString();
    filterBuilds();
    fetchJobs();
}

void Backend::toggleMain() {
    if (organization().isEmpty())
        m_branch = mainOnly() ? QString() : mainBranch();
    else
        m_orgMainOnly = !m_orgMainOnly;
    filterBuilds();
    fetchJobs();
}

void Backend::toggleRunning() {
    m_runningOnly = !m_runningOnly;
    // Turned on, it shows what is running, not the finished build you were on.
    if (m_runningOnly && !GitHub::isActive(selectedBuild()["state"].toString()))
        m_key.clear();
    filterBuilds();
    fetchJobs();
}

void Backend::toggleSkipped() {
    m_showSkipped = !m_showSkipped;
    filterBuilds();
    fetchJobs();
}

void Backend::setPullAuthor(const QString &author) {
    const auto org = organization();
    auto login = author.trimmed();
    if (login.startsWith('@'))
        login.remove(0, 1);
    if (org.isEmpty() || login == m_pullAuthor)
        return;
    if (!validPullAuthor(login)) {
        setError("Enter a GitHub username for the PR author.");
        return;
    }
    m_pullAuthor = login;
    m_cameFrom.clear();
    if (!m_demo)
        QSettings(stateFile(), QSettings::IniFormat).setValue("pullAuthor/" + org, login);
    // A filter change starts its own poll while the old requests wind down.
    ++m_orgPollId;
    m_orgPublishTimer.stop();
    m_fetchingRuns = false;
    m_orgRefreshRequested = false;
    m_orgManualRemaining.clear();
    m_orgAuthorRuns.clear();
    m_orgAuthorErrors.clear();
    m_allBuilds.clear();
    m_lastUpdated = login.isEmpty() ? m_orgUnfilteredUpdated : QDateTime();
    emit lastUpdatedChanged();
    setError({});
    publishOrganizationRuns();
    fetchJobs();
    refreshRuns(false);
}

QString Backend::shareUrl() const {
    const auto job = selectedJob();
    const auto build = selectedBuild();
    QVariantMap run;
    for (const auto &value : build["runs"].toList())
        if (run.isEmpty() || value.toMap()["id"] == job["runId"])
            run = value.toMap();
    if (job.isEmpty())
        run = QVariantMap{{"url", build["url"]}};
    return GitHub::shareUrl(repositoryOf(build), job["expected"].toBool() ? QVariantMap{} : job, run);
}

QString Backend::openUrl() const {
    auto url = shareUrl();
    const auto step = m_steps.value(m_step).toMap();
    if (!url.isEmpty() && !selectedJob().isEmpty() && step["number"].toInt() > 0)
        url += QString("#step:%1:1").arg(step["number"].toInt());
    return url;
}

QString Backend::failureReport() const {
    if (m_found.isEmpty())
        return {};
    const auto job = selectedJob();
    const auto build = selectedBuild();
    QString step;
    for (const auto &value : m_steps)
        if (value.toMap()["state"] == "failure" && step.isEmpty())
            step = value.toMap()["name"].toString();

    QStringList out;
    out << QString("CI failed in %1: job \"%2\"%3, on %4 at %5.")
               .arg(repositoryOf(build), job["name"].toString(), step.isEmpty() ? QString() : QString(", step \"%1\"").arg(step),
                    build["branch"].toString(), build["sha"].toString());
    out << shareUrl() << QString();
    for (const auto &failure : m_found)
        if (!failure.location.isEmpty())
            out << QString("- %1  %2").arg(failure.location, failure.title);
    if (out.last() != QString())
        out << QString();
    out << Failures::excerpt(m_log.lines(), m_found);
    return out.join('\n');
}

QString Backend::copyLink() {
    const auto url = shareUrl();
    if (!url.isEmpty())
        QGuiApplication::clipboard()->setText(url);
    return url;
}

bool Backend::copyFailure() {
    const auto report = failureReport();
    if (report.isEmpty())
        return false;
    QGuiApplication::clipboard()->setText(report);
    return true;
}

void Backend::copyLog() {
    QGuiApplication::clipboard()->setText(m_log.text());
}

QString Backend::rememberedRepo() {
    return QSettings(stateFile(), QSettings::IniFormat).value("repository").toString();
}

// Written once a repository has shown its runs, so a typo or a repository
// without access is never what the next window opens on.
void Backend::remember(const QString &repo) {
    if (m_demo || repo == m_remembered)
        return;
    m_remembered = repo;
    QSettings(stateFile(), QSettings::IniFormat).setValue("repository", repo);
}

void Backend::fetchRepositories() {
    if (m_repositoriesLoading)
        return;
    const auto now = QDateTime::currentDateTimeUtc();
    if (m_repositoriesFetched.isValid() && m_repositoriesFetched.secsTo(now) < 300 && m_repositoriesError.isEmpty())
        return;
    m_repositoriesFetched = now;
    m_repositoriesLoading = true;
    m_repositoriesError.clear();
    emit repositoriesChanged();
    // The first time the menu fills as the pages come in; after that it keeps
    // the list it has until the new one is whole.
    fetchRepositoryPage(1, {}, m_repositories.isEmpty());
}

// Most recently pushed first, which puts the repositories someone is working
// in at the top: the menu is mostly opened for one of those.
void Backend::fetchRepositoryPage(int page, const QVariantList &found, bool showPages) {
    const auto path = QString("user/repos?per_page=100&sort=pushed&affiliation=owner,collaborator,organization_member&page=%1")
                          .arg(page);
    gh({"api", path}, [this, page, found, showPages](bool ok, const QByteArray &out, const QString &err) {
        QString parseError;
        const auto repositories = ok ? GitHub::parseRepositories(out, &parseError) : QVariantList{};
        if (!ok || !parseError.isEmpty()) {
            m_repositoriesLoading = false;
            m_repositoriesError = !ok ? (err.isEmpty() ? "Asking GitHub for your repositories failed." : err) : parseError;
            if (!found.isEmpty())
                setRepositories(found);
            emit repositoriesChanged();
            return;
        }
        const auto all = found + repositories;
        // A page that isn't full is the last one. Twenty pages is two
        // thousand repositories, more than a menu is for.
        const bool more = repositories.size() == 100 && page < 20;
        if (!more)
            m_repositoriesLoading = false;
        if (!more || showPages) {
            setRepositories(all);
            emit repositoriesChanged();
        }
        if (more)
            fetchRepositoryPage(page + 1, all, showPages);
    });
}

// Archived repositories go last: their builds are history, never news.
void Backend::setRepositories(const QVariantList &repositories) {
    m_repositories = repositories;
    std::stable_partition(m_repositories.begin(), m_repositories.end(),
                          [](const QVariant &repo) { return !repo.toMap()["archived"].toBool(); });
}

void Backend::setError(const QString &error) {
    if (error == m_error)
        return;
    m_error = error;
    emit errorChanged();
}

void Backend::setLogStatus(const QString &status) {
    if (status == m_logStatus)
        return;
    m_logStatus = status;
    emit logStatusChanged();
}

void Backend::notify(const QVariantMap &build) {
    static const QHash<QString, QString> verbs{
        {"success", "passed"}, {"failure", "failed"}, {"cancelled", "was cancelled"}, {"neutral", "finished"}};
    const int seconds = build["seconds"].toInt();
    const auto took = seconds >= 60 ? QString("%1m %2s").arg(seconds / 60).arg(seconds % 60)
                                    : QString("%1s").arg(seconds);
    const auto summary = QString("Build %1").arg(verbs.value(build["state"].toString(), "finished"));
    const auto branch = organization().isEmpty() ? build["branch"].toString()
                                                 : repositoryOf(build) + " · " + build["branch"].toString();
    const auto body = QString("%1\n%2 · %3").arg(build["title"].toString(), branch, took);
    QStringList args{"--app-name=Omaci", "--icon=omaci", summary, body};
    if (build["state"] == "failure")
        args.prepend("--urgency=critical");
    // Not from a demo, and not from the tests, which run without a screen.
    if (!m_demo && QGuiApplication::platformName() != "offscreen")
        QProcess::startDetached("notify-send", args);
}
