#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>

// Invented runs, jobs and logs for screenshots and for trying the app without
// a GitHub login: one build running, its log growing a step at a time the
// way GitHub publishes it, one that failed, and a few that passed. Answers
// the same API paths the app asks gh for; a null answer is a 404.
namespace Demo {

QByteArray respond(const QString &path, const QDateTime &started);

}
