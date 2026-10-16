#include "HelperClient.hpp"

#include "ta_wire.h"

HelperClient::HelperClient(QObject* parent) : QObject(parent) {
  qRegisterMetaType<HelperClient::Source>();
  watchdog_.setSingleShot(true);
  connect(&watchdog_, &QTimer::timeout, this, &HelperClient::onTimeout);
  connect(&proc_, &QProcess::readyReadStandardOutput, this, &HelperClient::onReadyRead);
  connect(&proc_, &QProcess::finished, this, &HelperClient::onFinished);
  connect(&proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
    if (e == QProcess::FailedToStart) {
      dead_ = true;
      watchdog_.stop();
      emit died(tr("The acquisition helper could not be started (%1).").arg(program_));
    }
  });
  // The helper's stderr is diagnostic text only.
  proc_.setProcessChannelMode(QProcess::SeparateChannels);
  connect(&proc_, &QProcess::readyReadStandardError, this, [this] {
    const QByteArray text = proc_.readAllStandardError().trimmed();
    if (!text.isEmpty()) emit logLine("stderr", QString::fromLocal8Bit(text));
  });
}

HelperClient::~HelperClient() {
  if (proc_.state() != QProcess::NotRunning) {
    expectingExit_ = true;
    proc_.kill();
    proc_.waitForFinished(2000);
  }
}

bool HelperClient::start(const QString& program, const QString& backend) {
  if (isRunning()) return true;
  program_ = program;
  backend_ = backend;
  state_ = "closed";
  buffer_.clear();
  expectingExit_ = false;
  dead_ = false;
  proc_.start(program, {"--backend", backend});
  return true;
}

bool HelperClient::isRunning() const { return !dead_ && proc_.state() != QProcess::NotRunning; }

void HelperClient::send(uint32_t type, const QByteArray& payload, int timeoutMs, const char* what) {
  if (!isRunning()) {
    emit failed("not-running", tr("The acquisition helper is not running."), false);
    return;
  }
  uint8_t h[TA_WIRE_HEADER_SIZE];
  ta_wire_put_header(h, type, uint32_t(payload.size()));
  proc_.write(reinterpret_cast<const char*>(h), sizeof h);
  if (!payload.isEmpty()) proc_.write(payload);
  pendingWhat_ = QString::fromLatin1(what);
  if (timeoutMs > 0) watchdog_.start(timeoutMs);
}

void HelperClient::list(int timeoutMs) {
  listing_.clear();
  send(TA_MSG_LIST, {}, timeoutMs, "listing sources");
}

void HelperClient::open(const ta::Kv& options, int timeoutMs) {
  send(TA_MSG_OPEN, QByteArray::fromStdString(options.serialize()), timeoutMs, "opening the source");
}

void HelperClient::enable(int timeoutMs) { send(TA_MSG_ENABLE, {}, timeoutMs, "arming the source"); }

void HelperClient::disable(int timeoutMs) { send(TA_MSG_DISABLE, {}, timeoutMs, "disarming the source"); }

void HelperClient::close(int timeoutMs) { send(TA_MSG_CLOSE, {}, timeoutMs, "closing the source"); }

void HelperClient::shutdown(int timeoutMs) {
  if (!isRunning()) return;
  expectingExit_ = true;
  send(TA_MSG_QUIT, {}, 0, "shutting down");
  proc_.closeWriteChannel();
  if (!proc_.waitForFinished(timeoutMs)) {
    proc_.kill();
    proc_.waitForFinished(2000);
  }
}

void HelperClient::settle() {
  watchdog_.stop();
  pendingWhat_.clear();
}

void HelperClient::onReadyRead() {
  buffer_.append(proc_.readAllStandardOutput());
  for (;;) {
    if (buffer_.size() < qsizetype(TA_WIRE_HEADER_SIZE)) return;
    ta_wire_header h;
    if (!ta_wire_get_header(reinterpret_cast<const uint8_t*>(buffer_.constData()), &h)) {
      expectingExit_ = true;
      proc_.kill();
      dead_ = true;
      emit died(tr("The acquisition helper sent malformed data and was stopped."));
      return;
    }
    const qsizetype total = qsizetype(TA_WIRE_HEADER_SIZE) + qsizetype(h.length);
    if (buffer_.size() < total) {
      // A large frame is still arriving: that is progress, not a hang.
      if (watchdog_.isActive()) watchdog_.start();
      return;
    }
    const QByteArray payload = buffer_.mid(TA_WIRE_HEADER_SIZE, h.length);
    buffer_.remove(0, total);
    handle(h.type, payload);
  }
}

void HelperClient::handle(uint32_t type, const QByteArray& payload) {
  ta::Kv kv;
  if (type != TA_MSG_FRAME) ta::Kv::parse(payload.toStdString(), kv);
  auto q = [&](const char* key) { return QString::fromStdString(kv.str(key)); };

  switch (type) {
    case TA_MSG_HELLO:
      bits_ = int(kv.integer("bits"));
      emit hello(q("backend"), bits_);
      break;
    case TA_MSG_SOURCE: {
      Source s;
      s.name = q("name");
      s.manufacturer = q("manufacturer");
      s.family = q("family");
      s.version = q("version");
      s.twain2 = kv.boolean("twain2");
      s.program = program_;
      s.backend = backend_;
      s.bits = bits_;
      listing_.push_back(s);
      if (watchdog_.isActive()) watchdog_.start();
      break;
    }
    case TA_MSG_LIST_END:
      settle();
      emit sourcesListed(listing_);
      break;
    case TA_MSG_OPENED:
      settle();
      emit opened(kv);
      break;
    case TA_MSG_STATE:
      settle();
      state_ = q("state");
      if (state_ == "transferring" && transferTimeoutMs_ > 0) {
        // No request is outstanding during a transfer, but a source that
        // hangs half-way must still be caught.
        pendingWhat_ = QStringLiteral("transferring an image");
        watchdog_.start(transferTimeoutMs_);
      }
      emit stateChanged(state_, q("reason"));
      break;
    case TA_MSG_FRAME:
      if (pendingWhat_ == QLatin1String("transferring an image")) settle();
      emit frameReceived(payload);
      break;
    case TA_MSG_ERROR:
      settle();
      emit failed(q("code"), q("text"), kv.boolean("fatal"));
      break;
    case TA_MSG_LOG:
      emit logLine(q("level"), q("text"));
      break;
    default:
      emit logLine("warn", tr("Unknown message %1 from the helper").arg(type));
  }
}

void HelperClient::onTimeout() {
  const QString what = pendingWhat_;
  expectingExit_ = true;
  dead_ = true;
  proc_.kill();
  emit died(tr("The source stopped responding while %1 and was stopped.").arg(what));
}

void HelperClient::onFinished(int code, QProcess::ExitStatus status) {
  watchdog_.stop();
  const bool wasDead = dead_;
  dead_ = true;
  const QString was = state_;
  state_ = "closed";
  if (expectingExit_ || wasDead) return;
  if (status == QProcess::CrashExit)
    emit died(tr("The acquisition helper crashed (the source failed while %1).")
                  .arg(pendingWhat_.isEmpty() ? tr("in state %1").arg(was) : pendingWhat_));
  else
    emit died(tr("The acquisition helper exited unexpectedly (code %1).").arg(code));
}
