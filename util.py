from tempfile import mkdtemp
from time import time
import subprocess
import collections
import sys
import os
import random

class ExecFailure(Exception): pass

ExecResult = collections.namedtuple('ExecResult', ['elapsed', 'stdout', 'stderr'])

def choose(choice, except_=None):
    while True:
        chosen = random.choice(choice)
        if chosen != except_:
            return chosen


def call(cmd):
    begin = time()
    proc = subprocess.Popen(cmd, shell=True,
            stdout=subprocess.PIPE, 
            stderr=subprocess.PIPE)
    stdout, stderr = proc.communicate()
    elapsed = time() - begin

    if proc.returncode != 0:
        raise ExecFailure(stderr)

    return ExecResult(elapsed, stdout, stderr)


# get a temporary file
def get_temp():
    dir = mkdtemp()
    return os.path.join(dir, 'tempfile')


# delete a temporary file
def delete_temp(filename):
    call('rm -rf '+os.path.dirname(filename))


def compile_module(module):
    obj = get_temp()
    call('llc {m} -filetype=obj -o {obj}'.format(m=module, obj=obj))
    return obj
