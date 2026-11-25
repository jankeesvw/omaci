#include "compare.h"

#include <QHash>
#include <algorithm>
#include <cmath>

#include "github.h"

namespace Compare {

QVariantMap previous(const QVariantList &builds, const QVariantMap &build, const QString &branch) {
    const auto started = GitHub::ms(build["startedAt"].toString());
    QVariantMap found;
    for (const auto &value : builds) {
        const auto other = value.toMap();
        const auto state = other["state"].toString();
        if (other["repository"] != build["repository"] || other["key"] == build["key"] ||
            !other["isBuild"].toBool() || other["branch"] != branch ||
            GitHub::isActive(state) || state == "skipped")
            continue;
        const auto at = GitHub::ms(other["startedAt"].toString());
        if (at >= started)
            continue;
        if (found.isEmpty() || at > GitHub::ms(found["startedAt"].toString()))
            found = other;
    }
    return found;
}

bool notable(double now, double then) {
    return std::abs(now - then) > qMax(5.0, then * 0.15);
}

static bool ran(const Estimate::Job &job) {
    return job.start >= 0 && job.end >= job.start && job.state != "skipped" && job.state != "queued" &&
           job.state != "pending";
}

QList<Row> jobs(const QList<Estimate::Job> &now, const QList<Estimate::Job> &then, bool finished) {
    QHash<QString, Estimate::Job> earlier;
    for (const auto &job : then)
        earlier.insert(job.key, job);

    QList<Row> rows;
    for (const auto &job : now) {
        Row row;
        row.key = job.key;
        row.nowState = job.state;
        if (ran(job))
            row.now = job.end - job.start;
        const bool known = earlier.contains(job.key);
        if (known) {
            const auto &was = earlier[job.key];
            row.thenState = was.state;
            if (ran(was)) {
                row.then = was.end - was.start;
                row.thenStart = was.start;
                row.thenEnd = was.end;
            }
        }
        const bool both = row.now >= 0 && row.then >= 0;
        if (both)
            row.delta = row.now - row.then;

        if (job.state == "failure" && row.thenState != "failure")
            row.verdict = "broke";
        else if (job.state == "success" && row.thenState == "failure")
            row.verdict = "fixed";
        else if (!known && ran(job))
            row.verdict = "new";
        else if (both && job.state == "running")
            // Still going: only worth a word once it is past the earlier time.
            row.verdict = row.delta > 0 && notable(row.now, row.then) ? "slower" : "";
        else if (both && job.state == "success" && row.thenState == "success")
            row.verdict = !notable(row.now, row.then) ? "same" : row.delta > 0 ? "slower" : "faster";
        rows << row;
        earlier.remove(job.key);
    }
    if (finished) {
        for (const auto &was : then) {
            if (!earlier.contains(was.key) || !ran(was))
                continue;
            Row row;
            row.key = was.key;
            row.thenState = was.state;
            row.then = was.end - was.start;
            row.thenStart = was.start;
            row.thenEnd = was.end;
            row.verdict = "gone";
            rows << row;
        }
    }
    return rows;
}

QStringList notes(const QList<Row> &rows) {
    auto name = [](const Row &row) { return row.key.section('/', -1); };
    auto length = [](double seconds) {
        const int s = int(std::round(std::abs(seconds)));
        return s < 60 ? QString("%1s").arg(s) : s % 60 ? QString("%1m %2s").arg(s / 60).arg(s % 60)
                                                        : QString("%1m").arg(s / 60);
    };
    QStringList out;
    for (const auto &row : rows)
        if (row.verdict == "broke")
            out << name(row) + " broke";
    QList<Row> slower;
    for (const auto &row : rows)
        if (row.verdict == "slower")
            slower << row;
    std::stable_sort(slower.begin(), slower.end(), [](const Row &a, const Row &b) { return a.delta > b.delta; });
    for (const auto &row : slower)
        out << QString("%1 %2 slower").arg(name(row), length(row.delta));
    for (const auto &row : rows)
        if (row.verdict == "gone")
            out << name(row) + " didn't run";
    for (const auto &row : rows)
        if (row.verdict == "fixed")
            out << name(row) + " fixed";
    for (const auto &row : rows)
        if (row.verdict == "faster")
            out << QString("%1 %2 faster").arg(name(row), length(row.delta));
    return out;
}

}
