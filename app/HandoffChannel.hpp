/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <QByteArray>
#include <QHash>
#include <QLocalServer>
#include <QObject>
#include <QString>
#include <QStringList>

// Passes a second launch's arguments to the running window. Practice software
// launches twain-acquire for each patient; the running instance must receive
// the new patient, not refuse to start.
//
// The channel is a local socket named from the data folder and limited to the
// current user, so another account on the PC cannot inject a patient.
class HandoffChannel : public QObject {
  Q_OBJECT

 public:
  explicit HandoffChannel(const QString& dataDir, QObject* parent = nullptr);

  // Running instance: start listening. Call only while holding the instance lock.
  bool listen(QString* error = nullptr);

  // Second launch: send `args` to the running instance. Returns true once it
  // has confirmed receipt within `timeoutMs`.
  static bool send(const QString& dataDir, const QStringList& args, int timeoutMs);

  static QString serverName(const QString& dataDir);

 signals:
  void argumentsReceived(QStringList args);

 private:
  QString dataDir_;
  QLocalServer server_;
  QHash<QObject*, QByteArray> pending_;  // partial messages per connection
};
