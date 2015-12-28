# TODO be more precise about what libs to use
LIBS = all
LDFLAGS = $(shell llvm-config --ldflags --system-libs --libs $(LIBS))
CXXFLAGS = $(shell llvm-config --cxxflags) -g
CXX = clang++

EXE = extract-loops

.PHONY: all clean

all: $(EXE)

extract-loops: extract-loops.o
	$(CXX) $^ $(LDFLAGS) -o $@

clean:
	rm -rf *.o $(EXE)
