#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QSerialPort>
#include <QFileDialog>
#include <QFile>
#include <QSettings>
#include <QDir>


QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void on_refreshPorts_clicked();
    void on_connectDevice_clicked();
    void on_clearInfo_clicked();
    void readSerialData();
    void on_detectRom_clicked();
    void on_eraseFlash_clicked();
    void on_writeRom_clicked();
    void on_readRom_clicked();
    void on_verifyFile_clicked();
    void on_eraseBySize_clicked();
    void on_writeSave_clicked();
    void on_readSave_clicked();
    void on_flashsize_currentTextChanged(const QString &text);
    void on_selectRomButton_clicked();

private:
    Ui::MainWindow *ui;
    QSerialPort *serial;
    QString filePath;
    QString selectedRomPath;
    qint64 selectedRomSize;
    QByteArray hexStringToByteArray(const QString &hex);
    qint64 receivedDataLength;
    qint64 fileSize;
    QFile file;
    QByteArray buffer;
    bool isSaving;
    uint32_t address;
    uint32_t addj;
    uint32_t cnt;
    uint32_t bank;
    uint32_t progress;
    bool waitingForResponse;
    void sendData();
    void sendsramData();
    QString filePath1;
    QString filePath2;
    QByteArray readFile(const QString &filePath);
    void compareFiles(const QByteArray &fileContent1, const QByteArray &fileContent2);
    QString getLastDirectory(const QString &key);
    void saveLastDirectory(const QString &key, const QString &filePath);
    void setConnectedUIVisible(bool visible);
    bool isDarkMode;

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
};
#endif // MAINWINDOW_H
