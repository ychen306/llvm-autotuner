# makefile for linking bitcode files into a server

# begin config
MAIN_BC = ray.0.bc
LOOP_BC = ray.1.bc ray.2.bc
SERVER = ray.server
OBJ_DIR = obj
BIN_DIR = bin
FUNCS = _Z6renderIfEjRKSt6vectorIP6SphereIT_ESaIS4_EE_ _Z5traceIfE4Vec3IT_ERKS2_S4_RKSt6vectorIP6SphereIS1_ESaIS8_EERKi_
MAX_WORKERS = 2
LD = c++
# end config

SERVER_BC = $(SERVER).bc
SERVER_OBJ = $(SERVER).o

all: $(SERVER) 

$(SERVER_BC): $(MAIN_BC)
	$(BIN_DIR)/create-server $^ -o $@ $(FUNCS:%=-f%) -w $(MAX_WORKERS)

$(SERVER_OBJ): $(SERVER_BC) $(LOOP_BC) $(OBJ_DIR)/server.bc
	llvm-link $^ -o - | \
		llc -filetype=obj -o $@ 

$(SERVER): $(SERVER_OBJ)
	$(LD) $^ -o $@ -ldl

clean:
	rm -f $(SERVER_OBJ) $(SERVER_BC) $(SERVER)
