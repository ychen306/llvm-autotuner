LEVEL = ../../..

TARGET = consumer-lame
CPPFLAGS = -DHAVEMPGLIB -DLAMEPARSE -DNDEBUG -D__NO_MATH_INLINES -O -DLAMESNDFILE
MYSRCS = $(wildcard *.c)
MODULES = $(MYSRCS:%.c=%.bc)

LIBS = -lm
ARGS = -S ./large.wav Output/output_large.mp3 

include $(LEVEL)/prof.mak

$(TARGET)-native:
	clang $(CPPFLAGS) $(MYSRCS) -lm -o $@

default:
	echo "MYSRCS = " $(MYSRCS)
	echo "MODULES = " $(MODULES)

