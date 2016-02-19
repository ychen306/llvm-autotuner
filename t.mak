# makefile for generating `loop-prof.out.csv` for a bitcode file

# begin config
BC = raytracer.bc
BIN_DIR = bin
OBJ_DIR = obj

# command line argument to run the executable
ARGS =
# end config

OS := $(shell sh -c 'uname -s 2>/dev/null || echo not')
ifneq ($(OS),Darwin)
	LIB += -lrt
endif

PROF_OUT = loop-prof.flat.csv loop-prof.graph.csv

INSTRUMENTED_BC = $(BC:%.bc=%.prof.bc)
OBJ = $(BC:%.bc=%.prof.o)
EXE = $(BC:%.bc=%.prof.exe)

all: $(EXE)
	./$(EXE) $(ARGS)

$(INSTRUMENTED_BC): $(BC)
	$(BIN_DIR)/instrument-loops $^ -o $@

$(OBJ): $(OBJ_DIR)/prof.bc $(INSTRUMENTED_BC)
	llvm-link $^ -o - | llc -filetype=obj -o $@

$(EXE): $(OBJ)
	$(CXX) $^ -o $@ $(LIB)

clean:
	rm -f $(EXE) $(OBJ) $(INSTRUMENTED_BC) $(PROF_OUT)
