## Tools and what they do
### extract-loops
Splits a module into multiple modules given loops that the user wants to extract.
After running the program, there will be n + 1 new modules, where n is the number
of loops specified by the user
### instrument-loops
Inserts instructions to profile all top-level loops within a module. The module
should have `main` defined. After instrumenting the module, use `llvm-link` to
link with `prof.bc`. Instrumented module will automatically dump the profile output
to `prof.out.csv`
For example, to profile top-level loops in `fib.bc`, one can
```shell
./instrument-loops fib.bc
llvm-link fib.bc prof.bc -o - | llc -filetype=obj -o fib.o
cc fib.o -o fib
./fib && cat prof.out.csv
```
