#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>

#include "AppTypes.hpp"

// One running acquisition helper. Every request is answered within its
// timeout or the helper is killed: a source that hangs must never hang the
// application, and frames already received are kept.
class HelperClient : public QObject {
  Q_OBJECT

 public:
  struct Source {
    QString name, manufacturer, family, version;
    bool twain2 = false;
    QString program;   // helper executable that found it
    QString backend;   // "twain" or "sim"
    int bits = 0;      // helper bitness
  };

  explicit HelperClient(QObject* parent = nullptr);
  ~HelperClient() override;

  // extraEnv: NAME=value entries added to the helper's environment.
  bool start(const QString& program, const QString& backend, const QStringList& extraArgs = {},
             const QStringList& extraEnv = {});
  bool isRunning() const;
  QString program() const { return program_; }
  QString backend() const { return backend_; }
  QString state() const { return state_; }

  void list(int timeoutMs);
  void open(const ta::Kv& options, int timeoutMs);
  void enable(int timeoutMs);
  void disable(int timeoutMs);
  void close(int timeoutMs);
  // Disarm, close and exit; killed if it does not exit within the timeout.
  // Waits, so use it only when the program is closing.
  void shutdown(int timeoutMs);
  // The same without waiting: the helper is asked to quit, killed after the
  // timeout, and this object deletes itself once the process has ended.
  // Disconnect from its signals first.
  void retire(int timeoutMs);
  // How long one image transfer may take before the helper is stopped.
  void setTransferTimeout(int ms) { transferTimeoutMs_ = ms; }

 signals:
  void hello(QString backend, int bits);
  void sourcesListed(QList<HelperClient::Source> sources);
  void opened(ta::Kv negotiated);
  void stateChanged(QString state, QString reason);
  void frameReceived(QByteArray payload);
  void failed(QString code, QString text, bool fatal);
  void logLine(QString level, QString text);
  // The helper exited, crashed or was killed; the sensor's state is unknown.
  void died(QString why);

 private:
  void send(uint32_t type, const QByteArray& payload, int timeoutMs, const char* what);
  void onReadyRead();
  void onFinished(int code, QProcess::ExitStatus status);
  void onTimeout();
  void handle(uint32_t type, const QByteArray& payload);
  void settle();

  QProcess proc_;
  QByteArray buffer_;
  QTimer watchdog_;
  QString program_, backend_, state_ = "closed";
  QString pendingWhat_;
  // What the watchdog is waiting for: a request type (TA_MSG_*), kTransfer,
  // or 0. Only the reply to that request settles it.
  uint32_t pending_ = 0;
  static constexpr uint32_t kTransfer = 0xFFFFu;
  bool answers(uint32_t type, const QString& state) const;
  QList<Source> listing_;
  int bits_ = 0;
  int transferTimeoutMs_ = 30000;
  bool expectingExit_ = false;
  bool dead_ = true;
};

Q_DECLARE_METATYPE(HelperClient::Source)
