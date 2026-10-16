#include "HandoffChannel.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QLocalSocket>

#include "kv.hpp"

namespace {

// Arguments travel as key=value text: arg.0, arg.1, ... then "end".
QByteArray encode(const QStringList& args) {
  ta::Kv kv;
  for (int i = 0; i < args.size(); ++i) kv.set("arg." + std::to_string(i), args[i].toStdString());
  kv.set("end", "1");
  return QByteArray::fromStdString(kv.serialize());
}

bool decode(const QByteArray& bytes, QStringList& args) {
  ta::Kv kv;
  if (!ta::Kv::parse(bytes.toStdString(), kv) || !kv.get("end")) return false;
  for (int i = 0;; ++i) {
    auto v = kv.get("arg." + std::to_string(i));
    if (!v) break;
    args << QString::fromStdString(*v);
  }
  return true;
}

constexpr qsizetype kMaxMessage = 64 * 1024;

}  // namespace

HandoffChannel::HandoffChannel(const QString& dataDir, QObject* parent) : QObject(parent), dataDir_(dataDir) {
  server_.setSocketOptions(QLocalServer::UserAccessOption);
  connect(&server_, &QLocalServer::newConnection, this, [this] {
    while (QLocalSocket* s = server_.nextPendingConnection()) {
      connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
      connect(s, &QObject::destroyed, this, [this, s] { pending_.remove(s); });
      connect(s, &QLocalSocket::readyRead, this, [this, s] {
        QByteArray& buf = pending_[s];
        buf += s->readAll();
        if (buf.size() > kMaxMessage) {
          s->abort();
          return;
        }
        QStringList args;
        if (!decode(buf, args)) return;  // not complete yet
        s->write("ok\n");
        s->flush();
        s->disconnectFromServer();
        emit argumentsReceived(args);
      });
    }
  });
}

QString HandoffChannel::serverName(const QString& dataDir) {
  const QByteArray key = QDir::cleanPath(QDir(dataDir).absolutePath()).toLower().toUtf8();
  return "twain-acquire-" + QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex().left(16);
}

bool HandoffChannel::listen(QString* error) {
  const QString name = serverName(dataDir_);
  // We hold the instance lock, so a leftover name is from a crashed instance.
  QLocalServer::removeServer(name);
  if (server_.listen(name)) return true;
  if (error) *error = server_.errorString();
  return false;
}

bool HandoffChannel::send(const QString& dataDir, const QStringList& args, int timeoutMs) {
  QLocalSocket s;
  s.connectToServer(serverName(dataDir));
  if (!s.waitForConnected(timeoutMs)) return false;
  s.write(encode(args));
  if (!s.waitForBytesWritten(timeoutMs)) return false;
  QByteArray reply;
  while (!reply.contains('\n')) {
    if (!s.waitForReadyRead(timeoutMs)) return false;
    reply += s.readAll();
  }
  return reply.startsWith("ok");
}
