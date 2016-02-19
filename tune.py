import sys
import subprocess
import csv
from glob import glob
from collections import namedtuple
from tempfile import mkstemp
from functools import partial

FLAT_OUT = 'loop-prof.flat.csv'
GRAPH_OUT = 'loop-prof.graph.csv'

Loop = namedtuple('Loop', ['function', 'header_id', 'time', 'nested', 'idx'])

def print_loop(loop):
    print loop.function, loop.header_id, loop.time 

# a loop's relative time (%) has to be within this threshold to
# become a tuning candidate
UPPERBOUND = 60
LOWERBOUND = 20


def get_loops():
    # mapping function -> list of loop idxs
    func2loop = {}
    
    # figure out what loops we have
    with open(FLAT_OUT) as flat:
        profiles = csv.DictReader(flat)
    
        # mapping loop index -> loop
        loops = {}
        for i, p in enumerate(profiles):
            # function, not loop
            if p['header-id'] == '0':
                continue
    
            reltime = float(p['time(pct)'])
            function = p['function']
            if reltime > 0:
                loops[i] = Loop(time=reltime,
                        header_id=p['header-id'],
                        function=function,
                        nested=list(),
                        idx=i)
                func2loop.setdefault(function, []).append(i)
    
    # figure out relationship between the loops
    # i.e. fill out the `nested` propertie for each loop
    with open(GRAPH_OUT) as graph:
        for i, row in enumerate(graph):
            # row for function
            if i not in loops:
                continue
    
            for j, time in enumerate(row.split()):
                isnan = (time == 'nan')
                # filter out loops not called by `loops[i]`
                if isnan or j not in loops or float(time) <= 0:
                    continue

                if loops[i].function == 'uncompressStream' and loops[j].function == 'compressStream':
                    print time
                    exit()
    
                loops[i].nested.append(j)

    return loops, func2loop

loops, func2loop = get_loops()

visited = set()
sortedloops = []

def visit(i):
    if i in visited:
        return
    visited.add(i)
    for j in loops[i].nested:
        visit(j)
    sortedloops.append(i)

for i in loops:
    if i not in visited:
        visit(i)

sortedloops.reverse()


candidates = []
disqualified = set()
for i in sortedloops:
    if i in disqualified:
        continue
    time = loops[i].time
    if time >= LOWERBOUND and time <= UPPERBOUND:
        candidates.append(loops[i])
        disqualified.update(loops[i].nested)

for loop in loops.itervalues():
    print_loop(loop)
    for i in loop.nested:
        print '\t',
        print_loop(loops[i])

exit()

# exec a shell command
call = partial(subprocess.check_output, shell=True)

# get a temporary file
def get_temp():
    _, f = mkstemp()
    return f

# delete a temporary file
def delete_temp(filename):
    call('rm -f '+filename)


# helper function to call `./autotune -makefile=[makefile] [bc] `
# return the optimization sequence
def tune(bc, makefile, bc_var='BC'):
    call('./autotune -makefile=%s -bc-var=%s %s' % (makefile, bc_var, bc))
    with open(bc+'.passes') as result:
        passes = result.read().strip()
    return passes 


# generate a temporary makefile that extends `orig_makefile` with
# the ability to compile and link the extracted modules
#
# return the makefile and the mapping from <extracted module> -> <its makefile variable>
def gen_makefile(extracted_modules, orig_makefile):
    tempfile = get_temp()
    with open(tempfile, 'wb') as makefile:
        print >>makefile, 'include', orig_makefile 

        # mapping from <extracted module> -> <its makefile variable>
        vars = {}
        i = 0
        for bc in extracted_modules:
            var = 'VAR_%d' % i
            vars[bc] = var
            i += 1
            print >>makefile, var, ':=', bc

        print >>makefile, 'BC =', '<(llvm-link -o -', ' '.join('$(%s)' % var for var in vars.itervalues()), '| bin/combine -o - | opt -O3 -o -)'
        
    return tempfile, vars

# optimize each module with its optimization sequence and link them together
def link(modules, seqs, out_filename):
    optimized_modules = []
    for i, m in enumerate(modules):
        tempfile = get_temp()
        call('opt %s -o %s %s' % (m, tempfile, seqs[i]))
        optimized_modules.append(tempfile)

    call('llvm-link %s | bin/combine -o - | opt -O3 -o %s' % (' '.join(optimized_modules), out_filename))

    for m in optimized_modules:
        delete_temp(m)

# "client" for extract-loops tool
# return a list of extarcted modules, with the first one being the main module
def extract(module, candidates):
    call('bin/extract-loops %s -p extracted %s' % (module, ' '.join('-l%s,%s' % (l.function, l.header_id) for l in candidates)))
    extracted_modules = []
    with open('extracted.list') as extraction_out:
        # main module is in first line
        extracted_modules.append(next(extraction_out).strip())
    
        for line in extraction_out:
            _, m = line.split()
            extracted_modules.append(m)
    return extracted_modules

# now extract candidate loops
provided_makefile = 'provided.mak'
provided_bc = sys.argv[1]
extracted_modules = extract(provided_bc, candidates)
makefile, vars = gen_makefile(extracted_modules, provided_makefile)
# tuning result
seqs = ['-O3']
for m in extracted_modules[1:]:
    seqs.append(tune(m, makefile, vars[m]))
optimized = 'opt-'+provided_bc
link(extracted_modules, seqs, optimized)
delete_temp(makefile)
print 'optimized bitcode file:', optimized
