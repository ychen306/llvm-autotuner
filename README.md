## Tools and what they do
### extract-loops
Splits a module into multiple modules given loops that the user wants to extract. After running the program, there will be n + 1 new modules, where n is the number of loops specified by the user.
### instrument-loops
Inserts instructions to profile all top-level loops and functions within a module. After instrumenting the module, use `llvm-link` to link with `prof.bc`. Instrumented module will automatically dump the profile output to `loop-prof.flat.csv` and `loop-prof.graph.csv` after execution. `loop-prof.flat.csv` has flat information such as how long a loop was run during execution of the program. `loop-prof.graph.csv` shows the "dynamic call graph" (well... it's not really a "call graph" since loops don't call loops literally. but you get the idea) in the form of a table with the row being caller and column being callee. E.g. entry (0, 1) being 25% means that the first loop spends a quarter of its time running the second loop. The index in `loop-prof.graph.csv` implicitly matches the row number in `loop-prof.flat.csv`; this means that the first loop's detail info (such as what function it's in) can be found in the first row of `loop-prof.flat.csv`. All loops are identified by their loop-header basic blocks and have loop-header id starting from one; functions' "loop-header ids" are 0.
 
For example, to profile top-level loops in `fib.bc`, one can do
```shell
./instrument-loops fib.bc -o fib.prof.bc
# make generates `prof.bc'
llvm-link fib.prof.bc prof.bc -o - | llc -filetype=obj -o fib.o
cc fib.o -o fib
./fib && cat loop-prof.flat.csv loop-prof.graph.csv
```
### create-server
Transforms a bitcode file into a "server" that runs specified functions upon request and reports the time it takes to run those functions. Every function call will have its own worker process responsible for actually performing the call (such transformation is however upperbounded so as not to consume too much resource). Multiple functions can be specified. For example, to make a server that runs `loop` (and `loop` only) repeatedly in `x.bc`, one can do
```shell
# build the server
./create-server -f=loop -o x.server.bc
llc x.server.bc -o x.server.o -filetype=obj 
server x.server.o -o x.server -ldl

# spin up the server
./x.server

# ask the server to run `loop`, which is implemented in `loop.so`
# you can run command below as many times as you want
python tuning-cli.py loop --library-path=loop.so > time.txt

# now `time.txt` contains number of cycles it takes to run `loop`
...

# use this command to kill the **worker** responsible for running `loop`
python tuning-cli.py loop --kill
```
see `python tuning-cli.py -h` for further notes on using the client to communicate with the server.
### server.mak
Makefile to building a server from a list of bitcode files. See source for details on usage.
### prof.mak
Makefile to profile top-level loops of a bitcode files. Profiling result will be dumped to `loop-prof.flat.csv` and `loop-prof.graph.csv`. See source for details on usage.
