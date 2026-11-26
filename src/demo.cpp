#include "demo.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTimeZone>
#include <cmath>

namespace Demo {

namespace {

// When the app started, to the second. Everything is placed relative to it
// rather than to the current time, so the running build really runs: its
// clock counts, its steps move on, and after a few minutes it deploys.
QDateTime origin = QDateTime::currentDateTimeUtc();

const QString web = "https://github.com/acme/storefront";

struct Build {
    int number;
    QString title, branch, sha, actor, event, outcome; // outcome: running, success, failure, cancelled
    int startedAgo;                                    // seconds before the app started
    double pace;                                       // how slow the runners were that time
};

const QList<Build> &builds() {
    static const QList<Build> list{
        {14, "Add gift cards to checkout", "main", "4f1c2ab", "mira", "push", "running", 192, 1.03},
        {13, "Show order totals with tax", "order-totals", "b83e01d", "tomas", "pull_request", "failure", 26 * 60, 1.0},
        {12, "Fix rounding in shipping costs", "main", "0d7b3e9", "mira", "push", "success", 58 * 60, 0.97},
        {11, "Bump rails from 8.1.1 to 8.1.2", "dependabot/bundler/rails-8.1.2", "e2a94c7", "dependabot",
         "pull_request", "success", 71 * 60, 1.02},
        {10, "Send a receipt when an order ships", "main", "7c22f0e", "tomas", "push", "success", 128 * 60, 1.07},
        {9, "Show stock levels on the product page", "main", "a91d04c", "mira", "push", "cancelled", 135 * 60, 1.0},
        {8, "Let customers edit their address", "main", "51be7f2", "tomas", "push", "success", 181 * 60, 0.93},
        {7, "Add gift cards to checkout", "gift-cards", "c07a3de", "mira", "pull_request", "success", 240 * 60, 1.0},
        {6, "Speed up the product search", "main", "9e44b10", "mira", "push", "success", 302 * 60, 1.14},
        {5, "Move the cart to Turbo frames", "main", "d2f6a85", "tomas", "push", "failure", 365 * 60, 1.0},
        {4, "Remember the last shipping method", "main", "68c1e3b", "tomas", "push", "success", 384 * 60, 1.01},
        {3, "Round prices in the cart the same way", "main", "f3a0c97", "mira", "push", "success", 480 * 60, 0.9},
        {2, "Upgrade to Ruby 4.0", "main", "17d5b6e", "tomas", "push", "success", 545 * 60, 1.09},
        {1, "Translate the checkout into Dutch", "main", "be82f41", "mira", "push", "success", 660 * 60, 0.98},
    };
    return list;
}

const Build *buildOfRun(qint64 runId) {
    for (const auto &build : builds())
        if (runId == 9000 + build.number * 2 || runId == 9001 + build.number * 2)
            return &build;
    return nullptr;
}

QString iso(const QDateTime &when) {
    return when.toUTC().toString(Qt::ISODate);
}

QDateTime startOf(const Build &build) {
    return origin.addSecs(-build.startedAgo);
}

// How far into the build we are; a finished build is far past its end.
double elapsedOf(const Build &build) {
    return build.outcome == "running" ? startOf(build).msecsTo(QDateTime::currentDateTimeUtc()) / 1000.0 : 1e9;
}

struct Step {
    QString name;
    double start = 0, end = 0;
    QString outcome = "success";
};

struct Job {
    qint64 id = 0;
    QString name;
    double created = 0, start = 0, end = 0;
    QString outcome = "success";
    QList<Step> steps;
};

// A job's steps: fixed ones before, the one that does the work, fixed ones
// after. When the job fails, the work step is the one that failed.
QList<Step> stepsOf(const Job &job, const QList<QPair<QString, int>> &before, const QString &work,
                    const QStringList &afterWork, const QStringList &post) {
    QList<Step> steps;
    double at = job.start;
    for (const auto &[name, seconds] : before) {
        steps << Step{name, at, at + seconds, "success"};
        at += seconds;
    }
    const double tail = post.size();
    const double workEnd = qMax(at + 1, job.end - tail - afterWork.size());
    const bool failed = job.outcome == "failure", cancelled = job.outcome == "cancelled";
    steps << Step{work, at, workEnd, failed ? "failure" : cancelled ? "cancelled" : "success"};
    at = workEnd;
    for (const auto &name : afterWork) {
        const bool runs = !failed && !cancelled;
        steps << Step{name, at, at + (runs ? 1 : 0), runs ? "success" : "skipped"};
        at += runs ? 1 : 0;
    }
    for (const auto &name : post) {
        steps << Step{name, at, qMin(at + 1, job.end), "success"};
        at = qMin(at + 1, job.end);
    }
    return steps;
}

// The jobs of one run, as they end up. What is shown of them depends on how
// far the build is.
QList<Job> plan(const Build &build, bool linting) {
    const double p = build.pace;
    const qint64 run = 9000 + build.number * 2 + (linting ? 1 : 0);
    const bool failed = build.outcome == "failure", cancelled = build.outcome == "cancelled";
    QList<Job> jobs;
    auto add = [&](const QString &name, double start, double seconds, const QString &outcome = "success") {
        Job job;
        job.id = run * 10 + jobs.size() + 1;
        job.name = name;
        job.created = qMax(0.0, start - 3);
        job.start = start;
        job.end = cancelled ? qMin(start + seconds, 95.0) : start + seconds;
        job.outcome = cancelled ? "cancelled" : outcome;
        jobs << job;
        return jobs.size() - 1;
    };

    if (linting) {
        const int rubocop = add("rubocop", 4, std::round(44 * p));
        const int brakeman = add("brakeman", 4, std::round(37 * p));
        jobs[rubocop].steps = stepsOf(jobs[rubocop], {{"Set up job", 2}, {"Check out the code", 2}, {"Set up Ruby", 3}},
                                      "Run RuboCop", {}, {"Post Check out the code", "Complete job"});
        jobs[brakeman].steps = stepsOf(jobs[brakeman], {{"Set up job", 2}, {"Check out the code", 2}, {"Set up Ruby", 3}},
                                       "Scan for security problems", {}, {"Post Check out the code", "Complete job"});
        return jobs;
    }

    const int changes = add("changes", 4, 9);
    const int test = add("test", 16, failed ? 152 : std::round(268 * p), failed ? "failure" : "success");
    // The pull request that failed also changed the Dockerfile, so its image
    // was built without the cache: something for the comparison to find.
    const int image = add("build", 16, std::round(186 * p * (build.number == 13 ? 1.34 : 1.0)));
    const int cucumber = add("cucumber", 20, std::round(301 * p));
    jobs[changes].steps = stepsOf(jobs[changes], {{"Set up job", 1}, {"Check out the code", 4}},
                                  "Look for changes to the Rails app", {}, {"Post Check out the code", "Complete job"});
    jobs[test].steps = stepsOf(jobs[test],
                               {{"Set up job", 2}, {"Initialize containers", 4}, {"Check out the code", 3},
                                {"Set up Ruby", 2}, {"Install gems", 6}, {"Build the stylesheet", 5},
                                {"Set up databases", 6}},
                               "RSpec tests", {"Save the spec runtimes"},
                               {"Post Check out the code", "Stop containers", "Complete job"});
    jobs[image].steps = stepsOf(jobs[image], {{"Set up job", 2}, {"Check out the code", 3}, {"Set up Ruby", 2},
                                              {"Install gems", 8}},
                                "Build and push image", {}, {"Post Check out the code", "Complete job"});
    jobs[cucumber].steps = stepsOf(jobs[cucumber],
                                   {{"Set up job", 2}, {"Initialize containers", 4}, {"Check out the code", 3},
                                    {"Set up Ruby", 2}, {"Install gems", 6}, {"Build the stylesheet", 5},
                                    {"Set up the database", 4}},
                                   "Cucumber features", {}, {"Post Check out the code", "Stop containers", "Complete job"});

    // Only main deploys, and only what passed.
    double ready = 0;
    for (const auto &job : jobs)
        ready = qMax(ready, job.end);
    if (build.branch == "main" && !cancelled) {
        const int deploy = add("deploy", ready + 5, std::round(76 * p), failed ? "skipped" : "success");
        jobs[deploy].created = ready + 1;
        if (failed) {
            jobs[deploy].start = jobs[deploy].end = ready + 1;
        } else {
            jobs[deploy].steps = stepsOf(jobs[deploy], {{"Set up job", 2}, {"Check out the code", 3}, {"Set up Ruby", 2}},
                                         "Deploy with Kamal", {}, {"Post Check out the code", "Complete job"});
        }
    } else if (!cancelled) {
        const int deploy = add("deploy", ready + 1, 0, "skipped");
        jobs[deploy].created = ready + 1;
    }
    return jobs;
}

double endOf(const QList<Job> &jobs) {
    double end = 0;
    for (const auto &job : jobs)
        end = qMax(end, job.end);
    return end + 2;
}

QJsonObject runJson(const Build &build, bool linting) {
    const auto jobs = plan(build, linting);
    const double elapsed = elapsedOf(build);
    const double end = endOf(jobs);
    const bool done = elapsed >= end;
    bool anyFailed = false, anyCancelled = false;
    for (const auto &job : jobs) {
        anyFailed |= job.outcome == "failure";
        anyCancelled |= job.outcome == "cancelled";
    }
    const auto started = startOf(build).addSecs(linting ? 1 : 0);
    const qint64 id = 9000 + build.number * 2 + (linting ? 1 : 0);
    return {{"id", id},
            {"name", linting ? "Linting & Security" : "Tests & Deploy"},
            {"display_title", build.title},
            {"head_branch", build.branch},
            {"event", build.event},
            {"actor", QJsonObject{{"login", build.actor}}},
            {"run_number", int(id - 4000)},
            {"run_attempt", 1},
            {"head_sha", build.sha + QString("0e7d91c5a3b8f6240d17c9e2ab45f3068d1").left(33)},
            {"html_url", QString("%1/actions/runs/%2").arg(web).arg(id)},
            {"status", done ? "completed" : "in_progress"},
            {"conclusion", done ? QJsonValue(anyFailed ? "failure" : anyCancelled ? "cancelled" : "success") : QJsonValue()},
            {"run_started_at", iso(started)},
            {"updated_at", iso(done ? startOf(build).addMSecs(qint64(end * 1000)) : QDateTime::currentDateTimeUtc())}};
}

QByteArray runs(const QString &sha) {
    QJsonArray list;
    for (const auto &build : builds()) {
        if (!sha.isEmpty() && !sha.startsWith(build.sha))
            continue;
        list << runJson(build, false) << runJson(build, true);
        // A scheduled run on the head of main, which is not part of any build.
        if (build.number == 3 && sha.isEmpty())
            list << QJsonObject{{"id", 8999},
                                {"name", "Nightly evaluations"},
                                {"display_title", "Nightly evaluations"},
                                {"head_branch", "main"},
                                {"event", "schedule"},
                                {"actor", QJsonObject{{"login", "mira"}}},
                                {"run_number", 212},
                                {"run_attempt", 1},
                                {"head_sha", build.sha + QString("0e7d91c5a3b8f6240d17c9e2ab45f3068d1").left(33)},
                                {"html_url", web + "/actions/runs/8999"},
                                {"status", "completed"},
                                {"conclusion", "success"},
                                {"run_started_at", iso(origin.addSecs(-500 * 60))},
                                {"updated_at", iso(origin.addSecs(-479 * 60))}};
    }
    return QJsonDocument(QJsonObject{{"workflow_runs", list}}).toJson(QJsonDocument::Compact);
}

QByteArray jobs(qint64 runId) {
    QJsonArray list;
    const auto *build = buildOfRun(runId);
    if (runId == 8999) {
        const auto started = origin.addSecs(-500 * 60);
        list << QJsonObject{{"id", 89990}, {"run_id", runId}, {"name", "evaluate"}, {"workflow_name", "Nightly evaluations"},
                            {"status", "completed"}, {"conclusion", "success"},
                            {"html_url", QString("%1/actions/runs/8999/job/89990").arg(web)},
                            {"started_at", iso(started)}, {"completed_at", iso(started.addSecs(21 * 60))},
                            {"steps", QJsonArray{}}};
    }
    if (build) {
        const bool linting = runId % 2 == 1;
        const double elapsed = elapsedOf(*build);
        const auto base = startOf(*build);
        auto at = [&](double seconds) { return iso(base.addMSecs(qint64(seconds * 1000))); };
        for (const auto &job : plan(*build, linting)) {
            if (elapsed < job.created)
                continue; // GitHub hasn't created it yet.
            const bool skipped = job.outcome == "skipped";
            const bool done = skipped || elapsed >= job.end;
            const bool running = !done && elapsed >= job.start;
            QJsonArray steps;
            for (int i = 0; i < job.steps.size() && (running || done); ++i) {
                const auto &step = job.steps[i];
                const bool stepDone = elapsed >= step.end || step.outcome == "skipped";
                const bool stepRunning = !stepDone && elapsed >= step.start;
                steps << QJsonObject{{"number", i + 1},
                                     {"name", step.name},
                                     {"status", stepDone ? "completed" : stepRunning ? "in_progress" : "queued"},
                                     {"conclusion", stepDone ? QJsonValue(step.outcome) : QJsonValue()},
                                     {"started_at", stepDone || stepRunning ? QJsonValue(at(step.start)) : QJsonValue()},
                                     {"completed_at", stepDone ? QJsonValue(at(step.end)) : QJsonValue()}};
            }
            list << QJsonObject{{"id", job.id},
                                {"run_id", runId},
                                {"name", job.name},
                                {"workflow_name", linting ? "Linting & Security" : "Tests & Deploy"},
                                {"status", done ? "completed" : running ? "in_progress" : "queued"},
                                {"conclusion", done ? QJsonValue(job.outcome) : QJsonValue()},
                                {"html_url", QString("%1/actions/runs/%2/job/%3").arg(web).arg(runId).arg(job.id)},
                                {"started_at", at(job.start)},
                                {"completed_at", done ? QJsonValue(at(job.end)) : QJsonValue()},
                                {"steps", steps}};
        }
    }
    return QJsonDocument(QJsonObject{{"jobs", list}}).toJson(QJsonDocument::Compact);
}

const QStringList examples{
    "Cart adds a gift card", "Cart removes an expired gift card", "Cart keeps its contents after signing in",
    "Checkout pays with a gift card", "Checkout splits a payment over a card and a gift card",
    "GiftCard rejects a code that was used", "GiftCard never lets the balance go negative",
    "Order ships to the billing address by default", "Order sends a receipt", "Order totals include shipping",
    "ShippingCost rounds to whole cents", "ShippingCost is free above fifty euros",
    "Products lists what is in stock", "Products shows one product", "Orders creates an order",
    "Orders cancels an order", "Search finds a product by its name", "Search ignores accents",
};

// What a step writes, apart from the step that does the work.
QStringList chatter(const QString &step) {
    if (step == "Set up job")
        return {"Current runner version: '2.338.2'", "Runner name: 'runner-3'", "##[group]GITHUB_TOKEN Permissions",
                "Contents: read", "Metadata: read", "##[endgroup]", "Prepare workflow directory",
                "Prepare all required actions", "Complete job name: test"};
    if (step == "Initialize containers")
        return {"##[group]Checking docker version", "##[command]/usr/bin/docker version --format '{{.Server.APIVersion}}'",
                "'1.52'", "##[endgroup]", "##[group]Starting postgres service container",
                "##[command]/usr/bin/docker pull postgres:17", "Status: Image is up to date for postgres:17",
                "##[endgroup]", "postgres service is healthy."};
    if (step == "Check out the code")
        return {"##[group]Run actions/checkout@v6", "with:", "  repository: acme/storefront", "##[endgroup]",
                "Syncing repository: acme/storefront", "[command]/usr/bin/git fetch --depth=1 origin",
                "[command]/usr/bin/git checkout --progress --force main", "Switched to a new branch 'main'"};
    if (step == "Set up Ruby")
        return {"##[group]Run ruby/setup-ruby@v1", "with:", "  ruby-version: .ruby-version", "##[endgroup]",
                "Using ruby 4.0.1 from the tool cache"};
    if (step == "Install gems")
        return {"##[group]Run bundle install", "bundle install", "shell: /usr/bin/bash -e {0}", "##[endgroup]",
                "Fetching gem metadata from https://rubygems.org/.........", "Bundle complete! 87 Gemfile dependencies, 214 gems now installed."};
    if (step == "Build the stylesheet")
        return {"##[group]Run bin/rails tailwindcss:build", "bin/rails tailwindcss:build", "##[endgroup]",
                "Done in 3.2s"};
    if (step.startsWith("Set up database") || step == "Set up the database")
        return {"##[group]Run bin/rails db:create db:schema:load", "bin/rails db:create db:schema:load",
                "##[endgroup]", "Created database 'storefront_test'"};
    if (step.startsWith("Post "))
        return {"Post job cleanup.", "[command]/usr/bin/git version", "git version 2.43.0"};
    if (step == "Stop containers")
        return {"Print service container logs: postgres", "##[command]/usr/bin/docker logs --details 7a20fec39d17",
                " 2026-01-01 10:00:01.104 UTC [329] ERROR:  duplicate key value violates unique constraint \"index_gift_cards_on_code\"",
                " 2026-01-01 10:00:01.104 UTC [329] DETAIL:  Key (code)=(SPRING) already exists.",
                "Stop and remove container: postgres"};
    if (step == "Complete job")
        return {"Cleaning up orphan processes"};
    if (step == "Save the spec runtimes")
        return {"##[group]Run actions/cache/save@v5", "##[endgroup]", "Cache saved with key: rspec-runtime-9028"};
    return {"##[group]Run " + step.toLower(), "##[endgroup]"};
}

QStringList failureText() {
    return {"",
            "Failures:",
            "",
            "  1) OrderTotal includes tax in the total",
            "     Failure/Error: expect(order.total).to eq(121.0)",
            "",
            "       expected: 121.0",
            "            got: 100.0",
            "",
            "       (compared using ==)",
            "     # ./spec/models/order_total_spec.rb:21:in 'block (2 levels) in <main>'",
            "     # ./spec/rails_helper.rb:48:in 'block (3 levels) in <top (required)>'",
            "",
            "  2) Checkout shows the tax on the summary",
            "     Failure/Error: expect(page).to have_text(\"VAT 21%\")",
            "       expected to find text \"VAT 21%\" in \"Order summary Subtotal € 100,00 Total € 100,00\"",
            "     # ./spec/system/checkout_spec.rb:48:in 'block (2 levels) in <main>'",
            "     # ./spec/rails_helper.rb:48:in 'block (3 levels) in <top (required)>'",
            "",
            "Finished in 2 minutes 6.4 seconds (files took 3.79 seconds to load)",
            "78 examples, 2 failures",
            "",
            "Failed examples:",
            "",
            "rspec ./spec/models/order_total_spec.rb:18 # OrderTotal includes tax in the total",
            "rspec ./spec/system/checkout_spec.rb:44 # Checkout shows the tax on the summary",
            "",
            "Randomized with seed 24506",
            ""};
}

// What the step that does the work wrote, by the time it was done.
QStringList work(const Job &job, const Step &step) {
    QStringList lines;
    if (step.name == "RSpec tests") {
        lines << "##[group]Run bundle exec parallel_rspec" << "bundle exec parallel_rspec"
              << "shell: /usr/bin/bash -e {0}" << "env:" << "  RAILS_ENV: test" << "##[endgroup]"
              << "Using recorded test runtime" << "4 processes for 96 specs, ~ 24 specs per process";
        const int shown = int((step.end - step.start) / 1.5);
        for (int i = 0; i < shown; ++i)
            lines << QString("%1 (%2s)").arg(examples[i % examples.size()]).arg(0.08 + (i * 37 % 50) / 100.0, 0, 'f', 2);
        if (job.outcome == "failure") {
            lines << failureText();
            // The other processes finish after the one that failed, which is
            // what buries a failure in a real log.
            for (int process = 2; process <= 4; ++process) {
                lines << "RSpec is shutting down and will print the summary report... Interrupt again to force quit."
                      << "" << QString("Finished in 2 minutes %1 seconds (files took 3.8 seconds to load)").arg(5 + process)
                      << QString("%1 examples, 0 failures").arg(70 + process * 4) << ""
                      << QString("Randomized with seed %1").arg(48322 + process * 911) << "";
            }
            lines << "Tests Failed" << "" << "312 examples, 2 failures" << "" << "Took 133 seconds"
                  << "##[error]Process completed with exit code 1.";
        } else {
            lines << "" << "312 examples, 0 failures" << "" << QString("Took %1 seconds").arg(int(step.end - step.start));
        }
        return lines;
    }
    lines << "##[group]Run " + step.name.toLower() << "##[endgroup]";
    const int shown = int((step.end - step.start) / 3);
    for (int i = 0; i < shown; ++i)
        lines << QString("%1: part %2 done").arg(step.name).arg(i + 1);
    return lines;
}

// What one step wrote, with the times it wrote it at.
QByteArray written(const Build &build, const Job &job, const Step &step) {
    const auto base = startOf(build);
    const bool isWork = step.end - step.start > 12 || step.outcome == "failure";
    const auto lines = isWork ? work(job, step) : chatter(step.name);
    const double until = qMax(step.start + 0.4, step.end - 0.05);
    const double gap = lines.size() > 1 ? (until - step.start - 0.15) / (lines.size() - 1) : 0;
    QByteArray out;
    for (int n = 0; n < lines.size(); ++n) {
        const auto when = base.addMSecs(qint64((step.start + 0.15 + qMax(0.0, gap) * n) * 1000));
        out += (when.toString(Qt::ISODateWithMs) + " " + lines[n] + "\n").toUtf8();
    }
    return out;
}

// The job with this id, or one with id 0.
Job jobOf(const Build &build, qint64 jobId) {
    for (const auto &job : plan(build, (jobId / 10) % 2 == 1))
        if (job.id == jobId)
            return job;
    return {};
}

// The way GitHub has them: the log of a job once the job is done, the log of
// a step once the step is, and nothing (a 404) before that.
QByteArray log(qint64 jobId) {
    const auto *build = buildOfRun(jobId / 10);
    if (!build)
        return "2026-01-01T10:00:00.0000000Z Nothing to see in this job.\n";
    const auto job = jobOf(*build, jobId);
    if (!job.id || elapsedOf(*build) < job.end)
        return {};
    QByteArray out;
    for (const auto &step : job.steps)
        if (step.outcome != "skipped")
            out += written(*build, job, step);
    return out;
}

QByteArray stepLog(qint64 jobId, int number) {
    const auto *build = buildOfRun(jobId / 10);
    const auto job = build ? jobOf(*build, jobId) : Job{};
    if (!job.id || number < 1 || number > job.steps.size())
        return {};
    const auto &step = job.steps[number - 1];
    if (step.outcome == "skipped" || elapsedOf(*build) < step.end)
        return {};
    return written(*build, job, step);
}

// What `user/repos` would list for someone at the invented shop: their own
// repositories and the company's, most recently pushed first.
QByteArray repositories() {
    struct Repo {
        QString name, description;
        bool isPrivate, archived;
        int pushedAgo; // seconds before the app started
    };
    static const QList<Repo> list{
        {"acme/storefront", "The shop", true, false, 192},
        {"acme/storefront-api", "Orders, stock and payments for the shop", true, false, 41 * 60},
        {"mira/dotfiles", "", false, false, 3 * 3600},
        {"acme/design-system", "Buttons, forms and the type scale", true, false, 5 * 3600},
        {"acme/warehouse", "Picking lists and shipping labels", true, false, 26 * 3600},
        {"acme/infrastructure", "Servers, DNS and the deploy pipeline", true, false, 2 * 86400},
        {"acme/mobile", "The shop as an app", true, false, 4 * 86400},
        {"mira/blog", "", false, false, 9 * 86400},
        {"acme/newsletter", "The weekly mail to customers", true, false, 23 * 86400},
        {"acme/handbook", "How we work", false, false, 40 * 86400},
        {"acme/checkout-v1", "The old checkout, before Turbo", true, true, 300 * 86400},
    };
    QJsonArray out;
    for (const auto &repo : list)
        out.append(QJsonObject{{"full_name", repo.name},
                               {"description", repo.description.isEmpty() ? QJsonValue() : QJsonValue(repo.description)},
                               {"private", repo.isPrivate},
                               {"archived", repo.archived},
                               {"pushed_at", iso(origin.addSecs(-repo.pushedAgo))}});
    return QJsonDocument(out).toJson(QJsonDocument::Compact);
}

}

QByteArray respond(const QString &path, const QDateTime &started) {
    origin = QDateTime::fromSecsSinceEpoch(started.toSecsSinceEpoch(), QTimeZone::UTC);
    const auto bare = path.section('?', 0, 0);
    const auto parts = bare.split('/');
    if (bare == "user/repos")
        return repositories();
    if (bare.endsWith("/logs")) {
        const int steps = parts.indexOf("steps");
        if (steps > 0)
            return stepLog(parts.value(steps - 1).toLongLong(), parts.value(steps + 1).toInt());
        return log(parts.value(parts.size() - 2).toLongLong());
    }
    if (bare.endsWith("/jobs"))
        return jobs(parts.value(parts.size() - 2).toLongLong());
    if (parts.size() == 3 && parts.value(0) == "repos")
        return QJsonDocument(QJsonObject{{"default_branch", "main"}}).toJson(QJsonDocument::Compact);
    const auto query = path.section('?', 1);
    QString sha;
    for (const auto &pair : query.split('&'))
        if (pair.startsWith("head_sha="))
            sha = pair.mid(9);
    return runs(sha);
}

}
