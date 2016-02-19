$(EXE): $(BC)
	bash -c 'llc $(BC) -o - | clang++ -o $(EXE) -x assembler -'

run:
	$(EXE) bzip2.input.program 1

verify:
	bash -c 'diff $(STDOUT) bzip2.out'

#verify:
#	bash -c 'diff $(STDOUT) tsp.out'
#
#run:
#	$(EXE)
