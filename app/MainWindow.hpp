#pragma once

#include <QList>
#include <QMainWindow>
#include <QThread>
#include <QTimer>
#include <memory>
#include <vector>

#include "AppTypes.hpp"
#include "Dialogs.hpp"
#include "HelperClient.hpp"
#include "export.hpp"
#include "handoff.hpp"
#include "layout.hpp"
#include "profile.hpp"

class HandoffChannel;
class QSettings;
class QFileSystemWatcher;
class ImageView;
class MountView;
class Stager;
class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

struct LaunchOptions {
  QString dataDir;
  QStringList args;  // the program's arguments, parsed for a patient handoff
  QString layout;
  QString source;
  bool simulate = false;
};

class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(const LaunchOptions& launch, QWidget* parent = nullptr);
  ~MainWindow() override;

 protected:
  void closeEvent(QCloseEvent* e) override;

 private:
  // setup
  void buildUi();
  void loadConfig();
  void offerRecovery();

  // sources and helper
  void scanSources();
  QStringList helperPrograms(QStringList* backends) const;
  QStringList helperArgs(const QString& backend) const;
  QStringList helperEnv(const QString& program, const QString& backend) const;
  const HelperClient::Source* currentSource() const;
  void onSourceChanged();
  void editProfile();
  bool ensureHelper();
  void openSource();

  // arming
  void toggleArm();
  void arm();
  void disarm(const QString& why);
  void onHelperState(const QString& state, const QString& reason);
  void onHelperDied(const QString& why);
  void onFrame(const QByteArray& payload);
  void onImageStaged(uint seq, int slot);
  void updateBanner();

  // session
  bool ensureSession();
  bool settleCurrentSession(const QString& question);
  void newSession();
  void onSessionChanged(const ta::SessionState& state, const QString& dir);
  void onSessionClosed();
  ta::PatientInfo patientFromFields(QString* problem) const;
  void setPatientFields(const ta::PatientInfo& p);
  void pushPatient();

  // images
  void selectImage(int seq);
  void rotateSelected(int turns);
  void mirrorSelected();
  void discardSelected();
  void showImageMenu(int seq, const QPoint& at);

  // export
  void exportSession();
  void editTargets();
  void refreshExportStatus();
  bool lookupTarget(const std::string& id, ta::ExportTarget& out) const;

  void editOptions();

  // patient handoff from practice software
  ta::HandoffTemplate handoffTemplate() const;
  void applyHandoff(const QStringList& args, bool atStartup);
  ta::HandoffFileMapping fileMapping() const;
  bool readHandoffFile(const QString& path, ta::HandoffResult& out);
  void watchHandoffFile();
  void checkWatchedFile(bool atStartup);
  // Apply a patient from the practice software, with the session guard.
  void applyPatient(const ta::PatientInfo& incoming, const QString& origin);
  void log(const QString& level, const QString& text);
  void status(const QString& text);
  const ta::Layout& layout() const;

  LaunchOptions launch_;
  std::unique_ptr<QSettings> settings_;  // <data folder>/settings.ini
  QString dataDir_;
  ta::fs::Path profilesDir_, targetsDir_, layoutsDir_;
  AppOptions options_;
  std::vector<ta::Layout> layouts_;
  std::vector<ta::ExportTarget> targets_;
  QList<HelperClient::Source> sources_;
  ta::SourceProfile profile_;

  HelperClient* helper_ = nullptr;
  QList<HelperClient*> scanners_;
  bool armAfterOpen_ = false;
  bool helperReady_ = false;  // HELLO received from the current helper
  bool stateUnknown_ = false;
  QString helperState_ = "closed";
  QTimer armTimer_;

  QThread stagerThread_;
  Stager* stager_ = nullptr;
  ta::SessionState session_;
  QString sessionDir_;
  bool haveSession_ = false;

  std::unique_ptr<ta::ExportQueue> queue_;
  QTimer exportTimer_;

  int viewedSeq_ = -1;
  HandoffChannel* handoff_ = nullptr;
  QFileSystemWatcher* watcher_ = nullptr;
  QTimer watchDebounce_;
  QByteArray lastWatchedHash_;
  QString patientOrigin_;  // where the patient fields came from, shown before export

  // widgets
  QComboBox *sourceBox_ = nullptr, *layoutBox_ = nullptr, *sexBox_ = nullptr;
  QLineEdit *patientId_ = nullptr, *family_ = nullptr, *given_ = nullptr, *birth_ = nullptr;
  QPushButton* armButton_ = nullptr;
  QLabel* banner_ = nullptr;
  QLabel* exportLabel_ = nullptr;
  QLabel* helperLabel_ = nullptr;
  QPushButton* retryButton_ = nullptr;
  MountView* mount_ = nullptr;
  ImageView* view_ = nullptr;
  QPlainTextEdit* logView_ = nullptr;
};
