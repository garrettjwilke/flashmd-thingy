#include "../inc/mainwindow.h"
#include "ui_mainwindow.h"
#include "../inc/theme.h"
#include <QtSerialPort/QtSerialPort>
#include <QtSerialPort/QSerialPortInfo>
#include <QDebug>
#include <QThread>
#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include <QMouseEvent>

// Define a serial port pointer
QSerialPort *COM = new QSerialPort();

void delay(int milliseconds) {
    QThread::msleep(milliseconds);
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , selectedRomSize(0)
    , receivedDataLength(0)
    , address(0)
    , waitingForResponse(false)
    , isDarkMode(false)
{
    ui->setupUi(this);

    // Make logo clickable for theme toggle
    ui->logoLabel->installEventFilter(this);
    ui->logoLabel->setCursor(Qt::PointingHandCursor);

    // Load saved theme preference
    QSettings settings;
    isDarkMode = settings.value("darkMode", false).toBool();
    Theme::apply(this, isDarkMode);

    // Connect flashsize dropdown to update erase button text
    connect(ui->flashsize, &QComboBox::currentTextChanged,
            this, &MainWindow::on_flashsize_currentTextChanged);

    // Set initial button text based on default selection
    on_flashsize_currentTextChanged(ui->flashsize->currentText());

    // Hide ROM selection widgets until a ROM is selected
    ui->selectedRomInfo->hide();
    ui->writeRom->hide();

    // Hide all UI until device is connected
    setConnectedUIVisible(false);

    // Auto-refresh device list on startup
    on_refreshPorts_clicked();

    // Load auto-connect setting
    bool autoConnect = settings.value("autoConnect", false).toBool();
    ui->autoConnectCheckbox->setChecked(autoConnect);

    // Connect checkbox to save setting
    connect(ui->autoConnectCheckbox, &QCheckBox::toggled, this, [](bool checked) {
        QSettings settings;
        settings.setValue("autoConnect", checked);
    });

    // Auto-select last used device if available
    QString lastDevice = settings.value("lastDevice", "").toString();
    bool deviceFound = false;
    if (!lastDevice.isEmpty()) {
        int index = ui->portList->findText(lastDevice);
        if (index >= 0) {
            ui->portList->setCurrentIndex(index);
            deviceFound = true;
        }
    }

    // Auto-connect if enabled and device was found
    if (autoConnect && deviceFound) {
        on_connectDevice_clicked();
    }
}


MainWindow::~MainWindow()
{
    if (COM->isOpen())
        {
            COM->close();
        }
    delete ui;
}


void MainWindow::on_refreshPorts_clicked()
{
    ui->progressBar->setValue(0);
    progress=0;
    ui->portList->clear();
    foreach (const QSerialPortInfo &info, QSerialPortInfo::availablePorts())
       {
        ui->portList->addItem(info.portName());
       }

    // Auto-select last connected device if available
    QSettings settings;
    QString lastDevice = settings.value("lastDevice", "").toString();
    if (!lastDevice.isEmpty()) {
        int index = ui->portList->findText(lastDevice);
        if (index >= 0) {
            ui->portList->setCurrentIndex(index);
        }
    }

    connect(COM, &QSerialPort::readyRead, this, &MainWindow::readSerialData);
}


void MainWindow::on_connectDevice_clicked()
{
    ui->progressBar->setValue(0);
    progress = 0;
    if (COM->isOpen())
    {
       COM->close();
       ui->connectDevice->setText("Connect");
       setConnectedUIVisible(false);
    }
    else
    {
        COM->setPortName(ui->portList->currentText());
        COM->setBaudRate(1000000);
        COM->setDataBits(QSerialPort::Data8);
        COM->setParity(QSerialPort::NoParity);
        COM->setStopBits(QSerialPort::OneStop);
        COM->setFlowControl(QSerialPort::NoFlowControl);
        if (COM->open(QIODevice::ReadWrite))
        {
            ui->connectDevice->setText("Disconnect");
            ui->connectedDeviceLabel->setText("Connected: " + ui->portList->currentText());

            // Save last connected device
            QSettings settings;
            settings.setValue("lastDevice", ui->portList->currentText());

            setConnectedUIVisible(true);
            ui->infoDisplay->clear();
            ui->infoDisplay->append("Device connected\r\n");
            const uint8_t controlByte = 0x0C;
            const uint8_t controlByte1 = 0xAA;
            const uint8_t controlByte2 = 0x55;
            const uint8_t controlByte3 = 0xBB;
            QByteArray packet;
            packet.append(controlByte);
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte2); // 24-bit address high byte
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte3);  // 24-bit address middle byte
            COM->write(packet);
        }
        else
        {
            ui->infoDisplay->append("Unable to connect to device\r\n");
        }
     }
}

void MainWindow::on_clearInfo_clicked()
{
    ui->progressBar->setValue(0);
    progress=0;
    ui->infoDisplay->clear();
}

void MainWindow::readSerialData()
{
   QByteArray response = COM->readAll();
   QString str_rev;
   if(response.size()>256){
       if (file.isOpen()) {
            file.write(response);
            QString selectedItem = ui->flashsize->currentText();
            if(selectedItem == "512K"){
                progress=progress+1;
                ui->progressBar->setValue(progress/5.12+1);
                }
            else if(selectedItem == "1M"){
                progress=progress+1;
                ui->progressBar->setValue(progress/10.24+1);
                }
            else if(selectedItem == "2M"){
                progress=progress+1;
                ui->progressBar->setValue(progress/20.48+1);
                }
            else if(selectedItem == "128K"){
                progress=progress+1;
                ui->progressBar->setValue(progress/1.28+1);
                }
            else if(selectedItem == "256K"){
                progress=progress+1;
                ui->progressBar->setValue(progress/2.56+1);
                }
            else{
                progress=progress+1;
                ui->progressBar->setValue(progress/40.96+1);
                }
           }
       }
   else{
       if (response.contains("OK\r\n")) {
           if (!waitingForResponse) {
               return;
           }
           //str_rev = QString(response);
           //ui->infoDisplay->insertPlainText(str_rev);
           //ui->infoDisplay->moveCursor(QTextCursor::End);
           waitingForResponse = false;
           address += 1024;
           addj = addj+1;
           cnt = cnt+1;
           if(cnt==0x40){
               bank=bank+1;
               addj = 0;
               cnt = 0;
           }
           sendData();
       }
       else if (response.contains("GK\r\n")) {
           if (!waitingForResponse) {
               return;
           }
           //str_rev = QString(response);
           //ui->infoDisplay->insertPlainText(str_rev);
           //ui->infoDisplay->moveCursor(QTextCursor::End);
           waitingForResponse = false;
           address += 1024;
           addj = addj+1;
           cnt = cnt+1;
           if(cnt==0x40){
               bank=bank+1;
               addj = 0;
               cnt = 0;
           }
           sendsramData();
       }
       else{
               str_rev = QString(response);
               ui->infoDisplay->insertPlainText(str_rev);
               ui->infoDisplay->moveCursor(QTextCursor::End);
               if (response.contains("s\r\n")){
                   progress=progress+1;
                   ui->progressBar->setValue(progress*2.2);
                   }
               if (response.contains("FINISH!!!\r\n")){
                   ui->progressBar->setValue(100);
                   if (isSaving) {
                           file.close();
                           isSaving = false;
                           ui->infoDisplay->append("File saved\r\n");
                       }
                   }
            }
       }
    //QString hexString = response.toHex(' ').toUpper();
    //ui->shujuxianshi->append(hexString);
}

void MainWindow::on_detectRom_clicked()
{
    ui->progressBar->setValue(0);
    progress=0;
    ui->infoDisplay->clear();
    if (COM->isOpen()) {
            const uint8_t controlByte = 0x0D;
            const uint8_t controlByte1 = 0xAA;
            const uint8_t controlByte2 = 0x55;
            const uint8_t controlByte3 = 0xBB;
            QByteArray packet;
            packet.append(controlByte);
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte2); // 24-bit address high byte
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte3);  // 24-bit address middle byte
            COM->write(packet);
        } else {
            ui->infoDisplay->append("Device not connected\r\n");
        }
}

void MainWindow::on_eraseFlash_clicked()
{
    ui->progressBar->setValue(0);
    progress=0;
    ui->infoDisplay->clear();
    if (COM->isOpen()) {
            const uint8_t controlByte = 0x0E;
            const uint8_t controlByte1 = 0xAA;
            const uint8_t controlByte2 = 0x55;
            const uint8_t controlByte3 = 0xBB;
            QByteArray packet;
            packet.append(controlByte);
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte2); // 24-bit address high byte
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte3);  // 24-bit address middle byte
            COM->write(packet);
        } else {
            ui->infoDisplay->append("Device not connected\r\n");
        }

}


void MainWindow::on_writeRom_clicked()
{
    ui->progressBar->setValue(0);
    progress = 0;
    ui->infoDisplay->clear();

    // Check if a ROM has been selected
    if (selectedRomPath.isEmpty()) {
        ui->infoDisplay->append("No ROM selected. Use 'Select ROM' first.\r\n");
        return;
    }

    if (!COM->isOpen()) {
        ui->infoDisplay->append("Device not connected!\r\n");
        return;
    }

    // Get file info for confirmation dialog
    QFileInfo fileInfo(selectedRomPath);
    QString sizeLabel = ui->flashsize->currentText();

    // Show confirmation dialog
    QMessageBox::StandardButton reply = QMessageBox::question(
        this,
        "Write ROM to Cart?",
        QString("File: %1\nSize: %2\nDevice: %3\n\nThis will overwrite the cart contents.")
            .arg(fileInfo.fileName())
            .arg(sizeLabel)
            .arg(ui->portList->currentText()),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );

    if (reply != QMessageBox::Yes) {
        return;
    }

    // Proceed with write
    file.setFileName(selectedRomPath);
    if (!file.open(QIODevice::ReadOnly)) {
        ui->infoDisplay->append("File open failed!\r\n");
        return;
    }

    fileSize = file.size();
    address = 0x0;
    addj = 0x0;
    cnt = 0x0;
    bank = 0x0;
    sendData();
}


void MainWindow::sendData()
{
    if(fileSize==0x80000){
        progress=progress+1;
        ui->progressBar->setValue(progress/5.12+1);
         if(address >= 0x80000){
            const uint8_t controlByte = 0x0F;
            const uint8_t controlByte1 = 0xAA;
            const uint8_t controlByte2 = 0x55;
            const uint8_t controlByte3 = 0xBB;
            QByteArray packet;
            packet.append(controlByte);
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte2); // 24-bit address high byte
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte3);  // 24-bit address middle byte
            COM->write(packet);
            file.close();
            ui->infoDisplay->append("Game burning completed\r\n");
            return;
            }
        }
    else if(fileSize==0x100000){
        progress=progress+1;
        ui->progressBar->setValue(progress/10.24+1);
        if(address >= 0x100000){
            const uint8_t controlByte = 0x0F;
            const uint8_t controlByte1 = 0xAA;
            const uint8_t controlByte2 = 0x55;
            const uint8_t controlByte3 = 0xBB;
            QByteArray packet;
            packet.append(controlByte);
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte2); // 24-bit address high byte
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte3);  // 24-bit address middle byte
            COM->write(packet);
            file.close();
            ui->infoDisplay->append("Game burning completed\r\n");
            return;
            }
        }
    else if(fileSize==0x200000){
        progress=progress+1;
        ui->progressBar->setValue(progress/20.48+1);
        if(address >= 0x200000){
            const uint8_t controlByte = 0x0F;
            const uint8_t controlByte1 = 0xAA;
            const uint8_t controlByte2 = 0x55;
            const uint8_t controlByte3 = 0xBB;
            QByteArray packet;
            packet.append(controlByte);
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte2); // 24-bit address high byte
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte3);  // 24-bit address middle byte
            COM->write(packet);
            file.close();
            ui->infoDisplay->append("Game burning completed\r\n");
            return;
            }
        }
    else if(fileSize==0x300000){
        progress=progress+1;
        ui->progressBar->setValue(progress/30.72+1);
        if(address >= 0x300000){
            const uint8_t controlByte = 0x0F;
            const uint8_t controlByte1 = 0xAA;
            const uint8_t controlByte2 = 0x55;
            const uint8_t controlByte3 = 0xBB;
            QByteArray packet;
            packet.append(controlByte);
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte2); // 24-bit address high byte
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte3);  // 24-bit address middle byte
            COM->write(packet);
            file.close();
            ui->infoDisplay->append("Game burning completed\r\n");
            return;
            }
        }
    else if(fileSize==0x400000){
        progress=progress+1;
        ui->progressBar->setValue(progress/40.96+1);
        if(address >= 0x400000){
            const uint8_t controlByte = 0x0F;
            const uint8_t controlByte1 = 0xAA;
            const uint8_t controlByte2 = 0x55;
            const uint8_t controlByte3 = 0xBB;
            QByteArray packet;
            packet.append(controlByte);
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte2); // 24-bit address high byte
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte3);  // 24-bit address middle byte
            COM->write(packet);
            file.close();
            ui->infoDisplay->append("Game burning completed\r\n");
            return;
            }
        }
    else {
        ui->progressBar->setValue(0);
            const uint8_t controlByte = 0x0F;
            const uint8_t controlByte1 = 0xAA;
            const uint8_t controlByte2 = 0x55;
            const uint8_t controlByte3 = 0xBB;
            QByteArray packet;
            packet.append(controlByte);
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte2); // 24-bit address high byte
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte3);  // 24-bit address middle byte
            COM->write(packet);
            file.close();
            ui->infoDisplay->append("The file size is not supported by the system\r\n");
            return;
        }
    const uint8_t controlByte = 0x0B;
    const uint8_t controlByte1 = 0xAA;
    const uint8_t controlByte2 = 0x55;
    const uint8_t controlByte3 = 0xBB;
    QByteArray data = file.read(1024);
    COM->write(data);
    QByteArray packet = 0;
    packet.append(controlByte);
    packet.append(controlByte1);
    packet.append(controlByte2);
    packet.append(controlByte1);
    packet.append(controlByte3);
    packet.append(addj&0xff);
    packet.append(bank&0xff);
    COM->write(packet);
    waitingForResponse = true;
}

void MainWindow::sendsramData()
{
   if(fileSize==0x2000){
        progress=progress+1;
        ui->progressBar->setValue(progress/0.08+1);
        if(address >= 0x2000){
            const uint8_t controlByte = 0x0F;
            const uint8_t controlByte1 = 0xAA;
            const uint8_t controlByte2 = 0x55;
            const uint8_t controlByte3 = 0xBB;
            QByteArray packet;
            packet.append(controlByte);
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte2); // 24-bit address high byte
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte3);  // 24-bit address middle byte
            COM->write(packet);
            file.close();
            ui->infoDisplay->append("Archive burning completed\r\n");
            return;
            }
        }
    else if(fileSize==0x8000){
        progress=progress+1;
        ui->progressBar->setValue(progress/0.32+1);
        if(address >= 0x8000){
            const uint8_t controlByte = 0x0F;
            const uint8_t controlByte1 = 0xAA;
            const uint8_t controlByte2 = 0x55;
            const uint8_t controlByte3 = 0xBB;
            QByteArray packet;
            packet.append(controlByte);
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte2); // 24-bit address high byte
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte3);  // 24-bit address middle byte
            COM->write(packet);
            file.close();
            ui->infoDisplay->append("Archive burning completed\r\n");
            return;
            }
        }
    else {
        ui->progressBar->setValue(0);
            const uint8_t controlByte = 0x0F;
            const uint8_t controlByte1 = 0xAA;
            const uint8_t controlByte2 = 0x55;
            const uint8_t controlByte3 = 0xBB;
            QByteArray packet;
            packet.append(controlByte);
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte2); // 24-bit address high byte
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte3);  // 24-bit address middle byte
            COM->write(packet);
            file.close();
            ui->infoDisplay->append("The file size is not supported by the system\r\n");
            return;
        }
    const uint8_t controlByte = 0x1B;
    const uint8_t controlByte1 = 0xAA;
    const uint8_t controlByte2 = 0x55;
    const uint8_t controlByte3 = 0xBB;
    QByteArray data = file.read(1024);
    COM->write(data);
    QByteArray packet = 0;
    packet.append(controlByte);
    packet.append(controlByte1);
    packet.append(controlByte2);
    packet.append(controlByte1);
    packet.append(controlByte3);
    packet.append(addj&0xff);
    packet.append(bank&0xff);
    COM->write(packet);
    waitingForResponse = true;
}


QByteArray MainWindow::hexStringToByteArray(const QString &hex)
{
    QByteArray byteArray;
    QStringList hexList = hex.split(" ");
    for (const QString &hexByte : hexList) {
        bool ok;
        byteArray.append(static_cast<char>(hexByte.toInt(&ok, 16)));
    }
    return byteArray;
}


void MainWindow::on_readRom_clicked()
{
    ui->progressBar->setValue(0);
    progress=0;
    QString fileName;
    ui->infoDisplay->clear();
    if (COM->isOpen()){
       fileName = QFileDialog::getSaveFileName(this, tr("Please select the ROM file to burn"), getLastDirectory("lastRomDir"), tr("MDROM files (*.bin *.gen);;All files (*.*)"));
       if (!fileName.isEmpty()) {
           saveLastDirectory("lastRomDir", fileName);
           file.setFileName(fileName);
           if (file.open(QIODevice::WriteOnly)) {
               isSaving = true;
               const uint8_t controlByte = 0x0A;
               const uint8_t controlByte1 = 0xAA;
               const uint8_t controlByte2 = 0x55;
               const uint8_t controlByte3 = 0xBB;
               const uint8_t FLASHSIZE128K = 0x05;
               const uint8_t FLASHSIZE256K = 0x06;
               const uint8_t FLASHSIZE512K = 0x01;
               const uint8_t FLASHSIZE1M = 0x02;
               const uint8_t FLASHSIZE2M = 0x03;
               const uint8_t FLASHSIZE4M = 0x04;
               QString selectedItem = ui->flashsize->currentText();
               if(selectedItem == "128K"){
                   QByteArray packet;
                   packet.append(controlByte);
                   packet.append(controlByte1);
                   packet.append(controlByte2);
                   packet.append(controlByte1);
                   packet.append(controlByte3);
                   packet.append(FLASHSIZE128K);
                   COM->write(packet);
               }
               if(selectedItem == "256K"){
                   QByteArray packet;
                   packet.append(controlByte);
                   packet.append(controlByte1);
                   packet.append(controlByte2);
                   packet.append(controlByte1);
                   packet.append(controlByte3);
                   packet.append(FLASHSIZE256K);
                   COM->write(packet);
               }
               if(selectedItem == "512K"){
                   QByteArray packet;
                   packet.append(controlByte);
                   packet.append(controlByte1);
                   packet.append(controlByte2);
                   packet.append(controlByte1);
                   packet.append(controlByte3);
                   packet.append(FLASHSIZE512K);
                   COM->write(packet);
               }
               if(selectedItem == "1M"){
                   QByteArray packet;
                   packet.append(controlByte);
                   packet.append(controlByte1); // 24-bit address high byte
                   packet.append(controlByte2); // 24-bit address high byte
                   packet.append(controlByte1); // 24-bit address high byte
                   packet.append(controlByte3);  // 24-bit address middle byte
                   packet.append(FLASHSIZE1M);
                   COM->write(packet);
               }
               if(selectedItem == "2M"){
                   QByteArray packet;
                   packet.append(controlByte);
                   packet.append(controlByte1); // 24-bit address high byte
                   packet.append(controlByte2); // 24-bit address high byte
                   packet.append(controlByte1); // 24-bit address high byte
                   packet.append(controlByte3);  // 24-bit address middle byte
                   packet.append(FLASHSIZE2M);
                   COM->write(packet);
               }
               if(selectedItem == "4M"){
                   QByteArray packet;
                   packet.append(controlByte);
                   packet.append(controlByte1); // 24-bit address high byte
                   packet.append(controlByte2); // 24-bit address high byte
                   packet.append(controlByte1); // 24-bit address high byte
                   packet.append(controlByte3);  // 24-bit address middle byte
                   packet.append(FLASHSIZE4M);
                   COM->write(packet);
               }
           } else {
               ui->infoDisplay->append("Error opening file\r\n");
           }

       }
       else{
         ui->infoDisplay->append("No file selected\r\n");
       }
    } else {
        ui->infoDisplay->append("Device not connected\r\n");
    }

}

void MainWindow::on_verifyFile_clicked()
{
    ui->progressBar->setValue(0);
    progress=0;
    ui->infoDisplay->clear();
    filePath1 = QFileDialog::getOpenFileName(this, "Select file 1", "", "All files (*.*)");
    ui->infoDisplay-> append(filePath1);
    ui->infoDisplay-> append("File Opened\r\n");
    delay(100);
    filePath2 = QFileDialog::getOpenFileName(this, "Select file 2", "", "All files (*.*)");
    ui->infoDisplay-> append(filePath2);
    ui->infoDisplay-> append("File Opened\r\n");
    delay(100);
    if (filePath1.isEmpty() || filePath2.isEmpty()) {
            ui->infoDisplay->append("Please select two files for comparison first.\r\n");
            return;
        }
        QByteArray fileContent1 = readFile(filePath1);
        QByteArray fileContent2 = readFile(filePath2);
        compareFiles(fileContent1, fileContent2);
}


QByteArray MainWindow::readFile(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }

    QByteArray fileContent = file.readAll();
    file.close();

    return fileContent;
}

void MainWindow::compareFiles(const QByteArray &fileContent1, const QByteArray &fileContent2)
{
    QString result;
    uint8_t cnt = 0;
    int maxSize = qMax(fileContent1.size(), fileContent2.size());
    for (int i = 0; i < maxSize; ++i) {
        char byte1 = (i < fileContent1.size()) ? fileContent1[i] : 0;
        char byte2 = (i < fileContent2.size()) ? fileContent2[i] : 0;
        if (byte1 != byte2) {
            result += QString("Different byte position: %1\n").arg(i);
            cnt=cnt+1;
        }
    }

    if (result.isEmpty()) {
        result = "The contents of the two files are the same.\r\n";
    }

    if(cnt<=20){
        ui->infoDisplay->append(result);
    }

    else{
        ui->infoDisplay->append("More than 20 differences, not listing them all\r\n");
    }
}

void MainWindow::on_eraseBySize_clicked()
{
    ui->progressBar->setValue(0);
    progress=0;
    ui->infoDisplay->clear();
    if (COM->isOpen()){
        const uint8_t controlByte = 0x1E;
        const uint8_t controlByte1 = 0xAA;
        const uint8_t controlByte2 = 0x55;
        const uint8_t controlByte3 = 0xBB;
        const uint8_t FLASHSIZE512K = 0x01;
        const uint8_t FLASHSIZE1M = 0x02;
        const uint8_t FLASHSIZE2M = 0x03;
        const uint8_t FLASHSIZE4M = 0x04;
        const uint8_t FLASHSIZE128K = 0x05;
        const uint8_t FLASHSIZE256K = 0x06;
        QString selectedItem = ui->flashsize->currentText();
        if(selectedItem == "128K"){
            QByteArray packet;
            packet.append(controlByte);
            packet.append(controlByte1);
            packet.append(controlByte2);
            packet.append(controlByte1);
            packet.append(controlByte3);
            packet.append(FLASHSIZE128K);
            COM->write(packet);
        }
        if(selectedItem == "256K"){
            QByteArray packet;
            packet.append(controlByte);
            packet.append(controlByte1);
            packet.append(controlByte2);
            packet.append(controlByte1);
            packet.append(controlByte3);
            packet.append(FLASHSIZE256K);
            COM->write(packet);
        }
        if(selectedItem == "512K"){
            QByteArray packet;
            packet.append(controlByte);
            packet.append(controlByte1);
            packet.append(controlByte2);
            packet.append(controlByte1);
            packet.append(controlByte3);
            packet.append(FLASHSIZE512K);
            COM->write(packet);
        }
        if(selectedItem == "1M"){
            QByteArray packet;
            packet.append(controlByte);
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte2); // 24-bit address high byte
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte3);  // 24-bit address middle byte
            packet.append(FLASHSIZE1M);
            COM->write(packet);
        }
        if(selectedItem == "2M"){
            QByteArray packet;
            packet.append(controlByte);
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte2); // 24-bit address high byte
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte3);  // 24-bit address middle byte
            packet.append(FLASHSIZE2M);
            COM->write(packet);
        }
        if(selectedItem == "4M"){
            QByteArray packet;
            packet.append(controlByte);
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte2); // 24-bit address high byte
            packet.append(controlByte1); // 24-bit address high byte
            packet.append(controlByte3);  // 24-bit address middle byte
            packet.append(FLASHSIZE4M);
            COM->write(packet);
        }
      } else {
       ui->infoDisplay->append("Device not connected\r\n");
    }
}



void MainWindow::on_writeSave_clicked()
{
    ui->progressBar->setValue(0);
    progress = 0;
    ui->infoDisplay->clear();
    if (COM->isOpen()){
            QString fileName = QFileDialog::getOpenFileName(this, tr("Please select the Save file to Write"), getLastDirectory("lastSaveDir"), tr("SAV files (*.srm *.sav);;All files (*.*)"));
               if (!fileName.isEmpty()) {
                   saveLastDirectory("lastSaveDir", fileName);
                   file.setFileName(fileName);
                   if (file.open(QIODevice::ReadOnly)) {
                       fileSize = file.size();
                       address = 0x0;
                       addj = 0x0;
                       cnt = 0x0;
                       bank = 0x0;
                       sendsramData();
                       }
                    }
           else {
                ui->infoDisplay->append("File Open Failed\r\n");
           }
    } else {
        ui->infoDisplay->append("Device not connected\r\n");
    }
}

void MainWindow::on_readSave_clicked()
{
    ui->progressBar->setValue(0);
    progress=0;
    ui->infoDisplay->clear();
    if (COM->isOpen()){
            QString fileName = QFileDialog::getSaveFileName(this, tr("Please select the file to Save"), getLastDirectory("lastSaveDir"), tr("SAV files (*.srm);;All files (*.*)"));
            if (!fileName.isEmpty())
                {
                    saveLastDirectory("lastSaveDir", fileName);
                    file.setFileName(fileName);
                    if (file.open(QIODevice::WriteOnly))
                        {
                            isSaving = true;
                            const uint8_t controlByte = 0x1A;
                            const uint8_t controlByte1 = 0xAA;
                            const uint8_t controlByte2 = 0x55;
                            const uint8_t controlByte3 = 0xBB;
                            const uint8_t SRAMSIZE256K = 0x01;
                            const uint8_t SRAMSIZE64K = 0x02;
                            QString selectedItem = ui->sramsize->currentText();
                            if(selectedItem == "8K")
                                {
                                    QByteArray packet;
                                    packet.append(controlByte);
                                    packet.append(controlByte1);
                                    packet.append(controlByte2);
                                    packet.append(controlByte1);
                                    packet.append(controlByte3);
                                    packet.append(SRAMSIZE64K);
                                    COM->write(packet);
                                }
                            if(selectedItem == "32K")
                                {
                                    QByteArray packet;
                                    packet.append(controlByte);
                                    packet.append(controlByte1);
                                    packet.append(controlByte2);
                                    packet.append(controlByte1);
                                    packet.append(controlByte3);
                                    packet.append(SRAMSIZE256K);
                                    COM->write(packet);
                                }
                        }
                   else
                        {
                        ui->infoDisplay->append("File Open Failed\r\n");
                        }
                }
        }
    else
        {
            ui->infoDisplay->append("Device not connected\r\n");
        }
}

void MainWindow::on_flashsize_currentTextChanged(const QString &text)
{
    ui->eraseBySize->setText("Erase First " + text);
}

void MainWindow::on_selectRomButton_clicked()
{
    QString fileName = QFileDialog::getOpenFileName(this, tr("Select ROM File"), getLastDirectory("lastRomDir"), tr("MDROM files (*.bin *.md *.smd *.gen);;All files (*.*)"));
    if (fileName.isEmpty()) {
        return;
    }

    saveLastDirectory("lastRomDir", fileName);

    QFileInfo fileInfo(fileName);
    qint64 romSize = fileInfo.size();

    // Determine size label and dropdown index
    QString sizeLabel;
    int dropdownIndex = -1;
    if (romSize == 0x20000) { sizeLabel = "128K"; dropdownIndex = 0; }
    else if (romSize == 0x40000) { sizeLabel = "256K"; dropdownIndex = 1; }
    else if (romSize == 0x80000) { sizeLabel = "512K"; dropdownIndex = 2; }
    else if (romSize == 0x100000) { sizeLabel = "1M"; dropdownIndex = 3; }
    else if (romSize == 0x200000) { sizeLabel = "2M"; dropdownIndex = 4; }
    else if (romSize == 0x400000) { sizeLabel = "4M"; dropdownIndex = 5; }
    else {
        sizeLabel = QString::number(romSize / 1024) + "K";
    }

    // Check if size is valid
    if (dropdownIndex < 0) {
        ui->infoDisplay->clear();
        ui->infoDisplay->append("Invalid ROM size: " + sizeLabel + "\r\n");
        ui->infoDisplay->append("Supported sizes: 128K, 256K, 512K, 1M, 2M, 4M\r\n");
        // Clear previous selection and hide button
        selectedRomPath.clear();
        selectedRomSize = 0;
        ui->selectedRomInfo->hide();
        ui->writeRom->hide();
        ui->writeRom->setStyleSheet("");
        return;
    }

    // Auto-update the ROM SIZE dropdown
    ui->flashsize->setCurrentIndex(dropdownIndex);

    // Store selected ROM info
    selectedRomPath = fileName;
    selectedRomSize = romSize;

    // Update info label and show it
    ui->selectedRomInfo->setText("Selected: " + fileInfo.fileName() + " (" + sizeLabel + ")");
    ui->selectedRomInfo->show();

    // Show the Write ROM button with green highlight
    ui->writeRom->show();
    ui->writeRom->setStyleSheet("background-color: rgb(200, 230, 200); color: rgb(40, 40, 50);");

    // Show success in log
    ui->infoDisplay->clear();
    ui->infoDisplay->append("ROM selected: " + fileInfo.fileName() + "\r\n");
    ui->infoDisplay->append("Size: " + sizeLabel + "\r\n");
}

QString MainWindow::getLastDirectory(const QString &key)
{
    QSettings settings;
    QString dir = settings.value(key, QDir::homePath()).toString();
    if (!QDir(dir).exists()) {
        return QDir::homePath();
    }
    return dir;
}

void MainWindow::saveLastDirectory(const QString &key, const QString &filePath)
{
    QSettings settings;
    QFileInfo fileInfo(filePath);
    settings.setValue(key, fileInfo.absolutePath());
}

void MainWindow::setConnectedUIVisible(bool visible)
{
    // Device selection - opposite visibility
    ui->portList->setVisible(!visible);
    ui->refreshPorts->setVisible(!visible);
    ui->autoConnectCheckbox->setVisible(!visible);
    ui->disconnectedSpacer->setVisible(!visible);
    ui->connectedDeviceLabel->setVisible(visible);

    // Elements within device group that show when connected
    ui->separator1->setVisible(visible);
    ui->detectRom->setVisible(visible);
    ui->readRom->setVisible(visible);
    ui->readSave->setVisible(visible);

    // Group boxes that show when connected
    ui->romGroupBox->setVisible(visible);
    ui->sramGroupBox->setVisible(visible);
    ui->progressBar->setVisible(visible);
    ui->groupBox_3->setVisible(visible);

    // These are hidden until ROM is selected
    if (!visible) {
        ui->selectedRomInfo->hide();
        ui->writeRom->hide();
        ui->writeRom->setStyleSheet("");
        selectedRomPath.clear();
        selectedRomSize = 0;
    }
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == ui->logoLabel && event->type() == QEvent::MouseButtonPress) {
        // Toggle theme
        isDarkMode = !isDarkMode;

        // Save preference
        QSettings settings;
        settings.setValue("darkMode", isDarkMode);

        // Apply theme
        Theme::apply(this, isDarkMode);
        return true;
    }
    return QMainWindow::eventFilter(obj, event);
}
