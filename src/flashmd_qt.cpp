/*
 * FlashMD Qt GUI
 * Sega Genesis/Mega Drive ROM Flasher - Qt Interface
 *
 * Uses Qt for native desktop integration
 * Uses libusb via flashmd_core for USB operations
 */

#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QProgressBar>
#include <QTextEdit>
#include <QFileDialog>
#include <QMessageBox>
#include <QThread>
#include <QMutex>
#include <QTimer>
#include <QStyle>
#include <QStyleFactory>
#include <QSettings>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>

#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>

extern "C" {
#include "flashmd_core.h"
}

/* Size options */
static const uint32_t SIZE_VALUES[] = {0, 128, 256, 512, 1024, 2048, 4096};
static const char* SIZE_LABELS[] = {"Auto", "128 KB", "256 KB", "512 KB", "1 MB", "2 MB", "4 MB"};

/*
 * Configuration file management
 * Stores paths in ~/.config/flashmd/config.ini
 */
static QString getConfigPath() {
    QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    configDir += "/flashmd";
    QDir dir;
    if (!dir.exists(configDir)) {
        dir.mkpath(configDir);
    }
    return configDir + "/config.ini";
}

static QString getSavedPath(const QString &key, const QString &defaultPath = QString()) {
    QSettings settings(getConfigPath(), QSettings::IniFormat);
    return settings.value(key, defaultPath).toString();
}

static void savePath(const QString &key, const QString &path) {
    QSettings settings(getConfigPath(), QSettings::IniFormat);
    settings.setValue(key, path);
    settings.sync();
}

/*
 * IPC for privilege separation (Linux only)
 * When run with sudo, we fork: parent (root) handles USB, child (user) runs GUI
 */
#ifdef __linux__

enum IpcMsgType {
    IPC_COMMAND = 1,
    IPC_PROGRESS,
    IPC_LOG,
    IPC_RESULT,
    IPC_QUIT
};

struct IpcCommand {
    IpcMsgType type;
    int operation;
    char filepath[512];
    uint32_t sizeKb;
    int noTrim;
    int verbose;
    int fullErase;
};

struct IpcProgress {
    IpcMsgType type;
    uint32_t current;
    uint32_t total;
};

struct IpcLog {
    IpcMsgType type;
    int isError;
    char message[256];
};

struct IpcResult {
    IpcMsgType type;
    int result;
};

static int g_pipeToUsb[2] = {-1, -1};
static int g_pipeToGui[2] = {-1, -1};
static bool g_usingIpc = false;

/* IPC callbacks for USB handler */
static int g_ipcWriteFd = -1;

static void ipcProgressCb(uint32_t current, uint32_t total, void *) {
    if (g_ipcWriteFd < 0) return;
    IpcProgress msg = {IPC_PROGRESS, current, total};
    write(g_ipcWriteFd, &msg, sizeof(msg));
}

static void ipcMessageCb(const char *text, int isError, void *) {
    if (g_ipcWriteFd < 0) return;
    IpcLog msg = {IPC_LOG, isError, {}};
    strncpy(msg.message, text, sizeof(msg.message) - 1);
    write(g_ipcWriteFd, &msg, sizeof(msg));
}

/* USB handler loop - runs in root process */
static void usbHandlerLoop(int readFd, int writeFd) {
    g_ipcWriteFd = writeFd;
    IpcCommand cmd;

    while (true) {
        ssize_t n = read(readFd, &cmd, sizeof(cmd));
        if (n <= 0) break;
        if (cmd.type == IPC_QUIT) break;
        if (cmd.type != IPC_COMMAND) continue;

        flashmd_config_t config;
        flashmd_config_init(&config);
        config.verbose = cmd.verbose;
        config.no_trim = cmd.noTrim;
        config.progress = ipcProgressCb;
        config.message = ipcMessageCb;

        flashmd_result_t result = flashmd_open();
        if (result != FLASHMD_OK) {
            IpcLog logMsg = {IPC_LOG, 1, {}};
            snprintf(logMsg.message, sizeof(logMsg.message),
                     "Could not open USB: %s", flashmd_error_string(result));
            write(writeFd, &logMsg, sizeof(logMsg));
            IpcResult resMsg = {IPC_RESULT, (int)result};
            write(writeFd, &resMsg, sizeof(resMsg));
            continue;
        }

        switch (cmd.operation) {
            case 1: result = flashmd_connect(&config); break;
            case 2: result = flashmd_check_id(&config); break;
            case 3: {
                uint32_t sz = cmd.fullErase ? 0 : (cmd.sizeKb ? cmd.sizeKb : 4096);
                result = flashmd_erase(sz, &config);
                break;
            }
            case 4: result = flashmd_read_rom(cmd.filepath, cmd.sizeKb, &config); break;
            case 5: result = flashmd_write_rom(cmd.filepath, cmd.sizeKb, &config); break;
            case 6: result = flashmd_read_sram(cmd.filepath, &config); break;
            case 7: result = flashmd_write_sram(cmd.filepath, &config); break;
        }
        flashmd_close();

        IpcResult resMsg = {IPC_RESULT, (int)result};
        write(writeFd, &resMsg, sizeof(resMsg));
    }
    g_ipcWriteFd = -1;
}

#endif /* __linux__ */

/*
 * Worker thread for USB operations
 */
class UsbWorker : public QThread {
    Q_OBJECT

public:
    enum Operation {
        OP_NONE = 0,
        OP_CONNECT,
        OP_CHECK_ID,
        OP_ERASE,
        OP_READ_ROM,
        OP_WRITE_ROM,
        OP_READ_SRAM,
        OP_WRITE_SRAM
    };

    UsbWorker(QObject *parent = nullptr) : QThread(parent) {}

    void setOperation(Operation op, const QString &filepath = QString(),
                      uint32_t sizeKb = 0, bool noTrim = false,
                      bool verbose = false, bool fullErase = false) {
        m_operation = op;
        m_filepath = filepath;
        m_sizeKb = sizeKb;
        m_noTrim = noTrim;
        m_verbose = verbose;
        m_fullErase = fullErase;
    }

signals:
    void progressChanged(int current, int total);
    void logMessage(const QString &message, bool isError);
    void operationFinished(bool success, const QString &errorMsg);

protected:
    void run() override {
#ifdef __linux__
        if (g_usingIpc) {
            runViaIpc();
            return;
        }
#endif
        runLocal();
    }

    void runLocal() {
        flashmd_config_t config;
        flashmd_config_init(&config);
        config.verbose = m_verbose;
        config.no_trim = m_noTrim;
        config.progress = progressCallback;
        config.message = messageCallback;
        config.user_data = this;

        flashmd_result_t result = flashmd_open();
        if (result != FLASHMD_OK) {
            emit operationFinished(false, QString("Could not open USB device: %1")
                                   .arg(flashmd_error_string(result)));
            return;
        }

        switch (m_operation) {
            case OP_CONNECT:
                result = flashmd_connect(&config);
                break;
            case OP_CHECK_ID:
                result = flashmd_check_id(&config);
                break;
            case OP_ERASE: {
                uint32_t eraseSize = m_sizeKb;
                if (m_fullErase) {
                    eraseSize = 0;
                } else if (eraseSize == 0) {
                    eraseSize = 4096;
                }
                result = flashmd_erase(eraseSize, &config);
                break;
            }
            case OP_READ_ROM:
                result = flashmd_read_rom(m_filepath.toUtf8().constData(), m_sizeKb, &config);
                break;
            case OP_WRITE_ROM:
                result = flashmd_write_rom(m_filepath.toUtf8().constData(), m_sizeKb, &config);
                break;
            case OP_READ_SRAM:
                result = flashmd_read_sram(m_filepath.toUtf8().constData(), &config);
                break;
            case OP_WRITE_SRAM:
                result = flashmd_write_sram(m_filepath.toUtf8().constData(), &config);
                break;
            default:
                break;
        }

        flashmd_close();

        if (result == FLASHMD_OK) {
            emit operationFinished(true, QString());
        } else {
            emit operationFinished(false, QString(flashmd_error_string(result)));
        }
    }

#ifdef __linux__
    void runViaIpc() {
        /* Send command to root USB handler */
        IpcCommand cmd = {};
        cmd.type = IPC_COMMAND;
        cmd.operation = (int)m_operation;
        strncpy(cmd.filepath, m_filepath.toUtf8().constData(), sizeof(cmd.filepath) - 1);
        cmd.sizeKb = m_sizeKb;
        cmd.noTrim = m_noTrim;
        cmd.verbose = m_verbose;
        cmd.fullErase = m_fullErase;

        write(g_pipeToUsb[1], &cmd, sizeof(cmd));

        /* Read responses until we get a result */
        while (true) {
            IpcMsgType msgType;
            ssize_t n = read(g_pipeToGui[0], &msgType, sizeof(msgType));
            if (n <= 0) {
                emit operationFinished(false, "IPC error");
                return;
            }

            if (msgType == IPC_PROGRESS) {
                IpcProgress msg;
                msg.type = msgType;
                read(g_pipeToGui[0], ((char*)&msg) + sizeof(msgType), sizeof(msg) - sizeof(msgType));
                emit progressChanged(msg.current, msg.total);
            }
            else if (msgType == IPC_LOG) {
                IpcLog msg;
                msg.type = msgType;
                read(g_pipeToGui[0], ((char*)&msg) + sizeof(msgType), sizeof(msg) - sizeof(msgType));
                emit logMessage(QString::fromUtf8(msg.message), msg.isError != 0);
            }
            else if (msgType == IPC_RESULT) {
                IpcResult msg;
                msg.type = msgType;
                read(g_pipeToGui[0], ((char*)&msg) + sizeof(msgType), sizeof(msg) - sizeof(msgType));

                if (msg.result == FLASHMD_OK) {
                    emit operationFinished(true, QString());
                } else {
                    emit operationFinished(false, QString(flashmd_error_string((flashmd_result_t)msg.result)));
                }
                return;
            }
        }
    }
#endif

private:
    static void progressCallback(uint32_t current, uint32_t total, void *userData) {
        UsbWorker *worker = static_cast<UsbWorker*>(userData);
        emit worker->progressChanged(current, total);
    }

    static void messageCallback(const char *msg, int isError, void *userData) {
        UsbWorker *worker = static_cast<UsbWorker*>(userData);
        emit worker->logMessage(QString::fromUtf8(msg), isError != 0);
    }

    Operation m_operation = OP_NONE;
    QString m_filepath;
    uint32_t m_sizeKb = 0;
    bool m_noTrim = false;
    bool m_verbose = false;
    bool m_fullErase = false;
};

/*
 * Main Window
 */
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr) : QMainWindow(parent) {
        setWindowTitle("flashmd-thingy");
        setMinimumSize(500, 600);

        setupUi();
        setupWorker();

        log("flashmd-thingy");
    }

private slots:
    void onConnect() {
        if (m_worker->isRunning()) return;
        log("");
        m_worker->setOperation(UsbWorker::OP_CONNECT);
        startOperation();
    }

    void onCheckId() {
        if (m_worker->isRunning()) return;
        log("");
        m_worker->setOperation(UsbWorker::OP_CHECK_ID);
        startOperation();
    }

    void onWriteRom() {
        if (m_worker->isRunning()) return;

        QString savedPath = getSavedPath("writeRomPath");
        QString defaultPath;
        if (!savedPath.isEmpty()) {
            QFileInfo info(savedPath);
            defaultPath = info.absolutePath();
        }

        QString filepath = QFileDialog::getOpenFileName(this, "Open ROM File", defaultPath,
            "ROM Files (*.bin *.md *.gen *.smd);;All Files (*)");
        if (filepath.isEmpty()) return;

        // Save the selected path
        savePath("writeRomPath", filepath);

        if (QMessageBox::question(this, "Confirm Write",
            "Are you sure you want to write this ROM?") != QMessageBox::Yes) return;

        log("");
        m_worker->setOperation(UsbWorker::OP_WRITE_ROM, filepath,
                               SIZE_VALUES[m_sizeCombo->currentIndex()],
                               m_noTrimCheck->isChecked(),
                               m_verboseCheck->isChecked());
        startOperation();
    }

    void onReadRom() {
        if (m_worker->isRunning()) return;

        QString savedPath = getSavedPath("readRomPath", "dump.bin");
        QString defaultPath;
        if (!savedPath.isEmpty()) {
            QFileInfo info(savedPath);
            defaultPath = info.absolutePath() + "/" + info.fileName();
        } else {
            defaultPath = "dump.bin";
        }

        QString filepath = QFileDialog::getSaveFileName(this, "Save ROM File", defaultPath,
            "ROM Files (*.bin *.md *.gen *.smd);;All Files (*)");
        if (filepath.isEmpty()) return;

        // Save the selected path
        savePath("readRomPath", filepath);

        if (QMessageBox::question(this, "Confirm Read",
            "Are you sure you want to read the ROM to this file?") != QMessageBox::Yes) return;

        log("");
        m_worker->setOperation(UsbWorker::OP_READ_ROM, filepath,
                               SIZE_VALUES[m_sizeCombo->currentIndex()],
                               m_noTrimCheck->isChecked(),
                               m_verboseCheck->isChecked());
        startOperation();
    }

    void onErase() {
        if (m_worker->isRunning()) return;

        if (QMessageBox::question(this, "Confirm Erase",
            "Are you sure you want to erase the flash memory?") != QMessageBox::Yes) return;

        log("");
        m_worker->setOperation(UsbWorker::OP_ERASE, QString(),
                               SIZE_VALUES[m_sizeCombo->currentIndex()],
                               false, m_verboseCheck->isChecked(),
                               m_fullEraseCheck->isChecked());
        startOperation();
    }

    void onReadSram() {
        if (m_worker->isRunning()) return;

        QString savedPath = getSavedPath("readSramPath", "save.srm");
        QString defaultPath;
        if (!savedPath.isEmpty()) {
            QFileInfo info(savedPath);
            defaultPath = info.absolutePath() + "/" + info.fileName();
        } else {
            defaultPath = "save.srm";
        }

        QString filepath = QFileDialog::getSaveFileName(this, "Save SRAM File", defaultPath,
            "SRAM Files (*.srm *.sav *.bin);;All Files (*)");
        if (filepath.isEmpty()) return;

        // Save the selected path
        savePath("readSramPath", filepath);

        if (QMessageBox::question(this, "Confirm Read",
            "Are you sure you want to read SRAM?") != QMessageBox::Yes) return;

        log("");
        m_worker->setOperation(UsbWorker::OP_READ_SRAM, filepath);
        startOperation();
    }

    void onWriteSram() {
        if (m_worker->isRunning()) return;

        QString savedPath = getSavedPath("writeSramPath");
        QString defaultPath;
        if (!savedPath.isEmpty()) {
            QFileInfo info(savedPath);
            defaultPath = info.absolutePath();
        }

        QString filepath = QFileDialog::getOpenFileName(this, "Open SRAM File", defaultPath,
            "SRAM Files (*.srm *.sav *.bin);;All Files (*)");
        if (filepath.isEmpty()) return;

        // Save the selected path
        savePath("writeSramPath", filepath);

        if (QMessageBox::question(this, "Confirm Write",
            "Are you sure you want to write SRAM?") != QMessageBox::Yes) return;

        log("");
        m_worker->setOperation(UsbWorker::OP_WRITE_SRAM, filepath);
        startOperation();
    }

    void onClearLog() {
        m_console->clear();
    }

    void onProgressChanged(int current, int total) {
        m_progressBar->setMaximum(total);
        m_progressBar->setValue(current);
        m_progressLabel->setText(QString("%1 / %2 KB").arg(current / 1024).arg(total / 1024));
    }

    void onLogMessage(const QString &message, bool isError) {
        if (isError) {
            m_console->append("<span style='color: #ff6666;'>" + message.toHtmlEscaped() + "</span>");
        } else {
            m_console->append(message.toHtmlEscaped());
        }
    }

    void onOperationFinished(bool success, const QString &errorMsg) {
        setUiEnabled(true);

        if (success) {
            m_deviceStatus->setText("Connected");
            m_deviceStatus->setStyleSheet("color: green; font-weight: bold;");
        } else if (!errorMsg.isEmpty()) {
            log("Error: " + errorMsg);
        }
    }

private:
    void setupUi() {
        QWidget *central = new QWidget(this);
        setCentralWidget(central);

        QVBoxLayout *mainLayout = new QVBoxLayout(central);
        mainLayout->setSpacing(10);

        /* Title */
        QLabel *title = new QLabel("flashmd-thingy");
        title->setStyleSheet("font-size: 20px; font-weight: bold;");
        QLabel *subtitle = new QLabel("Sega Genesis ROM Flasher");
        subtitle->setStyleSheet("color: gray;");

        QHBoxLayout *titleLayout = new QHBoxLayout();
        titleLayout->addWidget(title);
        titleLayout->addWidget(subtitle);
        titleLayout->addStretch();
        mainLayout->addLayout(titleLayout);

        /* Device section */
        QGroupBox *deviceGroup = new QGroupBox("Device");
        QHBoxLayout *deviceLayout = new QHBoxLayout(deviceGroup);

        deviceLayout->addWidget(new QLabel("Status:"));
        m_deviceStatus = new QLabel("Not Connected");
        m_deviceStatus->setStyleSheet("color: orange; font-weight: bold;");
        deviceLayout->addWidget(m_deviceStatus);
        deviceLayout->addStretch();

        QPushButton *connectBtn = new QPushButton("Connect");
        QPushButton *checkIdBtn = new QPushButton("Check ID");
        connect(connectBtn, &QPushButton::clicked, this, &MainWindow::onConnect);
        connect(checkIdBtn, &QPushButton::clicked, this, &MainWindow::onCheckId);
        deviceLayout->addWidget(connectBtn);
        deviceLayout->addWidget(checkIdBtn);

        mainLayout->addWidget(deviceGroup);

        /* ROM Operations section */
        QGroupBox *romGroup = new QGroupBox("ROM Operations");
        QGridLayout *romLayout = new QGridLayout(romGroup);

        romLayout->addWidget(new QLabel("Size:"), 0, 0);
        m_sizeCombo = new QComboBox();
        for (int i = 0; i < 7; i++) {
            m_sizeCombo->addItem(SIZE_LABELS[i]);
        }
        romLayout->addWidget(m_sizeCombo, 0, 1);

        m_noTrimCheck = new QCheckBox("No trim");
        romLayout->addWidget(m_noTrimCheck, 0, 2);

        QPushButton *writeRomBtn = new QPushButton("Write ROM");
        QPushButton *readRomBtn = new QPushButton("Read ROM");
        QPushButton *eraseBtn = new QPushButton("Erase");
        m_fullEraseCheck = new QCheckBox("Full Erase");

        connect(writeRomBtn, &QPushButton::clicked, this, &MainWindow::onWriteRom);
        connect(readRomBtn, &QPushButton::clicked, this, &MainWindow::onReadRom);
        connect(eraseBtn, &QPushButton::clicked, this, &MainWindow::onErase);

        romLayout->addWidget(writeRomBtn, 1, 0);
        romLayout->addWidget(readRomBtn, 1, 1);
        romLayout->addWidget(eraseBtn, 1, 2);
        romLayout->addWidget(m_fullEraseCheck, 1, 3);

        mainLayout->addWidget(romGroup);

        /* SRAM Operations section */
        QGroupBox *sramGroup = new QGroupBox("SRAM Operations");
        QHBoxLayout *sramLayout = new QHBoxLayout(sramGroup);

        QPushButton *readSramBtn = new QPushButton("Read SRAM");
        QPushButton *writeSramBtn = new QPushButton("Write SRAM");
        connect(readSramBtn, &QPushButton::clicked, this, &MainWindow::onReadSram);
        connect(writeSramBtn, &QPushButton::clicked, this, &MainWindow::onWriteSram);

        sramLayout->addWidget(readSramBtn);
        sramLayout->addWidget(writeSramBtn);
        sramLayout->addStretch();

        mainLayout->addWidget(sramGroup);

        /* Progress section */
        QHBoxLayout *progressLayout = new QHBoxLayout();
        progressLayout->addWidget(new QLabel("Progress:"));
        m_progressBar = new QProgressBar();
        m_progressBar->setMinimum(0);
        m_progressBar->setValue(0);
        progressLayout->addWidget(m_progressBar, 1);
        m_progressLabel = new QLabel("0 / 0 KB");
        progressLayout->addWidget(m_progressLabel);
        mainLayout->addLayout(progressLayout);

        /* Console section */
        QLabel *consoleLabel = new QLabel("Console Output:");
        mainLayout->addWidget(consoleLabel);

        m_console = new QTextEdit();
        m_console->setReadOnly(true);
        m_console->setStyleSheet("font-family: monospace; background-color: #1e1e1e; color: #d4d4d4;");
        mainLayout->addWidget(m_console, 1);

        /* Bottom buttons */
        QHBoxLayout *bottomLayout = new QHBoxLayout();
        QPushButton *clearBtn = new QPushButton("Clear");
        connect(clearBtn, &QPushButton::clicked, this, &MainWindow::onClearLog);
        bottomLayout->addWidget(clearBtn);
        bottomLayout->addStretch();

        m_verboseCheck = new QCheckBox("Verbose");
        bottomLayout->addWidget(m_verboseCheck);

        mainLayout->addLayout(bottomLayout);
    }

    void setupWorker() {
        m_worker = new UsbWorker(this);
        connect(m_worker, &UsbWorker::progressChanged, this, &MainWindow::onProgressChanged);
        connect(m_worker, &UsbWorker::logMessage, this, &MainWindow::onLogMessage);
        connect(m_worker, &UsbWorker::operationFinished, this, &MainWindow::onOperationFinished);
    }

    void startOperation() {
        m_progressBar->setValue(0);
        m_progressLabel->setText("Starting...");
        setUiEnabled(false);
        m_worker->start();
    }

    void setUiEnabled(bool enabled) {
        centralWidget()->setEnabled(enabled);
    }

    void log(const QString &message) {
        m_console->append(message);
    }

    UsbWorker *m_worker;
    QLabel *m_deviceStatus;
    QComboBox *m_sizeCombo;
    QCheckBox *m_noTrimCheck;
    QCheckBox *m_fullEraseCheck;
    QCheckBox *m_verboseCheck;
    QProgressBar *m_progressBar;
    QLabel *m_progressLabel;
    QTextEdit *m_console;
};

int main(int argc, char *argv[]) {
    /* Set up file ownership for created files */
    const char *sudoUid = getenv("SUDO_UID");
    const char *sudoGid = getenv("SUDO_GID");
    uid_t realUid = sudoUid ? (uid_t)atoi(sudoUid) : getuid();
    gid_t realGid = sudoGid ? (gid_t)atoi(sudoGid) : getgid();

#ifdef __linux__
    /* Privilege separation: if running as root via sudo, fork */
    if (getuid() == 0 && sudoUid && sudoGid) {
        if (pipe(g_pipeToUsb) < 0 || pipe(g_pipeToGui) < 0) {
            fprintf(stderr, "Failed to create pipes: %s\n", strerror(errno));
            return 1;
        }

        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "Failed to fork: %s\n", strerror(errno));
            return 1;
        }

        if (pid > 0) {
            /* Parent - stays root, handles USB */
            close(g_pipeToUsb[1]);
            close(g_pipeToGui[0]);
            flashmd_set_real_ids(realUid, realGid);
            usbHandlerLoop(g_pipeToUsb[0], g_pipeToGui[1]);
            close(g_pipeToUsb[0]);
            close(g_pipeToGui[1]);
            waitpid(pid, NULL, 0);
            return 0;
        }

        /* Child - drops privileges, runs GUI */
        close(g_pipeToUsb[0]);
        close(g_pipeToGui[1]);

        if (setgid(realGid) < 0 || setuid(realUid) < 0) {
            fprintf(stderr, "Failed to drop privileges: %s\n", strerror(errno));
            _exit(1);
        }

        g_usingIpc = true;
    } else {
        flashmd_set_real_ids(realUid, realGid);
    }
#else
    flashmd_set_real_ids(realUid, realGid);
#endif

    QApplication app(argc, argv);
    app.setStyle(QStyleFactory::create("Fusion"));

    MainWindow window;
    window.show();

    int result = app.exec();

#ifdef __linux__
    /* Send quit to USB handler */
    if (g_usingIpc) {
        IpcCommand quit = {};
        quit.type = IPC_QUIT;
        write(g_pipeToUsb[1], &quit, sizeof(quit));
        close(g_pipeToUsb[1]);
        close(g_pipeToGui[0]);
    }
#endif

    return result;
}

/* MOC generated file - must be at end */
#include "moc_flashmd_qt.cpp"
