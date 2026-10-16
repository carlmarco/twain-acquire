// Drives the real helper process (simulated backend) through HelperClient.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTemporaryDir>
#include <atomic>
#include <functional>
#include <thread>

#include "HandoffChannel.hpp"
#include "HelperClient.hpp"
#include "Stager.hpp"
#include "check.hpp"
#include "normalize.hpp"

namespace {

bool waitFor(const std::function<bool()>& done, int ms) {
  QElapsedTimer t;
  t.start();
  while (!done()) {
    if (t.elapsed() > ms) return false;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  return true;
}

struct Recorder {
  HelperClient client;
  bool hello = false;
  QList<HelperClient::Source> sources;
  bool listed = false;
  ta::Kv opened;
  bool isOpened = false;
  QStringList states;
  QList<QByteArray> frames;
  QStringList errors;
  QString died;

  Recorder() {
    QObject::connect(&client, &HelperClient::hello, [this] { hello = true; });
    QObject::connect(&client, &HelperClient::sourcesListed, [this](QList<HelperClient::Source> s) {
      sources = s;
      listed = true;
    });
    QObject::connect(&client, &HelperClient::opened, [this](ta::Kv kv) {
      opened = kv;
      isOpened = true;
    });
    QObject::connect(&client, &HelperClient::stateChanged, [this](QString s, QString) { states << s; });
    QObject::connect(&client, &HelperClient::frameReceived, [this](QByteArray p) { frames << p; });
    QObject::connect(&client, &HelperClient::failed, [this](QString code, QString, bool) { errors << code; });
    QObject::connect(&client, &HelperClient::died, [this](QString why) { died = why; });
  }

  bool start() {
    client.start(TA_HELPER_PATH, "sim");
    return waitFor([this] { return hello; }, 5000);
  }

  bool open(const char* source, int count = 2, int bitDepth = 0) {
    ta::Kv kv;
    kv.set("source", source);
    kv.setInt("sim.width", 64);
    kv.setInt("sim.height", 48);
    kv.setInt("sim.interval-ms", 40);
    kv.setInt("transfer-count", count);
    kv.setInt("bit-depth", bitDepth);
    client.open(kv, 3000);
    return waitFor([this] { return isOpened || !errors.isEmpty(); }, 5000) && isOpened;
  }
};

ta::Frame decode(const QByteArray& p) {
  ta::Frame f;
  ta::normalizeTransfer(reinterpret_cast<const uint8_t*>(p.constData()), size_t(p.size()), {}, f);
  return f;
}

}  // namespace

TEST("helper: list, open, arm, receive, auto-disarm, close, exit") {
  Recorder r;
  CHECK(r.start());
  r.client.list(3000);
  CHECK(waitFor([&] { return r.listed; }, 5000));
  CHECK(r.sources.size() == 5);
  CHECK(r.sources.value(0).name == "Simulated sensor" && r.sources.value(0).backend == "sim");

  CHECK(r.open("Simulated sensor"));
  CHECK(r.states.last() == "open");
  r.client.enable(3000);
  CHECK(waitFor([&] { return r.frames.size() == 2 && r.states.last() == "open"; }, 5000));
  CHECK(r.states.contains("armed") && r.states.contains("transferring"));
  const ta::Frame f = decode(r.frames.value(0));
  CHECK(f.valid() && f.width == 64 && f.height == 48 && f.bitsStored == 16 && f.dpiX == 1270.0);
  r.client.close(3000);
  CHECK(waitFor([&] { return r.states.last() == "closed"; }, 3000));
  r.client.shutdown(3000);
  CHECK(!r.client.isRunning());
  CHECK(r.died.isEmpty() && r.errors.isEmpty());
}

TEST("helper: requests in the wrong state are refused and change nothing") {
  Recorder r;
  CHECK(r.start());
  r.client.enable(3000);
  CHECK(waitFor([&] { return !r.errors.isEmpty(); }, 3000));
  CHECK(r.errors.value(0) == "state");
  r.errors.clear();
  CHECK(r.open("No such sensor") == false);
  CHECK(r.errors.contains("no-source"));
  r.client.shutdown(3000);
}

TEST("helper: a source that hangs while arming is stopped by the watchdog") {
  Recorder r;
  CHECK(r.start());
  CHECK(r.open("Simulated sensor (hangs when armed)"));
  QElapsedTimer t;
  t.start();
  r.client.enable(400);
  CHECK(waitFor([&] { return !r.died.isEmpty(); }, 4000));
  CHECK(t.elapsed() < 3000);
  CHECK(r.died.contains("stopped responding"));
  CHECK(!r.client.isRunning());
}

TEST("helper: a source that crashes mid-transfer is reported, not hung on") {
  Recorder r;
  CHECK(r.start());
  CHECK(r.open("Simulated sensor (crashes during transfer)"));
  r.client.enable(3000);
  CHECK(waitFor([&] { return !r.died.isEmpty(); }, 5000));
  CHECK(r.died.contains("crashed"));
  CHECK(r.frames.isEmpty());
}

TEST("helper: an 8-bit-only source reports the refusal and sends 8 bits") {
  Recorder r;
  CHECK(r.start());
  CHECK(r.open("Simulated sensor (8-bit only)", 1, 16));
  CHECK(!r.opened.str("note").empty() && r.opened.integer("bit-depth") == 8);
  r.client.enable(3000);
  CHECK(waitFor([&] { return r.frames.size() == 1; }, 5000));
  CHECK(decode(r.frames.value(0)).bitsStored == 8);
  r.client.shutdown(3000);
}

TEST("helper: big-endian min-is-white data normalises to the same image") {
  Recorder a, b;
  CHECK(a.start() && b.start());
  CHECK(a.open("Simulated sensor", 1) && b.open("Simulated sensor (big-endian, min-is-white)", 1));
  a.client.enable(3000);
  b.client.enable(3000);
  CHECK(waitFor([&] { return a.frames.size() == 1 && b.frames.size() == 1; }, 5000));
  const ta::Frame fa = decode(a.frames.value(0)), fb = decode(b.frames.value(0));
  CHECK(fa.valid() && fa.pixels == fb.pixels);
  a.client.shutdown(3000);
  b.client.shutdown(3000);
}

TEST("helper: shutting down while armed disarms and exits") {
  Recorder r;
  CHECK(r.start());
  CHECK(r.open("Simulated sensor", -1));
  r.client.enable(3000);
  CHECK(waitFor([&] { return r.states.contains("armed"); }, 3000));
  r.client.shutdown(3000);
  CHECK(!r.client.isRunning());
  CHECK(r.died.isEmpty());
}

TEST("stager: a received frame is on disk before it is reported") {
  QTemporaryDir dir;
  Stager s(dir.path());
  ta::SessionState last;
  QString sessionDir;
  int staged = 0;
  QObject::connect(&s, &Stager::sessionChanged, [&](ta::SessionState st, QString d) {
    last = st;
    sessionDir = d;
  });
  QObject::connect(&s, &Stager::imageStaged, [&](uint, int) { ++staged; });
  QStringList problems;
  QObject::connect(&s, &Stager::problem, [&](QString p) { problems << p; });

  s.startSession("bitewings-2", "Sim", ta::PatientInfo{"7", "Roe", "", "", "", ""});
  Recorder r;
  CHECK(r.start() && r.open("Simulated sensor", 1));
  r.client.enable(3000);
  CHECK(waitFor([&] { return r.frames.size() == 1; }, 5000));
  s.addFrame(r.frames.value(0), {}, ta::Orientation{1, false}, 0);
  s.addFrame(r.frames.value(0), {}, {}, 0);  // retake into the same slot
  s.addFrame(QByteArray("junk"), {}, {}, 1);
  CHECK(staged == 2);
  CHECK(problems.size() == 1);
  CHECK(last.images.size() == 2 && last.image(1)->slot == -1 && last.image(2)->slot == 0);
  CHECK(last.image(1)->orientation == (ta::Orientation{1, false}));
  ta::SessionState disk;
  CHECK(ta::StagingSession::readState(ta::fs::fromUtf8(sessionDir.toStdString()), disk));
  CHECK(disk.images.size() == 2 && disk.patient.familyName == "Roe");
  r.client.shutdown(3000);
}

TEST("handoff channel: a second launch's arguments reach the running instance") {
  QTemporaryDir dir;
  CHECK(!HandoffChannel::send(dir.path(), {"x"}, 300));  // nobody listening yet
  HandoffChannel server(dir.path());
  QString err;
  CHECK(server.listen(&err));
  QStringList got;
  int received = 0;
  QObject::connect(&server, &HandoffChannel::argumentsReceived, [&](QStringList a) {
    got = a;
    ++received;
  });
  const QStringList sent{"/PATID", "123 456", "/NAME", "Zoë", "/SURNAME", "O'Neil\tTab"};
  std::atomic<int> ok{-1};
  std::thread client([&] { ok = HandoffChannel::send(dir.path(), sent, 3000) ? 1 : 0; });
  CHECK(waitFor([&] { return received == 1 && ok != -1; }, 5000));
  client.join();
  CHECK(ok == 1);
  CHECK(got == sent);
  CHECK(HandoffChannel::serverName(dir.path()) == HandoffChannel::serverName(dir.path() + "/"));
}

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  registerAppTypes();
  return check::runAll();
}
