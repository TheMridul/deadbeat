QT += core gui quick quickcontrols2 testlib
CONFIG += c++17 testcase
TARGET = deadbeat_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += ../src/backend.h ../src/checker.h ../src/jobstore.h ../src/probe.h ../src/theme.h ../src/translator.h
SOURCES += deadbeat_tests.cpp ../src/backend.cpp ../src/checker.cpp ../src/jobstore.cpp ../src/probe.cpp \
           ../src/theme.cpp ../src/translator.cpp
RESOURCES += ../src/resources.qrc
