TARGET   := tgStoreDump
SOURCES  := tgStoreDump.C \
            tgTigSizeAnalysis.C

SRC_INCDIRS := .. ../utility/src

TGT_LDFLAGS := -L${TARGET_DIR}/lib
TGT_LDLIBS  := -l${MODULE}
TGT_PREREQS := lib${MODULE}.a
