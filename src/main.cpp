#include "../inc/mainwindow.h"

#include <QApplication>
#include <QTranslator>
int main(int argc, char *argv[])
{
    // Suppress the iOS warning on macOS
    qputenv("QT_LOGGING_RULES", "qt.qpa.*=false");
    QApplication a(argc, argv);
    QCoreApplication::setOrganizationName(ORG_NAME);
    QCoreApplication::setApplicationName(APP_NAME);
    MainWindow w;
    w.setWindowTitle(APP_NAME);
//    QString strLanPath = QCoreApplication::applicationDirPath()  + "MDflasher_en.qm";
//    QTranslator trans;
//    trans.load(strLanPath);
//    qApp->installTranslator(&trans);
    w.show();
    return a.exec();
}
