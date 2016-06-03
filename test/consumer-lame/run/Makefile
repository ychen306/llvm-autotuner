LEVEL = ../../..

TARGET = consumer-lame

SRCDIR = ../src
MYSRCS = $(wildcard $(SRCDIR)/*.c) 
MODULES = $(MYSRCS:%.c=%.bc)

CPPFLAGS = -DHAVEMPGLIB -DLAMEPARSE -DNDEBUG -D__NO_MATH_INLINES -O -DLAMESNDFILE
LIBS = -lm
ARGS = -S ./large.wav Output/output_large.mp3 

include $(LEVEL)/prof.mak

$(TARGET)-native:
	clang $(CPPFLAGS) $(MYSRCS) -lm -o $@

default:
	echo "MYSRCS = " $(MYSRCS)
	echo "MODULES = " $(MODULES)

