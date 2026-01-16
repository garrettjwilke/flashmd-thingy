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
#include <QFontDatabase>
#include <QListView>

#include "theme.h"

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

        setMinimumSize(WINDOW_WIDTH, WINDOW_HEIGHT);
        setMaximumSize(WINDOW_WIDTH, WINDOW_HEIGHT);
        resize(WINDOW_WIDTH, WINDOW_HEIGHT);

        log("flashmd-thingy");

        // Reapply theme after event loop starts to fix combo box styling
        QTimer::singleShot(0, this, [this]() {
            applyTheme(m_currentTheme);
        });
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
                               false);
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
                               false);
        startOperation();
    }

    void onErase() {
        if (m_worker->isRunning()) return;

        if (QMessageBox::question(this, "Confirm Erase",
            "Are you sure you want to erase the flash memory?") != QMessageBox::Yes) return;

        log("");
        m_worker->setOperation(UsbWorker::OP_ERASE, QString(),
                               SIZE_VALUES[m_sizeCombo->currentIndex()],
                               false, false,
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
        QString errorColor = (m_currentTheme == "light") ? LIGHT_ERROR : DARK_ERROR;
        if (isError) {
            m_console->append("<span style='color: " + errorColor + ";'>" + message.toHtmlEscaped() + "</span>");
        } else {
            /* Handle progress dots specially - append inline without newline */
            if (message == ".") {
                m_console->moveCursor(QTextCursor::End);
                m_console->insertPlainText(".");
                m_console->moveCursor(QTextCursor::End);
            } else {
                m_console->append(message.toHtmlEscaped());
            }
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

        /* Logo section */
        m_logoLabel = new QLabel();
        QPixmap logo(":/images/logo.png");
        m_logoLabel->setPixmap(logo);
        m_logoLabel->setAlignment(Qt::AlignCenter);
        mainLayout->addWidget(m_logoLabel);

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
        m_sizeCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_sizeListView = new QListView(m_sizeCombo);
        m_sizeCombo->setView(m_sizeListView);
        for (int i = 0; i < 7; i++) {
            m_sizeCombo->addItem(SIZE_LABELS[i]);
        }
        sizeLayout->addWidget(m_sizeCombo, 1);
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
        m_progressLabel->setStyleSheet("color: " LIGHT_TEXT_SUBTLE "; font-size: " FONT_SIZE_SMALL "; font-weight: 500;");
        progressLayout->addWidget(m_progressLabel);
        mainLayout->addLayout(progressLayout);

        /* Console section */
        QGroupBox *consoleGroup = new QGroupBox("Console Output");
        QVBoxLayout *consoleLayout = new QVBoxLayout(consoleGroup);
        consoleLayout->setContentsMargins(16, 16, 16, 12);

        m_console = new QTextEdit();
        m_console->setReadOnly(true);
        m_console->setMinimumHeight(CONSOLE_MIN_HEIGHT);
        m_console->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        consoleLayout->addWidget(m_console);

        mainLayout->addWidget(consoleGroup, 1);

        /* Bottom buttons */
        QHBoxLayout *bottomLayout = new QHBoxLayout();
        m_clearBtn = new QPushButton("Clear");
        connect(m_clearBtn, &QPushButton::clicked, this, &MainWindow::onClearLog);
        bottomLayout->addWidget(m_clearBtn);
        bottomLayout->addStretch();

        // Theme toggle button
        m_themeBtn = new QPushButton();
        m_themeBtn->setText(m_currentTheme == "dark" ? "☀" : "☾");
        m_themeBtn->setToolTip("Toggle theme");
        m_themeBtn->setFixedSize(THEME_BTN_SIZE, THEME_BTN_SIZE);
        m_themeBtn->setStyleSheet(R"(
            QPushButton {
                background-color: transparent;
                border: 2px solid;
                border-radius: 8px;
                font-size: 16px;
                padding: 0;
            }
            QPushButton:hover {
                background-color: rgba(128, 128, 128, 0.2);
            }
        )");
        connect(m_themeBtn, &QPushButton::clicked, this, &MainWindow::onThemeChanged);
        bottomLayout->addWidget(m_themeBtn);

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
        bool isLight = (m_currentTheme == "light");
        QString bg = isLight ? LIGHT_BG : DARK_BG;
        QString groupBg = isLight ? LIGHT_GROUP_BG : DARK_GROUP_BG;
        QString text = isLight ? LIGHT_TEXT : DARK_TEXT;
        QString consoleBg = isLight ? LIGHT_CONSOLE_BG : DARK_CONSOLE_BG;
        QString progressBg = isLight ? LIGHT_PROGRESS_BG : DARK_PROGRESS_BG;
        QString gray = GRAY_COLOR;
        QString accent = isLight ? LIGHT_ACCENT : DARK_ACCENT;

        QString styleSheet = QString(R"(
            QMainWindow { background-color: %1; }
            QWidget { background-color: %1; color: %2; font-family: "Open Sans"; }
            QGroupBox {
                font-weight: 600; font-size: )" FONT_SIZE_NORMAL R"(; color: %3;
                border: )" BORDER_WIDTH R"( solid %3; border-radius: 12px;
                margin-top: 12px; padding-top: 12px; background-color: %4;
            }
            QGroupBox::title {
                subcontrol-origin: margin; left: )" GROUP_PADDING R"(; padding: 4px 12px;
                background-color: %3; color: white; border: )" BORDER_WIDTH R"( solid %3;
                border-radius: )" BORDER_RADIUS_SMALL R"(;
            }
            QPushButton {
                background-color: %3 !important; color: white !important; border: none !important;
                border-radius: )" BORDER_RADIUS R"(; padding: )" BUTTON_PADDING R"(;
                font-weight: 600; font-size: )" FONT_SIZE_BUTTON R"(; min-height: )" BUTTON_MIN_HEIGHT R"(;
            }
            QComboBox {
                background-color: %3 !important; border: )" BORDER_WIDTH R"( solid %3 !important;
                border-radius: )" BORDER_RADIUS R"(; padding: )" COMBO_PADDING R"(;
                min-height: )" COMBO_MIN_HEIGHT R"(; font-size: )" FONT_SIZE_NORMAL R"(; color: white !important;
            }
            QComboBox::drop-down { border: none; width: 30px; }
            QComboBox::down-arrow {
                image: none; border-left: 5px solid transparent;
                border-right: 5px solid transparent; border-top: 6px solid white; width: 0; height: 0;
            }
            QCheckBox { font-size: )" FONT_SIZE_NORMAL R"(; spacing: 8px; color: %3 !important; }
            QCheckBox::indicator {
                width: 20px; height: 20px; border: )" BORDER_WIDTH R"( solid %3 !important;
                border-radius: 4px; background-color: %3 !important;
            }
            QProgressBar {
                border: none; border-radius: )" BORDER_RADIUS R"(;
                background-color: %5; height: )" PROGRESS_HEIGHT R"(; text-align: center;
            }
            QProgressBar::chunk { background-color: %6; border-radius: )" BORDER_RADIUS R"(; }
            QTextEdit {
                background-color: %7; color: %2; border: none;
                border-radius: )" BORDER_RADIUS R"(; padding: 8px;
                font-family: "Roboto Mono"; font-size: )" FONT_SIZE_SMALL R"(;
            }
            QLabel { font-size: )" FONT_SIZE_NORMAL R"(; color: %3 !important; }
            QGroupBox QLabel { background-color: transparent; color: %3 !important; }
            QGroupBox QCheckBox { background-color: transparent; color: %3 !important; }
        )").arg(bg).arg(text).arg(gray).arg(groupBg).arg(progressBg).arg(accent).arg(consoleBg);

        qApp->setStyleSheet(styleSheet);

        if (m_progressLabel) {
            m_progressLabel->setStyleSheet(
                QString("color: %1; font-size: " FONT_SIZE_SMALL "; font-weight: 500;").arg(gray));
        }

        if (m_themeBtn) {
            m_themeBtn->setStyleSheet(QString(R"(
                QPushButton {
                    background-color: transparent; border: )" BORDER_WIDTH R"( solid %1;
                    border-radius: )" BORDER_RADIUS R"(; font-size: )" FONT_SIZE_THEME_BTN R"(;
                    padding: 0; color: %1;
                }
                QPushButton:hover { background-color: rgba(128, 128, 128, 0.2); }
            )").arg(gray));
        }

        QString grayBtnStyle = QString(R"(
            QPushButton {
                background-color: %1 !important; color: white !important;
                border: )" BORDER_WIDTH R"( solid %1 !important; border-radius: )" BORDER_RADIUS R"(;
                padding: )" BUTTON_PADDING R"(; font-weight: 600;
                font-size: )" FONT_SIZE_BUTTON R"(; min-height: )" BUTTON_MIN_HEIGHT R"(;
            }
        )").arg(gray);

        if (m_writeRomBtn) m_writeRomBtn->setStyleSheet(grayBtnStyle);
        if (m_readRomBtn) m_readRomBtn->setStyleSheet(grayBtnStyle);
        if (m_eraseBtn) m_eraseBtn->setStyleSheet(grayBtnStyle);
        if (m_writeSramBtn) m_writeSramBtn->setStyleSheet(grayBtnStyle);
        if (m_readSramBtn) m_readSramBtn->setStyleSheet(grayBtnStyle);
        if (m_clearBtn) m_clearBtn->setStyleSheet(grayBtnStyle);

        if (m_sizeListView) {
            m_sizeListView->setStyleSheet(QString(R"(
                QListView { background-color: %1; outline: none; }
                QListView::item { padding: )" LISTVIEW_PADDING R"(; min-height: )" LISTVIEW_MIN_HEIGHT R"(; color: white; }
                QListView::item:hover { background-color: %1; color: white; }
                QListView::item:selected { background-color: %1; color: white; }
            )").arg(gray));
        }
    }

    void log(const QString &message) {
        m_console->append(message);
    }

    void applyTheme(const QString &theme) {
        bool isLight = (theme == "light");

        // select colors based on theme
        QString bg = isLight ? LIGHT_BG : DARK_BG;
        QString text = isLight ? LIGHT_TEXT : DARK_TEXT;
        QString textSubtle = isLight ? LIGHT_TEXT_SUBTLE : DARK_TEXT_SUBTLE;
        QString groupBg = isLight ? LIGHT_GROUP_BG : DARK_GROUP_BG;
        QString border = isLight ? LIGHT_BORDER : DARK_BORDER;
        QString borderDark = isLight ? LIGHT_BORDER_DARK : DARK_BORDER_LIGHT;
        QString titleBg = isLight ? LIGHT_TITLE_BG : DARK_TITLE_BG;
        QString titleBorder = isLight ? LIGHT_TITLE_BORDER : DARK_TITLE_BORDER;
        QString accent = isLight ? LIGHT_ACCENT : DARK_ACCENT;
        QString comboBg = isLight ? LIGHT_COMBO_BG : DARK_COMBO_BG;
        QString comboBorder = isLight ? LIGHT_COMBO_BORDER : DARK_COMBO_BORDER;
        QString checkBg = isLight ? LIGHT_CHECK_BG : DARK_CHECK_BG;
        QString checkBorder = isLight ? LIGHT_CHECK_BORDER : DARK_CHECK_BORDER;
        QString progressBg = isLight ? LIGHT_PROGRESS_BG : DARK_PROGRESS_BG;
        QString consoleBg = isLight ? LIGHT_CONSOLE_BG : DARK_CONSOLE_BG;

        QString styleSheet = QString(R"(
            QMainWindow { background-color: %1; }
            QWidget { background-color: %1; color: %2; font-family: "Open Sans"; }
            QGroupBox {
                font-weight: 600; font-size: )" FONT_SIZE_NORMAL R"(; color: %2;
                border: )" BORDER_WIDTH R"( solid %3; border-radius: 12px;
                margin-top: 12px; padding-top: 12px; background-color: %4;
            }
            QGroupBox::title {
                subcontrol-origin: margin; left: )" GROUP_PADDING R"(; padding: 4px 12px;
                background-color: %5; border: )" BORDER_WIDTH R"( solid %6;
                border-radius: )" BORDER_RADIUS_SMALL R"(;
            }
            QPushButton {
                background-color: %7; color: white; border: none;
                border-radius: )" BORDER_RADIUS R"(; padding: )" BUTTON_PADDING R"(;
                font-weight: 600; font-size: )" FONT_SIZE_BUTTON R"(; min-height: )" BUTTON_MIN_HEIGHT R"(;
            }
            QComboBox {
                background-color: %8; border: )" BORDER_WIDTH R"( solid %9;
                border-radius: )" BORDER_RADIUS R"(; padding: )" COMBO_PADDING R"(;
                min-height: )" COMBO_MIN_HEIGHT R"(; font-size: )" FONT_SIZE_NORMAL R"(; color: %2;
            }
            QComboBox:hover { border-color: %7; }
            QComboBox::drop-down { border: none; width: 30px; }
            QComboBox::down-arrow {
                image: none; border-left: 5px solid transparent;
                border-right: 5px solid transparent; border-top: 6px solid %2; width: 0; height: 0;
            }
            QCheckBox { font-size: )" FONT_SIZE_NORMAL R"(; spacing: 8px; color: %2; }
            QCheckBox::indicator {
                width: 20px; height: 20px; border: )" BORDER_WIDTH R"( solid %10;
                border-radius: 4px; background-color: %11;
            }
            QCheckBox::indicator:hover { border-color: %7; }
            QCheckBox::indicator:checked { background-color: %7; border-color: %7; }
            QProgressBar {
                border: none; border-radius: )" BORDER_RADIUS R"(;
                background-color: %12; height: )" PROGRESS_HEIGHT R"(; text-align: center;
            }
            QProgressBar::chunk { background-color: %7; border-radius: )" BORDER_RADIUS R"(; }
            QTextEdit {
                background-color: %13; color: %2; border: none;
                border-radius: )" BORDER_RADIUS R"(; padding: 8px;
                font-family: "Roboto Mono"; font-size: )" FONT_SIZE_SMALL R"(;
            }
            QLabel { font-size: )" FONT_SIZE_NORMAL R"(; }
            QGroupBox QLabel { background-color: transparent; }
            QGroupBox QCheckBox { background-color: transparent; }
        )").arg(bg).arg(text).arg(border).arg(groupBg).arg(titleBg).arg(titleBorder)
          .arg(accent).arg(comboBg).arg(comboBorder).arg(checkBorder).arg(checkBg)
          .arg(progressBg).arg(consoleBg);

        qApp->setStyleSheet(styleSheet);

        // progress label
        if (m_progressLabel) {
            m_progressLabel->setStyleSheet(
                QString("color: %1; font-size: " FONT_SIZE_SMALL "; font-weight: 500;").arg(textSubtle));
        }

        // theme button
        if (m_themeBtn) {
            m_themeBtn->setStyleSheet(QString(R"(
                QPushButton {
                    background-color: transparent; border: )" BORDER_WIDTH R"( solid %1;
                    border-radius: )" BORDER_RADIUS R"(; font-size: )" FONT_SIZE_THEME_BTN R"(;
                    padding: 0; color: %2;
                }
                QPushButton:hover { background-color: rgba(128, 128, 128, 0.2); }
            )").arg(border).arg(text));
        }

        // combo list view
        if (m_sizeListView) {
            m_sizeListView->setStyleSheet(QString(R"(
                QListView { background-color: %1; outline: none; }
                QListView::item { padding: )" LISTVIEW_PADDING R"(; min-height: )" LISTVIEW_MIN_HEIGHT R"(; color: %3; }
                QListView::item:hover { background-color: %2; color: white; }
                QListView::item:selected { background-color: %2; color: white; }
            )").arg(comboBg).arg(accent).arg(text));
        }

        applyButtonColors(theme);
    }
    
    void applyButtonColors(const QString &theme) {
        QString writeColor = (theme == "light") ? LIGHT_BTN_WRITE : DARK_BTN_WRITE;
        QString readColor = (theme == "light") ? LIGHT_BTN_READ : DARK_BTN_READ;
        QString eraseColor = (theme == "light") ? LIGHT_BTN_ERASE : DARK_BTN_ERASE;
        QString clearColor = (theme == "light") ? LIGHT_BTN_CLEAR : DARK_BTN_CLEAR;
        QString buttonText = (theme == "light") ? LIGHT_BTN_TEXT : DARK_BTN_TEXT;
        QString hoverBorder = (theme == "light") ? LIGHT_BTN_HOVER : DARK_BTN_HOVER;

        auto makeButtonStyle = [&](const QString &bgColor) {
            return QString(R"(
                QPushButton {
                    background-color: %1;
                    color: %2;
                    border: )" BORDER_WIDTH R"( solid %1;
                    border-radius: )" BORDER_RADIUS R"(;
                    padding: )" BUTTON_PADDING R"(;
                    font-weight: 600;
                    font-size: )" FONT_SIZE_BUTTON R"(;
                    min-height: )" BUTTON_MIN_HEIGHT R"(;
                }
                QPushButton:hover {
                    border: )" BORDER_WIDTH R"( solid %3;
                }
                QPushButton:pressed {
                    border: )" BORDER_WIDTH R"( solid %3;
                }
                QPushButton:disabled {
                    opacity: 0.4;
                }
            )").arg(bgColor).arg(buttonText).arg(hoverBorder);
        };
        
        if (m_writeRomBtn) {
            m_writeRomBtn->setStyleSheet(makeButtonStyle(writeColor));
        }
        if (m_readRomBtn) {
            m_readRomBtn->setStyleSheet(makeButtonStyle(readColor));
        }
        if (m_eraseBtn) {
            m_eraseBtn->setStyleSheet(makeButtonStyle(eraseColor));
        }
        if (m_writeSramBtn) {
            m_writeSramBtn->setStyleSheet(makeButtonStyle(writeColor));
        }
        if (m_readSramBtn) {
            m_readSramBtn->setStyleSheet(makeButtonStyle(readColor));
        }
        if (m_clearBtn) {
            m_clearBtn->setStyleSheet(makeButtonStyle(clearColor));
        }
    }

    UsbWorker *m_worker;
    QLabel *m_logoLabel;
    QPushButton *m_themeBtn;
    QPushButton *m_writeRomBtn;
    QPushButton *m_readRomBtn;
    QPushButton *m_eraseBtn;
    QPushButton *m_writeSramBtn;
    QPushButton *m_readSramBtn;
    QPushButton *m_clearBtn;
    QComboBox *m_sizeCombo;
    QListView *m_sizeListView;
    QCheckBox *m_noTrimCheck;
    QCheckBox *m_fullEraseCheck;
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

    /* Load embedded fonts */
    int fontId = QFontDatabase::addApplicationFont(":/fonts/opensans.ttf");
    if (fontId != -1) {
        QStringList fontFamilies = QFontDatabase::applicationFontFamilies(fontId);
        if (!fontFamilies.isEmpty()) {
            QFont defaultFont(fontFamilies.first(), FONT_DEFAULT_PT);
            app.setFont(defaultFont);
        }
    }

    /* Load mono font for console */
    QFontDatabase::addApplicationFont(":/fonts/mono.ttf");

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
