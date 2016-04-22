import sys
import os
import re
import subprocess
import csv
import random
from collections import namedtuple
from tempfile import mkdtemp
import sklearn.preprocessing
import sklearn.cluster
import numpy as np
import argparse

class bcolors:
    HEADER = '\033[95m'
    OKBLUE = '\033[94m'
    OKGREEN = '\033[92m'
    WARNING = '\033[93m'
    FAIL = '\033[91m'
    ENDC = '\033[0m'
    BOLD = '\033[1m'
    UNDERLINE = '\033[4m'

# a loop's relative time (%) has to be above this threshold to become a tuning candidate
TUNING_UPPERBOUND = 60
TUNING_LOWERBOUND = 10

MAX_INVOS = 10000
# maximum number of workers spawn to run invocations
MAX_WORKERS = 100

Loop = namedtuple('Loop', [
    'function',
    'header_id',
    'runs',
    'time',
    'nested',
    'idx'])

def get_loops():
    # mapping function -> list of loop idxs
    func2loop = {}

    # mapping loop index -> loop
    loops = {}

    # figure out what loops we have
    with open(config.flat_profile) as flat:
        profiles = csv.DictReader(flat)

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
                        runs=int(p['runs']),
                        idx=i)
                func2loop.setdefault(function, []).append(i)

    # figure out loop nesting
    with open(config.graph_profile) as graph:
        for i, row in enumerate(graph):
            # row for function
            if i not in loops:
                continue

            for j, time in enumerate(row.split()):
                isnan = (time == 'nan')
                # filter out loops not called by `loops[i]`
                if isnan or j not in loops or float(time) <= 0:
                    continue

                loops[i].nested.append(j)

    return loops, func2loop


# in the presence of cycles, this assign arbitrary ordering
# to portions of the loops that are cyclic
def topological_sort(loops):
    sortedloops = []
    visited = set()

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

    return sortedloops


def find_candidate_loops(loops):
    candidates = []
    sortedloops = topological_sort(loops)
    # loops that are disqualified to be candidates
    disqualified = set()

    for i in sortedloops:
        if i in disqualified:
            continue
        loop = loops[i]
        if loop.time >= TUNING_LOWERBOUND and loop.time <= TUNING_UPPERBOUND:
            candidates.append(loop)
            disqualified.update(loop.nested)

    return candidates

# exec a shell command
def call(cmd):
    print bcolors.OKGREEN, '----------------', cmd, bcolors.ENDC
    subprocess.check_output(cmd, shell=True)

# get a temporary file
def get_temp():
    dir = mkdtemp()
    return os.path.join(dir, 'tempfile')

# delete a temporary file
def delete_temp(filename):
    call('rm -rf '+os.path.dirname(filename))

# helper function to call `./autotune -makefile=[makefile] [bc]`
# return the optimization sequence
def tune(bc, makefile, obj_var, using_server=False):
    call('{tunerpath}/bin/autotune -passes={tunerpath}/opts.txt -makefile={makefile} -obj-var={obj_var} -server={using_server} {bc}'.format(
        tunerpath=config.tunerpath,
        makefile=makefile,
        obj_var=obj_var,
        using_server=using_server,
        bc=bc))
    with open(bc+'.passes') as result:
        passes = result.read().strip()
    return passes

# generate a temporary makefile that extends `orig_makefile` with
# the ability to compile and link the extracted modules
# 
# in the case of tuning using a replay-server,
# extend the makefile to build a shared library
#
# return makefile,
#   the mapping from <extracted module> -> <its makefile variable>,
#   and path of the main shared library,
def gen_makefile(extracted_modules, orig_makefile):
    tempfile = get_temp()
    with open(tempfile, 'wb') as makefile:
        print >>makefile, 'include', orig_makefile

        # mapping from <extracted module> -> <its makefile variable>
        vars = {}
        for i, bc in enumerate(extracted_modules):
            var = 'VAR_%d' % i
            vars[bc] = var
            obj = re.sub(r'\.bc$', '.o', bc)
            optbc = call('opt %s -o - -O3 | llc -o %s -filetype=obj'% (bc, obj))
            print >>makefile, var, ':=', obj

        # list of variable for object files
        deps = ' '.join('$(%s)'% v for v in vars.itervalues())
        deps_without_globals  = ' '.join('$(%s)'% vars[m] for m in extracted_modules[1:])

        print >>makefile, 'OBJ :=', deps

        main_lib = './main-lib.so'

        # build shared library in case of using a tuning server
        print >>makefile, '$(LIB) :', main_lib, deps_without_globals
        print >>makefile, '\tcc -shared -o $(LIB) %s %s' % (main_lib, deps_without_globals)

    return tempfile, vars, main_lib 


# optimize each module with its optimization sequence and link them together
def link(modules, seqs, out_filename):
    objs = []
    for i, m in enumerate(modules):
        tempfile = get_temp()
        call('opt {bc} -o - {passes} | llc -filetype=obj -o {obj}'.format(
            bc=m, passes=seqs[i], obj=tempfile))
        objs.append(tempfile)

    call('ld -r %s -o %s'% (' '.join(objs), out_filename))

    for m in objs:
        delete_temp(m)

# given all the extracted modules (with the first one being the "main" module)
# shared library and the extracted top level loop one wants to tune (a function),
# build a replay-server
def create_server(server_lib, modules, func, invos):
    main = extracted_modules[0]
    server_bc = re.sub(r'\.bc$', '.server.bc', main)
    server_obj = re.sub(r'\.bc$', '.server.o', main)
    server_exe = re.sub(r'\.bc$', '.server.exe', main)
    server_runtime = '%s/obj/server.bc' % config.tunerpath

    extra_lib = '-lrt' if sys.platform != 'darwin' else ''

    # instrument the main module
    invo_args = ' '.join('-inv%d' % invo for invo in invos)
    call('{tunerpath}/bin/create-server {main} -f{func} {invos} -o {server_bc}'.format(
        tunerpath=config.tunerpath,
        main=main,
        server_bc=server_bc,
        func=func,
        invos=invo_args))
    call('llvm-link {main} {others} {runtime} -o - | opt -O3 -o - | llc -filetype=obj -relocation-model=pic -o {out}'.format(
        main=server_bc,
        others=' '.join(modules[1:]),
        runtime=server_runtime,
        out=server_obj))

    # build the shared library
    call('cc -shared %s %s -o %s' % (server_obj, extra_lib, server_lib))

    # build the server executable
    call('cc {lib} -o {server_exe} -ldl {extra_lib}'.format(
        lib=server_lib,
        server_exe=server_exe,
        extra_lib=extra_lib))

    return server_exe


# "client" for extract-loops tool
# return a list of extracted modules (with the first one being the globals module and the second one being the main modules)
# and a mapping from extracted modules to its top-level extracted loop (a function)
def extract(module, candidates):
    call('{tunerpath}/bin/extract-loops {module} -p extracted {loops}'.format(
        tunerpath=config.tunerpath,
        module=module,
        loops=' '.join('-l%s,%s' % (l.function, l.header_id) for l in candidates)))
    extracted_modules = []
    extracted_loops = {}
    with open('extracted.list') as extraction_out:
        # main module is in the first line
        extracted_modules.append(next(extraction_out).strip())

        for line in extraction_out:
            extracted_func, func, header_id, m = line.strip().split()
            extracted_modules.append(m)
            extracted_loops[m] = {
                    'extracted_func': extracted_func,
                    'func': func,
                    'header_id': header_id
                    }

    return extracted_modules, extracted_loops

# given a list of elapsed time (indexed by invocation numbers)
# return list of representative invocations and their weights
def find_clusters(elapsed):
    num_invos = len(elapsed)
    # list of invocations
    invos = range(num_invos)
    
    # in case the dataset gets too large for the clustering algorithm,
    # randomly choose a subset of the invocations to cluster
    if num_invos > MAX_INVOS:
        invos = np.random.choice(num_invos, MAX_INVOS)
        elapsed = [elapsed[i] for i in invos]
        num_invos = MAX_INVOS

    elapsed = np.reshape(elapsed, (num_invos, 1))
    invos = np.array(invos)

    clusterer = sklearn.cluster.DBSCAN(eps=0.3, min_samples=max(1, num_invos/1000))
    clusterer.fit(sklearn.preprocessing.scale(elapsed))
    labels = clusterer.labels_
    clusters = set(l for l in labels if l >= 0)

    representatives = []
    weights = []
    for cluster in clusters:
        invos_ = invos[labels == cluster]
        elapsed_ = elapsed[labels == cluster]
        mean = np.mean(elapsed_)
        rep_idx = np.argmin(np.abs(elapsed_ - mean))
        rep = invos_[rep_idx]
        weight = np.sum(elapsed_) / elapsed_[rep_idx][0]
        representatives.append(rep)
        weights.append(weight)
    return representatives, weights


# return a set of a invocations representative of the functions
def select_invos(this, modules, func, provided_makefile):
    instrumented = get_temp()
    exe = get_temp()
    obj = get_temp()

    # do the second profiling run
    call('{tunerpath}/bin/instrument-invos {input} -o {output} -f{func}'.format(
        tunerpath=config.tunerpath,
        input=this,
        output=instrumented,
        func=func))

    call('llvm-link {this} {others} {tunerpath}/obj/invos.bc -o - |\
            llc -filetype=obj -o {obj}'.format(
                tunerpath=config.tunerpath,
                this=instrumented,
                others=' '.join(m for m in modules if m != this ),
                obj=obj,
                func=func))

    call('make -f{provided} OBJ={obj} EXE={exe} run'.format(
        provided=provided_makefile,
        obj=obj,
        exe=os.path.abspath(exe)))

    delete_temp(obj)
    delete_temp(exe)
    delete_temp(instrumented)

    def parse_invo(line):
        fields = line.strip().split()
        return float(fields[0]), map(float, fields[1:])

    with open('invocations.txt') as prof_out:
        elapsed = map(float, prof_out.read().strip().split())
    
    print 'clustering invocations'
    return find_clusters(elapsed)

default_config = dict(
    tunerpath='.',
    flat_profile='loop-prof.flat.csv',
    graph_profile='loop-prof.graph.csv',
    makefile='provided.mak')

def get_config(): 
    arg_parser = argparse.ArgumentParser()
    arg_parser.add_argument("input", help="llvm bitcode file to tune") 
    arg_parser.add_argument("--tunerpath",
            default=default_config['tunerpath'],
            help="path to llvmtuner")
    arg_parser.add_argument("--graph_profile",
            default=default_config['graph_profile'],
            help="path to loop-prof.graph.csv")
    arg_parser.add_argument("--flat_profile",
            default=default_config['flat_profile'],
            help="path to loop-prof.flat.csv")
    arg_parser.add_argument("--makefile",
            default=default_config['makefile'],
            help="path to makefile")
    return arg_parser.parse_args()

if __name__ == '__main__':
    config = get_config()

    loops, func2loop = get_loops()
    candidates = find_candidate_loops(loops)
    
    # now extract candidate loops
    provided_makefile = config.makefile
    provided_bc = config.input
    extracted_modules, extracted_loops = extract(provided_bc, candidates)

    print 'extracted module(s):', ' '.join(extracted_modules[1:])

    makefile, vars, main_lib = gen_makefile(extracted_modules, provided_makefile)
    
    # tune loops one at a time
    seqs = ['-O3']
    for m in extracted_modules[1:]:
        loop = extracted_loops[m]

        #invos, weights = select_invos(m, extracted_modules, loop['extracted_func'], provided_makefile)
        for l in candidates:
            if l.function == loop['func'] and l.header_id == loop['header_id']:
                num_invos = l.runs
                break
        invos = random.sample(xrange(num_invos), MAX_WORKERS)

        with open('worker-weight.txt', 'w') as weight_file:
            for _ in invos:
                print >>weight_file, 1

        print 'creating server to run %s in %s' % (loop['extracted_func'], m)
        server = create_server(main_lib, extracted_modules, loop['extracted_func'], invos)
    
        server_path = os.path.abspath(server)
    
        print 'spawning workers'
        call('make -f%s EXE=%s run' % (provided_makefile, server_path))
    
        print 'tuning', m
        seqs.append(tune(m, makefile, obj_var=vars[m], using_server=True))
    
    optimized = re.sub('\.bc', '.opt.o', provided_bc)
    link(extracted_modules, seqs, optimized)
    delete_temp(makefile)
    print 'optimized object file:', optimized
