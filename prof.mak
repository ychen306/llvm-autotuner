# makefile for generating `loop-prof.out.csv` for a bitcode file

# begin config
BC = raytracer.bc
BIN_DIR = bin
OBJ_DIR = obj

# command line argument to run the executable
ARGS =
# end config

INSTRUMENTED_BC = $(BC:%.bc=%.prof.bc)
OBJ = $(BC:%.bc=%.prof.o)
EXE = $(BC:%.bc=%.prof.exe)

all: loop-prof.out.csv

$(INSTRUMENTED_BC): $(BC)
	$(BIN_DIR)/instrument-loops $^ -o $@

$(OBJ): $(OBJ_DIR)/prof.bc $(INSTRUMENTED_BC)
	llvm-link $^ -o - | llc -filetype=obj -o $@

$(EXE): $(OBJ)
	$(CXX) $^ -o $@ -lrt

loop-prof.out.csv: $(EXE) 
	./$(EXE) $(ARGS)

clean:
	rm -f $(EXE) $(OBJ) $(INSTRUMENTED_BC) loop-prof.out.csv
