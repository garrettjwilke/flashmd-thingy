QT       += core gui serialport

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++11
CONFIG -= app_bundle

APP_NAME = "flashmd-thingy" # no spaces!
ORG_NAME = $$APP_NAME

DESTDIR = build
OBJECTS_DIR = $$DESTDIR/obj
TARGET = $$APP_NAME
MOC_DIR = $${OBJECTS_DIR}
RCC_DIR = $${OBJECTS_DIR}
UI_DIR  = $${OBJECTS_DIR}
INCLUDEPATH += $${UI_DIR}
QMAKE_DISTCLEAN += .qmake.stash
QMAKE_CLEAN += $${DESTDIR}/*

# Export app constants to C++ as preprocessor macros
DEFINES += APP_NAME=\\\"$$APP_NAME\\\"
DEFINES += ORG_NAME=\\\"$$ORG_NAME\\\"

# The following define makes your compiler emit warnings if you use
# any Qt feature that has been marked deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    src/main.cpp \
    src/mainwindow.cpp \
    src/theme.cpp

HEADERS += \
    inc/mainwindow.h \
    inc/theme.h

FORMS += \
    src/mainwindow.ui
	
RC_ICONS = "res/md.ico"

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

DISTFILES +=

RESOURCES += \
    res/resources.qrc
