#include "estimate.h"

#include <QSet>
#include <algorithm>
#include <cmath>

#include "github.h"

namespace Estimate {

double quantile(QList<double> values, double q) {
    if (values.isEmpty())
        return -1;
    std::sort(values.begin(), values.end());
    const double at = q * (values.size() - 1);
    const int below = int(std::floor(at));
    const int above = qMin(below + 1, int(values.size()) - 1);
    return values[below] + (values[above] - values[below]) * (at - below);
}

QVariantList likeBuilds(const QVariantList &builds, const QVariantMap &build, int most) {
    const auto started = GitHub::ms(build["startedAt"].toString());
    QVariantList sameBranch, sameEvent, all;
    int passedOnBranch = 0, passedOnEvent = 0;
    for (const auto &value : builds) {
        const auto other = value.toMap();
        const auto state = other["state"].toString();
        if (other["repository"] != build["repository"] || other["key"] == build["key"] ||
            !other["isBuild"].toBool() || GitHub::isActive(state) ||
            state == "skipped" || GitHub::ms(other["startedAt"].toString()) > started)
            continue;
        const bool passed = state == "success";
        all << other;
        if (other["event"] == build["event"]) {
            sameEvent << other;
            passedOnEvent += passed;
        }
        if (other["branch"] == build["branch"]) {
            sameBranch << other;
            passedOnBranch += passed;
        }
    }
    auto chosen = passedOnBranch >= 3 ? sameBranch : passedOnEvent >= 3 ? sameEvent : all;
    return chosen.mid(0, most);
}

Spread spread(const QVariantList &like) {
    QList<double> seconds;
    for (const auto &value : like)
        if (value.toMap()["state"] == "success")
            seconds << value.toMap()["seconds"].toDouble();
    Spread out;
    out.samples = seconds.size();
    if (!seconds.isEmpty()) {
        out.low = quantile(seconds, .25);
        out.typical = quantile(seconds, .5);
        out.high = quantile(seconds, .75);
    }
    return out;
}

namespace {

bool ran(const Job &job) {
    return job.start >= 0 && job.end >= job.start && job.state != "skipped";
}

struct Habit {
    QList<double> lengths;
    QList<double> offsets;
    QSet<QString> after; // Jobs that had always finished before this one started.
    bool anyAfter = false;
    int seen = 0;
};

}

Forecast forecast(const QList<Job> &jobs, double elapsed, const Spread &builds, const QList<Earlier> &earlier) {
    Forecast out;
    out.elapsed = elapsed;

    // Only earlier builds that did what this one is doing say anything about
    // it: once the tests run, a build that skipped them is a different build.
    QList<Earlier> alike;
    for (const auto &build : earlier) {
        bool fits = true;
        for (const auto &job : jobs)
            if ((job.state == "running" || job.state == "success" || job.state == "failure") &&
                !(build.contains(job.key) && ran(build[job.key])))
                fits = false;
        if (fits)
            alike << build;
    }

    QHash<QString, Habit> habits;
    for (const auto &build : alike) {
        for (auto it = build.cbegin(); it != build.cend(); ++it) {
            if (!ran(it.value()))
                continue;
            auto &habit = habits[it.key()];
            habit.lengths << it.value().end - it.value().start;
            habit.offsets << it.value().start;
            QSet<QString> before;
            for (auto other = build.cbegin(); other != build.cend(); ++other)
                if (other.key() != it.key() && ran(other.value()) && other.value().end <= it.value().start + 2)
                    before << other.key();
            habit.after = habit.seen ? habit.after.intersect(before) : before;
            ++habit.seen;
        }
    }

    // The jobs GitHub lists, and the ones it will only create once what they
    // wait for is done.
    QList<Job> all = jobs;
    QSet<QString> present;
    QHash<QString, QString> states;
    for (const auto &job : jobs) {
        present << job.key;
        states[job.key] = job.state;
    }
    QStringList expectedKeys;
    for (auto it = habits.cbegin(); it != habits.cend(); ++it)
        if (!present.contains(it.key()) && it.value().seen * 2 > alike.size())
            expectedKeys << it.key();
    std::sort(expectedKeys.begin(), expectedKeys.end(), [&](const QString &a, const QString &b) {
        const double x = quantile(habits[a].offsets, .5), y = quantile(habits[b].offsets, .5);
        return x == y ? a < b : x < y;
    });
    QSet<QString> dropped;
    for (const auto &key : expectedKeys) {
        // A job that waits for one that failed, was cancelled or was skipped
        // is not going to run.
        bool blocked = false;
        for (const auto &needed : habits[key].after) {
            const auto state = states.value(needed);
            if (state == "failure" || state == "cancelled" || state == "skipped" || dropped.contains(needed))
                blocked = true;
        }
        if (blocked) {
            dropped << key;
            continue;
        }
        Job job;
        job.key = key;
        job.state = "pending";
        all << job;
    }

    // Play the build forward three times: every job quick, usual, and slow.
    const double quantiles[3] = {.25, .5, .75};
    QHash<QString, double> ends[3];
    // The same ends without holding a running job to "not before now", which
    // is what tells a build that is merely busy from one that is overdue.
    double unheld[3] = {0, 0, 0};
    bool complete = !all.isEmpty();
    QList<Job> order = all;
    std::stable_sort(order.begin(), order.end(), [&](const Job &a, const Job &b) {
        auto rank = [&](const Job &job) {
            if (job.start >= 0 && job.state != "queued" && job.state != "pending")
                return job.start;
            return habits.contains(job.key) ? quantile(habits[job.key].offsets, .5) : 1e12;
        };
        return rank(a) < rank(b);
    });

    QHash<QString, JobForecast> forecasts;
    for (const auto &job : order) {
        JobForecast f;
        f.key = job.key;
        f.expected = job.state == "pending";
        const bool hasHabit = habits.contains(job.key);
        if (hasHabit)
            f.typical = quantile(habits[job.key].lengths, .5);

        const bool finished = !GitHub::isActive(job.state) && job.state != "pending";
        double result[3];
        for (int q = 0; q < 3; ++q) {
            if (finished) {
                f.start = job.start;
                result[q] = job.state == "skipped" ? qMax(0.0, job.start) : job.end;
            } else if (job.state == "running" && job.start >= 0) {
                f.start = job.start;
                if (!hasHabit) {
                    complete = false;
                    result[q] = elapsed;
                } else {
                    const double usual = job.start + quantile(habits[job.key].lengths, quantiles[q]);
                    unheld[q] = qMax(unheld[q], usual);
                    result[q] = qMax(elapsed, usual);
                }
            } else {
                if (!hasHabit) {
                    complete = false;
                    result[q] = elapsed;
                    continue;
                }
                const auto &habit = habits[job.key];
                double start = elapsed;
                bool waits = false;
                for (const auto &needed : habit.after) {
                    if (ends[q].contains(needed)) {
                        start = qMax(start, ends[q][needed]);
                        waits = true;
                    }
                }
                if (!waits)
                    start = qMax(start, quantile(habit.offsets, .5));
                if (q == 1)
                    f.start = start;
                result[q] = start + quantile(habit.lengths, quantiles[q]);
            }
            ends[q][job.key] = result[q];
            if (job.state != "running")
                unheld[q] = qMax(unheld[q], result[q]);
        }
        f.low = result[0];
        f.end = result[1];
        f.high = result[2];
        f.late = job.state == "running" && hasHabit && f.typical >= 0 && elapsed - job.start > f.typical + 1;
        forecasts[job.key] = f;
    }
    for (const auto &job : all)
        out.jobs << forecasts[job.key];

    auto latest = [&](int q) {
        double end = elapsed;
        for (const auto value : ends[q])
            end = qMax(end, value);
        return end;
    };

    if (complete && alike.size() >= 2) {
        out.known = true;
        out.basis = "jobs";
        out.samples = alike.size();
        out.total = latest(1);
        out.low = qMin(latest(0), out.total);
        out.high = qMax(latest(2), out.total);
        out.late = elapsed > unheld[2] + 1;
        out.over = qMax(0.0, elapsed - unheld[1]);
    } else if (builds.known()) {
        out.known = true;
        out.basis = "builds";
        out.samples = builds.samples;
        out.total = qMax(elapsed, builds.typical);
        out.low = qMin(builds.low, out.total);
        out.high = qMax(builds.high, out.total);
        out.late = elapsed > builds.high + 1;
        out.over = qMax(0.0, elapsed - builds.typical);
    }
    return out;
}

int rounded(double seconds) {
    const double step = seconds < 60 ? 5 : seconds < 300 ? 10 : seconds < 900 ? 30 : 60;
    return int(std::round(seconds / step) * step);
}

}
