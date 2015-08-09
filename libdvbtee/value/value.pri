INCLUDEPATH += $$PWD

VALUEOBJ_SOURCES = \
    $$PWD/array.cpp \
    $$PWD/object.cpp \
    $$PWD/value.cpp

HEADERS += \
    $$PWD/array.h \
    $$PWD/object.h \
    $$PWD/value-macros.h \
    $$PWD/value.h

OTHER_FILES += $$PWD/Makefile.am
