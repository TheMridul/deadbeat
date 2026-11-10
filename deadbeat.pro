QT += core gui qml quick quickcontrols2

CONFIG += c++17 release
TARGET = deadbeat
TEMPLATE = app

HEADERS += \
    src/backend.h \
    src/checker.h \
    src/cli.h \
    src/jobstore.h \
    src/probe.h \
    src/theme.h \
    src/translator.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/checker.cpp \
    src/cli.cpp \
    src/jobstore.cpp \
    src/probe.cpp \
    src/theme.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc
