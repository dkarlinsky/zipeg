PROG=../../../bin/Codecs/LZMA.so

LOCAL_FLAGS=-DCOMPRESS_MF_MT -D_ST_MODE

MY_WINDOWS=

include ../../../makefile.machine

LOCAL_SHARED=$(LINK_SHARED)
LIBS=$(LOCAL_LIBS)

OBJS = \
../../../Common/MyWindows.o \
../../../Common/Alloc.o \
../../../Common/CRC.o \
InBuffer.o \
OutBuffer.o \
StreamUtils.o \
RangeCoderBit.o \
MT.o \
LZInWindow.o \
LZOutWindow.o \
Synchronization.o \
LZMADecoder.o \
LZMAEncoder.o

include ../../../makefile.glb

