#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QLockFile>
#include <QMessageBox>
#include <QStandardPaths>

#include "AppTypes.hpp"
#include "MainWindow.hpp"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setOrganizationName("twain-acquire");
  QApplication::setApplicationName("twain-acquire");
  QApplication::setApplicationVersion(TA_VERSION);
  registerAppTypes();

  QCommandLineParser cli;
  cli.setApplicationDescription("TWAIN acquisition for intra-oral X-ray.");
  cli.addHelpOption();
  cli.addVersionOption();
  const QCommandLineOption dataDir("data-dir", "Folder for sessions, settings and the export queue.", "dir");
  const QCommandLineOption id("patient-id", "Patient ID.", "id");
  const QCommandLineOption family("family", "Patient family name.", "name");
  const QCommandLineOption given("given", "Patient given name.", "name");
  const QCommandLineOption birth("birth", "Patient birth date, YYYYMMDD.", "date");
  const QCommandLineOption sex("sex", "Patient sex: M, F or O.", "sex");
  const QCommandLineOption accession("accession", "Accession number.", "number");
  const QCommandLineOption layout("layout", "Layout id to start with.", "id");
  const QCommandLineOption source("source", "TWAIN source name to select.", "name");
  const QCommandLineOption simulate("simulate", "Show simulated sources.");
  cli.addOptions({dataDir, id, family, given, birth, sex, accession, layout, source, simulate});
  cli.process(app);

  LaunchOptions launch;
  launch.dataDir = cli.isSet(dataDir) ? cli.value(dataDir)
                                      : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  launch.patient.id = cli.value(id).toStdString();
  launch.patient.familyName = cli.value(family).toStdString();
  launch.patient.givenName = cli.value(given).toStdString();
  launch.patient.birthDate = cli.value(birth).toStdString();
  launch.patient.sex = cli.value(sex).toUpper().toStdString();
  launch.patient.accession = cli.value(accession).toStdString();
  launch.layout = cli.value(layout);
  launch.source = cli.value(source);
  launch.simulate = cli.isSet(simulate);

  // One instance per data folder: two would share sessions and fight over the sensor.
  QDir().mkpath(launch.dataDir);
  QLockFile lock(launch.dataDir + "/instance.lock");
  lock.setStaleLockTime(0);
  if (!lock.tryLock(100)) {
    QMessageBox::warning(nullptr, "twain-acquire", QObject::tr("twain-acquire is already running."));
    return 1;
  }

  MainWindow w(launch);
  w.show();
  return app.exec();
}
