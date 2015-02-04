#CXXFLAGS := -fopenmp -D_GLIBCXX_PARALLEL -O3 -fPIC -m64 -pipe -Wno-write-strings
#LDFLAGS  := -fopenmp -lm

TARGET       := kmer-mask
SOURCES      := kmer-mask.C

SRC_INCDIRS := ../libutil ../libbio ../libseq ../libkmer ../libmeryl

TGT_LDFLAGS := -L${TARGET_DIR}
TGT_LDLIBS  := -lkmer -lmeryl -lseq -lbio -lutil
TGT_PREREQS := libkmer.a libmeryl.a libseq.a libbio.a libutil.a

SUBMAKEFILES := 