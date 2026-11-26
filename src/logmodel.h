#pragma once

#include <QAbstractListModel>
#include <QHash>

#include "failures.h"
#include "github.h"

// The log of one job, line by line. A running job's log grows a step at a
// time, and the whole log follows on from it once the job is done; when the
// new text only adds lines, they're appended so the view keeps its place
// instead of jumping back to the top.
//
// Besides the text, a line knows whether a step starts on it (the view draws
// a heading there) and whether it is part of a failure.
class LogModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role { TimeRole = Qt::UserRole + 1, TextRole, KindRole, StepRole, MarkRole };
    enum Mark { NoMark = 0, InFailure, FailureLine };

    using QAbstractListModel::QAbstractListModel;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setLines(const QList<GitHub::LogLine> &lines);
    void clear();
    const QList<GitHub::LogLine> &lines() const { return m_lines; }
    QString text() const;

    // Where each step starts (-1 for none) and what it is called.
    void setSteps(const QList<int> &starts, const QStringList &names);
    void setFailures(const QList<Failures::Failure> &failures);

    // The step the given line belongs to, as an index into the steps, or -1.
    Q_INVOKABLE int stepAt(int row) const;
    // The line after which the step that starts at `row` ends.
    int stepEnd(int stepIndex) const;

private:
    void touch(const QList<int> &roles);

    QList<GitHub::LogLine> m_lines;
    QList<int> m_starts;
    QHash<int, QString> m_headings;
    QHash<int, int> m_marks;
};
