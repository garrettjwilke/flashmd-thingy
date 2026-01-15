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
#include <pwd.h>
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
static QString getRealUserHome() {
    // Get the real user's home directory (not root's if running via sudo)
    const char *homeDir = getenv("HOME");
    
    // If HOME is set to /root, we're probably running as root via sudo
    // In that case, use SUDO_UID to get the real user's home
    if (homeDir && strcmp(homeDir, "/root") == 0) {
        homeDir = nullptr; // Force lookup via SUDO_UID
    }
    
    if (!homeDir) {
        // Get home from passwd using real UID
        const char *sudoUid = getenv("SUDO_UID");
        if (sudoUid) {
            struct passwd *pw = getpwuid((uid_t)atoi(sudoUid));
            if (pw && pw->pw_dir) {
                homeDir = pw->pw_dir;
            }
        }
        if (!homeDir) {
            // Last resort: use current user
            struct passwd *pw = getpwuid(getuid());
            if (pw && pw->pw_dir) {
                homeDir = pw->pw_dir;
            }
        }
    }
    
    return homeDir ? QString::fromUtf8(homeDir) : QString();
}

static QString getConfigPath() {
    QString homeDir = getRealUserHome();
    
    QString configDir;
    if (!homeDir.isEmpty()) {
        configDir = homeDir + "/.config/flashmd";
    } else {
        // Fallback to QStandardPaths if we can't determine home
        configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
        configDir += "/flashmd";
    }
    
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

static QString getTheme() {
    QSettings settings(getConfigPath(), QSettings::IniFormat);
    return settings.value("theme", "dark").toString();
}

static void saveTheme(const QString &theme) {
    QSettings settings(getConfigPath(), QSettings::IniFormat);
    settings.setValue("theme", theme);
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
    ssize_t _unused = write(g_ipcWriteFd, &msg, sizeof(msg));
    (void)_unused;
}

static void ipcMessageCb(const char *text, int isError, void *) {
    if (g_ipcWriteFd < 0) return;
    IpcLog msg = {IPC_LOG, isError, {}};
    strncpy(msg.message, text, sizeof(msg.message) - 1);
    ssize_t _unused = write(g_ipcWriteFd, &msg, sizeof(msg));
    (void)_unused;
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
            ssize_t _unused1 = write(writeFd, &logMsg, sizeof(logMsg));
            (void)_unused1;
            IpcResult resMsg = {IPC_RESULT, (int)result};
            ssize_t _unused2 = write(writeFd, &resMsg, sizeof(resMsg));
            (void)_unused2;
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
        ssize_t _unused = write(writeFd, &resMsg, sizeof(resMsg));
        (void)_unused;
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

        ssize_t _unused_write = write(g_pipeToUsb[1], &cmd, sizeof(cmd));
        (void)_unused_write;

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
                ssize_t _unused = read(g_pipeToGui[0], ((char*)&msg) + sizeof(msgType), sizeof(msg) - sizeof(msgType));
                (void)_unused;
                emit progressChanged(msg.current, msg.total);
            }
            else if (msgType == IPC_LOG) {
                IpcLog msg;
                msg.type = msgType;
                ssize_t _unused = read(g_pipeToGui[0], ((char*)&msg) + sizeof(msgType), sizeof(msg) - sizeof(msgType));
                (void)_unused;
                emit logMessage(QString::fromUtf8(msg.message), msg.isError != 0);
            }
            else if (msgType == IPC_RESULT) {
                IpcResult msg;
                msg.type = msgType;
                ssize_t _unused = read(g_pipeToGui[0], ((char*)&msg) + sizeof(msgType), sizeof(msg) - sizeof(msgType));
                (void)_unused;

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
        m_currentTheme = getTheme();

        setupUi();
        setupWorker();
        applyTheme(m_currentTheme);
        
        setMinimumSize(550, 700);
        resize(550, 700);

        log("flashmd-thingy");
    }

private slots:
    void onWriteRom() {
        if (m_worker->isRunning()) return;

        QString savedPath = getSavedPath("writeRomPath");
        QString defaultPath;
        if (!savedPath.isEmpty()) {
            QFileInfo info(savedPath);
            defaultPath = info.absolutePath();
        } else {
            defaultPath = getRealUserHome();
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
            QString homeDir = getRealUserHome();
            defaultPath = homeDir.isEmpty() ? "dump.bin" : (homeDir + "/dump.bin");
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
            QString homeDir = getRealUserHome();
            defaultPath = homeDir.isEmpty() ? "save.srm" : (homeDir + "/save.srm");
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
        } else {
            defaultPath = getRealUserHome();
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

    void onThemeChanged() {
        m_currentTheme = (m_currentTheme == "dark") ? "light" : "dark";
        saveTheme(m_currentTheme);
        if (m_themeBtn) {
            m_themeBtn->setText(m_currentTheme == "dark" ? "☀" : "☾");
        }
        applyTheme(m_currentTheme);
    }

    void onProgressChanged(int current, int total) {
        m_progressBar->setMaximum(total);
        m_progressBar->setValue(current);
        m_progressLabel->setText(QString("%1 / %2 KB").arg(current / 1024).arg(total / 1024));
    }

    void onLogMessage(const QString &message, bool isError) {
        QString errorColor = (m_currentTheme == "light") ? "#ff3b30" : "#ff453a";
        if (isError) {
            m_console->append("<span style='color: " + errorColor + ";'>" + message.toHtmlEscaped() + "</span>");
        } else {
            m_console->append(message.toHtmlEscaped());
        }
    }

    void onOperationFinished(bool success, const QString &errorMsg) {
        setUiEnabled(true);

        if (!success && !errorMsg.isEmpty()) {
            log("Error: " + errorMsg);
        }
    }

private:
    void setupUi() {
        QWidget *central = new QWidget(this);
        setCentralWidget(central);

        QVBoxLayout *mainLayout = new QVBoxLayout(central);
        mainLayout->setSpacing(16);
        mainLayout->setContentsMargins(20, 20, 20, 20);

        /* Title section with theme toggle */
        QHBoxLayout *titleLayout = new QHBoxLayout();
        QVBoxLayout *titleTextLayout = new QVBoxLayout();
        m_titleLabel = new QLabel("flashmd-thingy");
        m_subtitleLabel = new QLabel("Sega Genesis ROM Flasher");
        titleTextLayout->addWidget(m_titleLabel);
        titleTextLayout->addWidget(m_subtitleLabel);
        titleTextLayout->setSpacing(4);
        titleLayout->addLayout(titleTextLayout);
        titleLayout->addStretch();
        
        // Theme toggle button
        m_themeBtn = new QPushButton();
        // Use Unicode symbols that work better across platforms
        m_themeBtn->setText(m_currentTheme == "dark" ? "☀" : "☾");
        m_themeBtn->setToolTip("Toggle theme");
        m_themeBtn->setFixedSize(40, 40);
        m_themeBtn->setStyleSheet(R"(
            QPushButton {
                background-color: transparent;
                border: 2px solid;
                border-radius: 8px;
                font-size: 20px;
                padding: 0;
            }
            QPushButton:hover {
                background-color: rgba(128, 128, 128, 0.2);
            }
        )");
        connect(m_themeBtn, &QPushButton::clicked, this, &MainWindow::onThemeChanged);
        titleLayout->addWidget(m_themeBtn);
        
        mainLayout->addLayout(titleLayout);

        /* ROM Operations section */
        QGroupBox *romGroup = new QGroupBox("ROM Operations");
        QVBoxLayout *romMainLayout = new QVBoxLayout(romGroup);
        romMainLayout->setSpacing(12);
        romMainLayout->setContentsMargins(16, 20, 16, 16);

        /* Size selector in its own horizontal layout */
        QHBoxLayout *sizeLayout = new QHBoxLayout();
        sizeLayout->setSpacing(8);
        sizeLayout->addWidget(new QLabel("Size:"));
        m_sizeCombo = new QComboBox();
        m_sizeCombo->setFixedWidth(400);
        for (int i = 0; i < 7; i++) {
            m_sizeCombo->addItem(SIZE_LABELS[i]);
        }
        sizeLayout->addWidget(m_sizeCombo);
        sizeLayout->addStretch();
        romMainLayout->addLayout(sizeLayout);

        /* Buttons in a grid layout */
        QGridLayout *romLayout = new QGridLayout();
        romLayout->setSpacing(12);

        m_writeRomBtn = new QPushButton("Write ROM");
        m_readRomBtn = new QPushButton("Read ROM");
        m_noTrimCheck = new QCheckBox("No trim");
        m_eraseBtn = new QPushButton("Erase");
        m_fullEraseCheck = new QCheckBox("Full Erase");

        connect(m_writeRomBtn, &QPushButton::clicked, this, &MainWindow::onWriteRom);
        connect(m_readRomBtn, &QPushButton::clicked, this, &MainWindow::onReadRom);
        connect(m_eraseBtn, &QPushButton::clicked, this, &MainWindow::onErase);

        romLayout->addWidget(m_writeRomBtn, 0, 0);
        romLayout->addWidget(m_readRomBtn, 0, 1);
        romLayout->addWidget(m_noTrimCheck, 0, 2);
        romLayout->addWidget(m_eraseBtn, 1, 0);
        romLayout->addWidget(m_fullEraseCheck, 1, 1);
        romMainLayout->addLayout(romLayout);

        mainLayout->addWidget(romGroup);

        /* SRAM Operations section */
        QGroupBox *sramGroup = new QGroupBox("SRAM Operations");
        QHBoxLayout *sramLayout = new QHBoxLayout(sramGroup);
        sramLayout->setSpacing(12);
        sramLayout->setContentsMargins(16, 20, 16, 16);

        m_writeSramBtn = new QPushButton("Write SRAM");
        m_readSramBtn = new QPushButton("Read SRAM");
        connect(m_writeSramBtn, &QPushButton::clicked, this, &MainWindow::onWriteSram);
        connect(m_readSramBtn, &QPushButton::clicked, this, &MainWindow::onReadSram);

        sramLayout->addWidget(m_writeSramBtn);
        sramLayout->addWidget(m_readSramBtn);
        sramLayout->addStretch();

        mainLayout->addWidget(sramGroup);

        /* Progress section */
        QHBoxLayout *progressLayout = new QHBoxLayout();
        progressLayout->setSpacing(12);
        progressLayout->addWidget(new QLabel("Progress:"));
        m_progressBar = new QProgressBar();
        m_progressBar->setMinimum(0);
        m_progressBar->setValue(0);
        progressLayout->addWidget(m_progressBar, 1);
        m_progressLabel = new QLabel("0 / 0 KB");
        m_progressLabel->setStyleSheet("color: #86868b; font-size: 12px; font-weight: 500;");
        progressLayout->addWidget(m_progressLabel);
        mainLayout->addLayout(progressLayout);

        /* Add stretch to push console to bottom */
        mainLayout->addStretch(1);

        /* Console section */
        QGroupBox *consoleGroup = new QGroupBox("Console Output");
        consoleGroup->setFixedHeight(160);
        QVBoxLayout *consoleLayout = new QVBoxLayout(consoleGroup);
        consoleLayout->setSpacing(0);
        consoleLayout->setContentsMargins(16, 0, 16, 12);

        m_console = new QTextEdit();
        m_console->setReadOnly(true);
        m_console->setFixedHeight(108);
        consoleLayout->addWidget(m_console);
        
        mainLayout->addWidget(consoleGroup, 0);

        /* Bottom buttons */
        QHBoxLayout *bottomLayout = new QHBoxLayout();
        m_clearBtn = new QPushButton("Clear");
        connect(m_clearBtn, &QPushButton::clicked, this, &MainWindow::onClearLog);
        bottomLayout->addWidget(m_clearBtn);
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
        if (enabled) {
            // Restore the original theme
            applyTheme(m_currentTheme);
        } else {
            // Apply gray stylesheet for all UI elements
            applyGrayStylesheet();
        }
    }

    void applyGrayStylesheet() {
        // Determine base colors based on current theme
        bool isLight = (m_currentTheme == "light");
        QString bgColor = isLight ? "#f5f5f7" : "#1c1c1e";
        QString groupBg = isLight ? "#ffffff" : "#2c2c2e";
        QString grayColor = "#808080";
        QString grayDark = "#666666";
        QString grayLight = "#999999";
        QString textColor = isLight ? "#1d1d1f" : "#f5f5f7";
        QString grayText = grayColor;
        
        QString grayStyleSheet = QString(R"(
            QMainWindow {
                background-color: %1;
            }
            QWidget {
                background-color: %1;
                color: %2;
                font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
            }
            QGroupBox {
                font-weight: 600;
                font-size: 13px;
                color: %3;
                border: 2px solid %4;
                border-radius: 12px;
                margin-top: 12px;
                padding-top: 12px;
                background-color: %5;
            }
            QGroupBox::title {
                subcontrol-origin: margin;
                left: 16px;
                padding: 0 8px;
                background-color: %5;
                color: %3;
            }
            QPushButton {
                background-color: %4 !important;
                color: white !important;
                border: none !important;
                border-radius: 8px;
                padding: 10px 20px;
                font-weight: 600;
                font-size: 13px;
                min-height: 20px;
            }
            QPushButton:hover {
                background-color: %4 !important;
            }
            QPushButton:pressed {
                background-color: %4 !important;
            }
            QPushButton:disabled {
                background-color: %4 !important;
                color: white !important;
            }
            QComboBox {
                background-color: %4 !important;
                border: 2px solid %4 !important;
                border-radius: 8px;
                padding: 8px 12px;
                min-height: 20px;
                font-size: 13px;
                color: white !important;
            }
            QComboBox:hover {
                border-color: %4 !important;
            }
            QComboBox::drop-down {
                border: none;
                width: 30px;
            }
            QComboBox::down-arrow {
                image: none;
                border-left: 5px solid transparent;
                border-right: 5px solid transparent;
                border-top: 6px solid white;
                width: 0;
                height: 0;
            }
            QComboBox QAbstractItemView {
                background-color: %4 !important;
                border: 2px solid %4 !important;
                border-radius: 8px;
                selection-background-color: %4 !important;
                selection-color: white !important;
                color: white !important;
            }
            QCheckBox {
                font-size: 13px;
                spacing: 8px;
                color: %3 !important;
            }
            QCheckBox::indicator {
                width: 20px;
                height: 20px;
                border: 2px solid %4 !important;
                border-radius: 4px;
                background-color: %4 !important;
            }
            QCheckBox::indicator:hover {
                border-color: %4 !important;
            }
            QCheckBox::indicator:checked {
                background-color: %4 !important;
                border-color: %4 !important;
            }
            QProgressBar {
                border: none;
                border-radius: 8px;
                background-color: %6;
                height: 8px;
                text-align: center;
            }
            QProgressBar::chunk {
                background-color: %4;
                border-radius: 8px;
            }
            QTextEdit {
                background-color: %7;
                color: %2;
                border: none;
                border-radius: 8px;
                padding: 8px;
                font-family: "SF Mono", "Monaco", "Cascadia Code", "Roboto Mono", monospace;
                font-size: 12px;
            }
            QLabel {
                font-size: 13px;
                color: %3 !important;
            }
            QGroupBox QLabel {
                background-color: transparent;
                color: %3 !important;
            }
            QGroupBox QCheckBox {
                background-color: transparent;
                color: %3 !important;
            }
        )").arg(bgColor).arg(textColor).arg(grayText).arg(grayColor)
          .arg(groupBg).arg(isLight ? "#e5e5e7" : "#38383a")
          .arg(isLight ? "#ffffff" : "#000000");
        
        qApp->setStyleSheet(grayStyleSheet);
        
        // Update title and subtitle to gray
        if (m_titleLabel) {
            m_titleLabel->setStyleSheet(QString("font-size: 28px; font-weight: 700; color: %1;")
                .arg(grayText));
        }
        if (m_subtitleLabel) {
            m_subtitleLabel->setStyleSheet(QString("color: %1; font-size: 14px;")
                .arg(grayText));
        }
        
        // Update progress label to gray
        if (m_progressLabel) {
            m_progressLabel->setStyleSheet(QString("color: %1; font-size: 12px; font-weight: 500;")
                .arg(grayText));
        }
        
        // Update theme button to gray
        if (m_themeBtn) {
            m_themeBtn->setStyleSheet(QString(R"(
                QPushButton {
                    background-color: transparent;
                    border: 2px solid %1;
                    border-radius: 8px;
                    font-size: 20px;
                    padding: 0;
                    color: %1;
                }
                QPushButton:hover {
                    background-color: rgba(128, 128, 128, 0.2);
                }
            )").arg(grayColor));
        }
        
        // Explicitly set ALL buttons to gray (they have individual stylesheets that override global)
        QString grayButtonStyle = QString(R"(
            QPushButton {
                background-color: %1 !important;
                color: white !important;
                border: none !important;
                border-radius: 8px;
                padding: 10px 20px;
                font-weight: 600;
                font-size: 13px;
                min-height: 20px;
            }
            QPushButton:hover {
                background-color: %1 !important;
            }
            QPushButton:pressed {
                background-color: %1 !important;
            }
            QPushButton:disabled {
                background-color: %1 !important;
                color: white !important;
            }
        )").arg(grayColor);
        
        if (m_writeRomBtn) {
            m_writeRomBtn->setStyleSheet(grayButtonStyle);
        }
        if (m_readRomBtn) {
            m_readRomBtn->setStyleSheet(grayButtonStyle);
        }
        if (m_eraseBtn) {
            m_eraseBtn->setStyleSheet(grayButtonStyle);
        }
        if (m_writeSramBtn) {
            m_writeSramBtn->setStyleSheet(grayButtonStyle);
        }
        if (m_readSramBtn) {
            m_readSramBtn->setStyleSheet(grayButtonStyle);
        }
        if (m_clearBtn) {
            m_clearBtn->setStyleSheet(grayButtonStyle);
        }
    }

    void log(const QString &message) {
        m_console->append(message);
    }

    void applyTheme(const QString &theme) {
        QString styleSheet;
        
        if (theme == "light") {
            styleSheet = R"(
                QMainWindow {
                    background-color: #f5f5f7;
                }
                QWidget {
                    background-color: #f5f5f7;
                    color: #1d1d1f;
                    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
                }
                QGroupBox {
                    font-weight: 600;
                    font-size: 13px;
                    color: #1d1d1f;
                    border: 2px solid #e5e5e7;
                    border-radius: 12px;
                    margin-top: 12px;
                    padding-top: 12px;
                    background-color: #ffffff;
                }
                QGroupBox::title {
                    subcontrol-origin: margin;
                    left: 16px;
                    padding: 0 8px;
                    background-color: #ffffff;
                }
                QPushButton {
                    background-color: #007aff;
                    color: white;
                    border: none;
                    border-radius: 8px;
                    padding: 10px 20px;
                    font-weight: 600;
                    font-size: 13px;
                    min-height: 20px;
                }
                QPushButton:hover {
                    background-color: #0051d5;
                }
                QPushButton:pressed {
                    background-color: #0040a8;
                }
                QPushButton:disabled {
                    background-color: #c7c7cc;
                    color: #8e8e93;
                }
                QComboBox {
                    background-color: #e5e5e7;
                    border: 2px solid #d1d1d6;
                    border-radius: 8px;
                    padding: 8px 12px;
                    min-height: 20px;
                    font-size: 13px;
                }
                QComboBox:hover {
                    border-color: #007aff;
                }
                QComboBox::drop-down {
                    border: none;
                    width: 30px;
                }
                QComboBox::down-arrow {
                    image: none;
                    border-left: 5px solid transparent;
                    border-right: 5px solid transparent;
                    border-top: 6px solid #1d1d1f;
                    width: 0;
                    height: 0;
                }
                QComboBox QAbstractItemView {
                    background-color: #e5e5e7;
                    border: 2px solid #d1d1d6;
                    border-radius: 8px;
                    selection-background-color: #007aff;
                    selection-color: white;
                }
                QCheckBox {
                    font-size: 13px;
                    spacing: 8px;
                }
                QCheckBox::indicator {
                    width: 20px;
                    height: 20px;
                    border: 2px solid #c7c7cc;
                    border-radius: 4px;
                    background-color: #ffffff;
                }
                QCheckBox::indicator:hover {
                    border-color: #007aff;
                }
                QCheckBox::indicator:checked {
                    background-color: #007aff;
                    border-color: #007aff;
                    image: url(data:image/svg+xml;base64,PHN2ZyB3aWR0aD0iMTIiIGhlaWdodD0iOSIgdmlld0JveD0iMCAwIDEyIDkiIGZpbGw9Im5vbmUiIHhtbG5zPSJodHRwOi8vd3d3LnczLm9yZy8yMDAwL3N2ZyI+CjxwYXRoIGQ9Ik0xIDQuNUw0LjUgOEwxMSAxIiBzdHJva2U9IndoaXRlIiBzdHJva2Utd2lkdGg9IjIiIHN0cm9rZS1saW5lY2FwPSJyb3VuZCIgc3Ryb2tlLWxpbmVqb2luPSJyb3VuZCIvPgo8L3N2Zz4=);
                }
                QProgressBar {
                    border: none;
                    border-radius: 8px;
                    background-color: #e5e5e7;
                    height: 8px;
                    text-align: center;
                }
                QProgressBar::chunk {
                    background-color: #007aff;
                    border-radius: 8px;
                }
                QTextEdit {
                    background-color: #ffffff;
                    color: #1d1d1f;
                    border: none;
                    border-radius: 8px;
                    padding: 8px;
                    font-family: "SF Mono", "Monaco", "Cascadia Code", "Roboto Mono", monospace;
                    font-size: 12px;
                }
                QLabel {
                    font-size: 13px;
                }
                QGroupBox QLabel {
                    background-color: transparent;
                }
                QGroupBox QCheckBox {
                    background-color: transparent;
                }
            )";
        } else { // dark theme
            styleSheet = R"(
                QMainWindow {
                    background-color: #1c1c1e;
                }
                QWidget {
                    background-color: #1c1c1e;
                    color: #f5f5f7;
                    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
                }
                QGroupBox {
                    font-weight: 600;
                    font-size: 13px;
                    color: #f5f5f7;
                    border: 2px solid #38383a;
                    border-radius: 12px;
                    margin-top: 12px;
                    padding-top: 12px;
                    background-color: #2c2c2e;
                }
                QGroupBox::title {
                    subcontrol-origin: margin;
                    left: 16px;
                    padding: 0 8px;
                    background-color: #2c2c2e;
                }
                QPushButton {
                    background-color: #0a84ff;
                    color: white;
                    border: none;
                    border-radius: 8px;
                    padding: 10px 20px;
                    font-weight: 600;
                    font-size: 13px;
                    min-height: 20px;
                }
                QPushButton:hover {
                    background-color: #409cff;
                }
                QPushButton:pressed {
                    background-color: #0051d5;
                }
                QPushButton:disabled {
                    background-color: #3a3a3c;
                    color: #636366;
                }
                QComboBox {
                    background-color: #48484a;
                    border: 2px solid #545458;
                    border-radius: 8px;
                    padding: 8px 12px;
                    min-height: 20px;
                    font-size: 13px;
                    color: #f5f5f7;
                }
                QComboBox:hover {
                    border-color: #0a84ff;
                }
                QComboBox::drop-down {
                    border: none;
                    width: 30px;
                }
                QComboBox::down-arrow {
                    image: none;
                    border-left: 5px solid transparent;
                    border-right: 5px solid transparent;
                    border-top: 6px solid #f5f5f7;
                    width: 0;
                    height: 0;
                }
                QComboBox QAbstractItemView {
                    background-color: #48484a;
                    border: 2px solid #545458;
                    border-radius: 8px;
                    selection-background-color: #0a84ff;
                    selection-color: white;
                    color: #f5f5f7;
                }
                QCheckBox {
                    font-size: 13px;
                    spacing: 8px;
                    color: #f5f5f7;
                }
                QCheckBox::indicator {
                    width: 20px;
                    height: 20px;
                    border: 2px solid #636366;
                    border-radius: 4px;
                    background-color: #2c2c2e;
                }
                QCheckBox::indicator:hover {
                    border-color: #0a84ff;
                }
                QCheckBox::indicator:checked {
                    background-color: #0a84ff;
                    border-color: #0a84ff;
                    image: url(data:image/svg+xml;base64,PHN2ZyB3aWR0aD0iMTIiIGhlaWdodD0iOSIgdmlld0JveD0iMCAwIDEyIDkiIGZpbGw9Im5vbmUiIHhtbG5zPSJodHRwOi8vd3d3LnczLm9yZy8yMDAwL3N2ZyI+CjxwYXRoIGQ9Ik0xIDQuNUw0LjUgOEwxMSAxIiBzdHJva2U9IndoaXRlIiBzdHJva2Utd2lkdGg9IjIiIHN0cm9rZS1saW5lY2FwPSJyb3VuZCIgc3Ryb2tlLWxpbmVqb2luPSJyb3VuZCIvPgo8L3N2Zz4=);
                }
                QProgressBar {
                    border: none;
                    border-radius: 8px;
                    background-color: #38383a;
                    height: 8px;
                    text-align: center;
                }
                QProgressBar::chunk {
                    background-color: #0a84ff;
                    border-radius: 8px;
                }
                QTextEdit {
                    background-color: #000000;
                    color: #f5f5f7;
                    border: none;
                    border-radius: 8px;
                    padding: 8px;
                    font-family: "SF Mono", "Monaco", "Cascadia Code", "Roboto Mono", monospace;
                    font-size: 12px;
                }
                QLabel {
                    font-size: 13px;
                }
                QGroupBox QLabel {
                    background-color: transparent;
                }
                QGroupBox QCheckBox {
                    background-color: transparent;
                }
            )";
        }
        
        qApp->setStyleSheet(styleSheet);
        
        // Update title styling
        if (m_titleLabel) {
            m_titleLabel->setStyleSheet(QString("font-size: 28px; font-weight: 700; color: %1;")
                .arg(theme == "light" ? "#1d1d1f" : "#f5f5f7"));
        }
        if (m_subtitleLabel) {
            m_subtitleLabel->setStyleSheet(QString("color: %1; font-size: 14px;")
                .arg(theme == "light" ? "#86868b" : "#98989d"));
        }
        
        // Update progress label
        if (m_progressLabel) {
            m_progressLabel->setStyleSheet(QString("color: %1; font-size: 12px; font-weight: 500;")
                .arg(theme == "light" ? "#86868b" : "#98989d"));
        }
        
        // Update console error colors
        // This will be handled in onLogMessage
        
        // Update theme button border and text color
        if (m_themeBtn) {
            QString borderColor = (theme == "light") ? "#e5e5e7" : "#38383a";
            QString textColor = (theme == "light") ? "#1d1d1f" : "#f5f5f7";
            m_themeBtn->setStyleSheet(QString(R"(
                QPushButton {
                    background-color: transparent;
                    border: 2px solid %1;
                    border-radius: 8px;
                    font-size: 20px;
                    padding: 0;
                    color: %2;
                }
                QPushButton:hover {
                    background-color: rgba(128, 128, 128, 0.2);
                }
            )").arg(borderColor).arg(textColor));
        }
        
        // Apply custom button colors
        applyButtonColors(theme);
    }
    
    void applyButtonColors(const QString &theme) {
        // Washed out colors
        QString writeGreen = (theme == "light") ? "#a8d5ba" : "#4a7c5e";
        QString readBlue = (theme == "light") ? "#a8c5d5" : "#4a6c7c";
        QString eraseRed = (theme == "light") ? "#d5a8a8" : "#7c4a4a";
        QString clearGray = (theme == "light") ? "#c7c7cc" : "#636366";
        QString buttonText = (theme == "light") ? "#1d1d1f" : "#f5f5f7";
        
        QString baseStyle = QString(R"(
            QPushButton {
                background-color: %1;
                color: %2;
                border: none;
                border-radius: 8px;
                padding: 10px 20px;
                font-weight: 600;
                font-size: 13px;
                min-height: 20px;
            }
            QPushButton:hover {
                opacity: 0.8;
            }
            QPushButton:pressed {
                opacity: 0.6;
            }
            QPushButton:disabled {
                opacity: 0.4;
            }
        )");
        
        if (m_writeRomBtn) {
            m_writeRomBtn->setStyleSheet(baseStyle.arg(writeGreen).arg(buttonText));
        }
        if (m_readRomBtn) {
            m_readRomBtn->setStyleSheet(baseStyle.arg(readBlue).arg(buttonText));
        }
        if (m_eraseBtn) {
            m_eraseBtn->setStyleSheet(baseStyle.arg(eraseRed).arg(buttonText));
        }
        if (m_writeSramBtn) {
            m_writeSramBtn->setStyleSheet(baseStyle.arg(writeGreen).arg(buttonText));
        }
        if (m_readSramBtn) {
            m_readSramBtn->setStyleSheet(baseStyle.arg(readBlue).arg(buttonText));
        }
        if (m_clearBtn) {
            m_clearBtn->setStyleSheet(baseStyle.arg(clearGray).arg(buttonText));
        }
    }

    UsbWorker *m_worker;
    QLabel *m_titleLabel;
    QLabel *m_subtitleLabel;
    QPushButton *m_themeBtn;
    QPushButton *m_writeRomBtn;
    QPushButton *m_readRomBtn;
    QPushButton *m_eraseBtn;
    QPushButton *m_writeSramBtn;
    QPushButton *m_readSramBtn;
    QPushButton *m_clearBtn;
    QComboBox *m_sizeCombo;
    QCheckBox *m_noTrimCheck;
    QCheckBox *m_fullEraseCheck;
    QCheckBox *m_verboseCheck;
    QProgressBar *m_progressBar;
    QLabel *m_progressLabel;
    QTextEdit *m_console;
    QString m_currentTheme;
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

        /* Update environment variables to match the real user */
        struct passwd *pw = getpwuid(realUid);
        if (pw && pw->pw_dir) {
            setenv("HOME", pw->pw_dir, 1);
            setenv("USER", pw->pw_name, 1);
            setenv("USERNAME", pw->pw_name, 1);
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
        ssize_t _unused = write(g_pipeToUsb[1], &quit, sizeof(quit));
        (void)_unused;
        close(g_pipeToUsb[1]);
        close(g_pipeToGui[0]);
    }
#endif

    return result;
}

/* MOC generated file - must be at end */
#include "moc_flashmd_qt.cpp"
