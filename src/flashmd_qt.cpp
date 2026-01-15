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

#include <unistd.h>

extern "C" {
#include "flashmd_core.h"
}

/* Size options */
static const uint32_t SIZE_VALUES[] = {0, 128, 256, 512, 1024, 2048, 4096};
static const char* SIZE_LABELS[] = {"Auto", "128 KB", "256 KB", "512 KB", "1 MB", "2 MB", "4 MB"};

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
        setWindowTitle("FlashMD - Sega Genesis ROM Flasher");
        setMinimumSize(500, 600);

        /* Set up file ownership for created files */
        const char *sudoUid = getenv("SUDO_UID");
        const char *sudoGid = getenv("SUDO_GID");
        if (sudoUid && sudoGid) {
            flashmd_set_real_ids(atoi(sudoUid), atoi(sudoGid));
        } else {
            flashmd_set_real_ids(getuid(), getgid());
        }

        setupUi();
        setupWorker();

        log("FlashMD GUI - Ready");
        log("Connect your FlashMaster MD device and click Connect.");
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

        QString filepath = QFileDialog::getOpenFileName(this, "Open ROM File", QString(),
            "ROM Files (*.bin *.md *.gen *.smd);;All Files (*)");
        if (filepath.isEmpty()) return;

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

        QString filepath = QFileDialog::getSaveFileName(this, "Save ROM File", "dump.bin",
            "ROM Files (*.bin *.md *.gen *.smd);;All Files (*)");
        if (filepath.isEmpty()) return;

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

        QString filepath = QFileDialog::getSaveFileName(this, "Save SRAM File", "save.srm",
            "SRAM Files (*.srm *.sav *.bin);;All Files (*)");
        if (filepath.isEmpty()) return;

        if (QMessageBox::question(this, "Confirm Read",
            "Are you sure you want to read SRAM?") != QMessageBox::Yes) return;

        log("");
        m_worker->setOperation(UsbWorker::OP_READ_SRAM, filepath);
        startOperation();
    }

    void onWriteSram() {
        if (m_worker->isRunning()) return;

        QString filepath = QFileDialog::getOpenFileName(this, "Open SRAM File", QString(),
            "SRAM Files (*.srm *.sav *.bin);;All Files (*)");
        if (filepath.isEmpty()) return;

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
        QLabel *title = new QLabel("FlashMD");
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
    QApplication app(argc, argv);
    app.setStyle(QStyleFactory::create("Fusion"));

    MainWindow window;
    window.show();

    return app.exec();
}

/* MOC generated file - must be at end */
#include "moc_flashmd_qt.cpp"
