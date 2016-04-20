$(EXE): $(OBJ)
	clang++ $(OBJ) -o $(EXE)

run: $(EXE)
	$(EXE) bzip2.input.program 10

verify:
	bash -c 'diff $(STDOUT) bzip2.out'






#run: $(EXE)
#	bash -c '$(EXE) -init /Users/tom/workspace/llvm-test-suite/MultiSource/Applications/sqlite3/sqliterc :memory: < /Users/tom/workspace/llvm-test-suite/MultiSource/Applications/sqlite3/commands'
#
#verify:
#	/Users/tom/workspace/llvm-test-bin/tools/fpcmp -r 1.0e-9 $(STDOUT) sqlite.out

#verify:
#	diff $(STDOUT) tsp.out
#verify:
#	bash -c 'diff $(STDOUT) tsp.out'
#
#run:
#	$(EXE)
