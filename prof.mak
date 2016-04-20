# makefile for generating `loop-prof.out.csv` for a bitcode file

LEVEL := .

# begin config
ifndef TARGET
    TARGET=bzip2
endif
ifndef ARGS
    ARGS = $(TARGET).input.program 10
endif
ifndef SRCDIR
    SRCDIR = .
endif
ifndef MODULES
    MODULES=$(TARGET).bc
endif
BIN_DIR = $(LEVEL)/bin
OBJ_DIR = $(LEVEL)/obj
EXE = $(TARGET)
INSTRUMENTED_MODS = $(MODULES:%.bc=%-prof.bc) 
OBJ = $(INSTRUMENTED_MODS:%.bc=%.o)
LIBS += $(OBJ_DIR)/prof.o

CC = clang
CXX = clang++
LLLINK = llvm-link
LINK = $(CXX)

# command line argument to run the executable
# end config

OS := $(shell sh -c 'uname -s 2>/dev/null || echo not')
ifneq ($(OS),Darwin)
	LIBS += -lrt
endif

PROF_OUT = loop-prof.flat.csv loop-prof.graph.csv

.PRECIOUS: %.bc

all: $(EXE)
	./$(EXE) $(ARGS)

%.bc: $(SRCDIR)/%.c
	$(CC) -c -emit-llvm $<

%.bc: %.cpp
	$(CXX) -c -emit-llvm $<

%.o: %.bc
	llc -filetype=obj $< -o $@

%-prof.bc: %.bc
	$(BIN_DIR)/instrument-loops $< -o $@

$(TARGET)-prof-bc.o: $(INSTRUMENTED_MODS) $(OBJ_DIR)/prof.bc
	$(LINK) $^ -o - | llc -filetype=obj -o $@

$(EXE): $(OBJ)
	$(CXX) $^ $(LIBS) -o $@

clean:
	rm -f $(EXE) $(OBJ) $(INSTRUMENTED_MODS) $(PROF_OUT)

