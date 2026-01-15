/****************************************************************************
** Meta object code from reading C++ file 'flashmd_qt.cpp'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.13)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'flashmd_qt.cpp' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.13. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_UsbWorker_t {
    QByteArrayData data[11];
    char stringdata0[103];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_UsbWorker_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_UsbWorker_t qt_meta_stringdata_UsbWorker = {
    {
QT_MOC_LITERAL(0, 0, 9), // "UsbWorker"
QT_MOC_LITERAL(1, 10, 15), // "progressChanged"
QT_MOC_LITERAL(2, 26, 0), // ""
QT_MOC_LITERAL(3, 27, 7), // "current"
QT_MOC_LITERAL(4, 35, 5), // "total"
QT_MOC_LITERAL(5, 41, 10), // "logMessage"
QT_MOC_LITERAL(6, 52, 7), // "message"
QT_MOC_LITERAL(7, 60, 7), // "isError"
QT_MOC_LITERAL(8, 68, 17), // "operationFinished"
QT_MOC_LITERAL(9, 86, 7), // "success"
QT_MOC_LITERAL(10, 94, 8) // "errorMsg"

    },
    "UsbWorker\0progressChanged\0\0current\0"
    "total\0logMessage\0message\0isError\0"
    "operationFinished\0success\0errorMsg"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_UsbWorker[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       3,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       3,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    2,   29,    2, 0x06 /* Public */,
       5,    2,   34,    2, 0x06 /* Public */,
       8,    2,   39,    2, 0x06 /* Public */,

 // signals: parameters
    QMetaType::Void, QMetaType::Int, QMetaType::Int,    3,    4,
    QMetaType::Void, QMetaType::QString, QMetaType::Bool,    6,    7,
    QMetaType::Void, QMetaType::Bool, QMetaType::QString,    9,   10,

       0        // eod
};

void UsbWorker::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<UsbWorker *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->progressChanged((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< int(*)>(_a[2]))); break;
        case 1: _t->logMessage((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< bool(*)>(_a[2]))); break;
        case 2: _t->operationFinished((*reinterpret_cast< bool(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (UsbWorker::*)(int , int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&UsbWorker::progressChanged)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (UsbWorker::*)(const QString & , bool );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&UsbWorker::logMessage)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (UsbWorker::*)(bool , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&UsbWorker::operationFinished)) {
                *result = 2;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject UsbWorker::staticMetaObject = { {
    QMetaObject::SuperData::link<QThread::staticMetaObject>(),
    qt_meta_stringdata_UsbWorker.data,
    qt_meta_data_UsbWorker,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *UsbWorker::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *UsbWorker::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_UsbWorker.stringdata0))
        return static_cast<void*>(this);
    return QThread::qt_metacast(_clname);
}

int UsbWorker::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QThread::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 3)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 3;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 3)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 3;
    }
    return _id;
}

// SIGNAL 0
void UsbWorker::progressChanged(int _t1, int _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void UsbWorker::logMessage(const QString & _t1, bool _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void UsbWorker::operationFinished(bool _t1, const QString & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}
struct qt_meta_stringdata_MainWindow_t {
    QByteArrayData data[19];
    char stringdata0[193];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_MainWindow_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_MainWindow_t qt_meta_stringdata_MainWindow = {
    {
QT_MOC_LITERAL(0, 0, 10), // "MainWindow"
QT_MOC_LITERAL(1, 11, 9), // "onConnect"
QT_MOC_LITERAL(2, 21, 0), // ""
QT_MOC_LITERAL(3, 22, 9), // "onCheckId"
QT_MOC_LITERAL(4, 32, 10), // "onWriteRom"
QT_MOC_LITERAL(5, 43, 9), // "onReadRom"
QT_MOC_LITERAL(6, 53, 7), // "onErase"
QT_MOC_LITERAL(7, 61, 10), // "onReadSram"
QT_MOC_LITERAL(8, 72, 11), // "onWriteSram"
QT_MOC_LITERAL(9, 84, 10), // "onClearLog"
QT_MOC_LITERAL(10, 95, 17), // "onProgressChanged"
QT_MOC_LITERAL(11, 113, 7), // "current"
QT_MOC_LITERAL(12, 121, 5), // "total"
QT_MOC_LITERAL(13, 127, 12), // "onLogMessage"
QT_MOC_LITERAL(14, 140, 7), // "message"
QT_MOC_LITERAL(15, 148, 7), // "isError"
QT_MOC_LITERAL(16, 156, 19), // "onOperationFinished"
QT_MOC_LITERAL(17, 176, 7), // "success"
QT_MOC_LITERAL(18, 184, 8) // "errorMsg"

    },
    "MainWindow\0onConnect\0\0onCheckId\0"
    "onWriteRom\0onReadRom\0onErase\0onReadSram\0"
    "onWriteSram\0onClearLog\0onProgressChanged\0"
    "current\0total\0onLogMessage\0message\0"
    "isError\0onOperationFinished\0success\0"
    "errorMsg"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_MainWindow[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      11,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       0,       // signalCount

 // slots: name, argc, parameters, tag, flags
       1,    0,   69,    2, 0x08 /* Private */,
       3,    0,   70,    2, 0x08 /* Private */,
       4,    0,   71,    2, 0x08 /* Private */,
       5,    0,   72,    2, 0x08 /* Private */,
       6,    0,   73,    2, 0x08 /* Private */,
       7,    0,   74,    2, 0x08 /* Private */,
       8,    0,   75,    2, 0x08 /* Private */,
       9,    0,   76,    2, 0x08 /* Private */,
      10,    2,   77,    2, 0x08 /* Private */,
      13,    2,   82,    2, 0x08 /* Private */,
      16,    2,   87,    2, 0x08 /* Private */,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Int, QMetaType::Int,   11,   12,
    QMetaType::Void, QMetaType::QString, QMetaType::Bool,   14,   15,
    QMetaType::Void, QMetaType::Bool, QMetaType::QString,   17,   18,

       0        // eod
};

void MainWindow::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<MainWindow *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->onConnect(); break;
        case 1: _t->onCheckId(); break;
        case 2: _t->onWriteRom(); break;
        case 3: _t->onReadRom(); break;
        case 4: _t->onErase(); break;
        case 5: _t->onReadSram(); break;
        case 6: _t->onWriteSram(); break;
        case 7: _t->onClearLog(); break;
        case 8: _t->onProgressChanged((*reinterpret_cast< int(*)>(_a[1])),(*reinterpret_cast< int(*)>(_a[2]))); break;
        case 9: _t->onLogMessage((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< bool(*)>(_a[2]))); break;
        case 10: _t->onOperationFinished((*reinterpret_cast< bool(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        default: ;
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject MainWindow::staticMetaObject = { {
    QMetaObject::SuperData::link<QMainWindow::staticMetaObject>(),
    qt_meta_stringdata_MainWindow.data,
    qt_meta_data_MainWindow,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *MainWindow::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *MainWindow::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_MainWindow.stringdata0))
        return static_cast<void*>(this);
    return QMainWindow::qt_metacast(_clname);
}

int MainWindow::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QMainWindow::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 11)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 11;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 11)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 11;
    }
    return _id;
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
