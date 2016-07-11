from __future__ import division
from util import *
import signal
import collections
import functools
import math
import autotune
import random
import multiprocessing
import multiprocess
from config import config
import numpy
import re
from copy import copy

from concurrent.futures import ThreadPoolExecutor

class Transform(object):
    @classmethod
    def new(cls, module, job):
        '''
        return a new transformation that does nothing
        '''
        raise NotImplementedError

    def mutate(self):
        '''
        return a slightly mutated mutation
        '''
        raise NotImplementedError

    def apply(self):
        '''
        apply transformation to current module
        '''
        raise NotImplementedError

    def update_module(self):
        ''' 
        update the module with `self.transformation`
        after this, `self.apply` should be a nop

        this is so that Reordering transformation can maintain an intermediate module

        subclass doesn't have to implement this method
        '''
        pass

    def compile(self):
        '''
        apply a transformation to module, compile the transformed module, and
        return name of of the executable file 
        '''
        transformed = self.apply()

        obj = compile_module(transformed)

        exe = get_temp()
        self.job.make(exe, OBJ=obj, EXE=exe)

        delete_temp(obj)
        delete_temp(transformed)
        return exe

    def evaluate(self):
        '''
        evaluate cost of the transformation
        '''
        exe = self.compile()
        r = self.job.run(exe)
        delete_temp(exe)
        return r.elapsed


CodeLayoutTransform = collections.namedtuple('CodeLayoutTransform', ['kind', 'func1', 'func2'])

def transform2args(transform):
    '''
    turn a CodeLayoutTransform into command line argument for bin/reorer-functions
    '''
    return '-t{0},{1},{2}'.format(transform.kind, transform.func1, transform.func2)


# TODO what happens when there's only one function (or two...) in the module?
class Reordering(Transform):
    # (<functions>, <delarations>)
    module_info = None

    # probability to reorder functions
    p_funcs = 0.8

    p_shuffle = 0.01
    p_swap = 0.2

    def __init__(self, reorderings, module, job, using_temp=False):
        self.reorderings = reorderings
        self.module = module
        self.job = job
        self.using_temp = using_temp

    @classmethod
    def get_module_info(cls, module):
        '''
        return { function : number of basic blocks },
                list of each function's probabitlity to be chosen to reorder its basic blocks
        '''
        if cls.module_info is None:
            r = call('{tunerpath}/bin/reorder-functions -list-functions {m}'.format(
                tunerpath=config.tunerpath,
                m=module))
            s = r.stdout.split('\n')[0]
            info = {}
            probs = []
            total_weight = 0
            for pair in s.split(','):
                func, bb_count_number = pair.split('|')
                info[func] = bb_count = int(bb_count_number)
                weight = 0 if bb_count <= 2 else bb_count
                probs.append(weight)
                total_weight += weight

            for i, weight in enumerate(probs):
                probs[i] = weight / total_weight

            cls.module_info = info, probs
        return cls.module_info

    @classmethod
    def rand_reordering(cls, module):
        reorderings = []
        if random.random() < cls.p_funcs:
            reorderings += cls.reorder_functions(module)
        reorderings += cls.reorder_basic_blocks(module)
        return reorderings

    @classmethod
    def reorder_basic_blocks(cls, module):
        module_info, chosen_probs = cls.get_module_info(module)
        func = numpy.random.choice(module_info.keys(), p=chosen_probs)
        num_bbs = module_info[func]
        reorderings = []

        # with some probability swap a basic blocks with the next one
        for i in xrange(1, num_bbs-1):
            if random.random() < cls.p_swap:
                reorderings.append(CodeLayoutTransform('s'+func, str(i), str(i+1)))
            if random.random() < cls.p_shuffle:
                reorderings.append(CodeLayoutTransform('m'+func, str(i), str(choose(range(1, num_bbs), except_=i))))

        return reorderings

    @classmethod
    def reorder_functions(cls, module):
        kind = choose(['s', 'm'])
        functions = cls.get_module_info(module)[0].keys()
        func1 = choose(functions)
        func2 = choose(functions, except_=func1)
        return [CodeLayoutTransform(kind, func1, func2)]

    @classmethod
    def new(cls, module, job):
        return Reordering([], module, job)

    def mutate(self):
        new_reorderings = self.__class__.rand_reordering(self.module)
        return Reordering(self.reorderings + new_reorderings, self.module, self.job, self.using_temp)

    def as_args(self):
        return map(transform2args, self.reorderings)

    def apply(self):
        transformed = get_temp()
        transform_cmd = '{tunerpath}/bin/reorder-functions {old} -o {new} {args}'.format(
                tunerpath=config.tunerpath,
                old=self.module,
                new=transformed,
                args=' '.join(self.as_args()))
        call(transform_cmd)
        return transformed

    def update_module(self):
        transformed = self.apply()
        if self.using_temp:
            delete_temp(self.module)
        self.module = transformed
        self.using_temp = True
        self.reorderings = []


default_num_workers = int(max(1, multiprocessing.cpu_count() / 2))
def tune(module, makefile,
        obj_var='OBJ',
        run_rule='run',
        using_server=False,
        transform_type=Reordering,
        iterations=100,
        num_workers=default_num_workers):

    job = autotune.TuningJob(makefile, obj_var, run_rule)

    init_transform = transform_type.new(module, job)
    cost = best_cost = init_cost = init_transform.evaluate()
    best = new_transform = transform = init_transform

    # hack
    best_module = module
    in_module = module

    # magic
    t_max = -0.01 / math.log(0.8)
    t_min = -0.01 / math.log(0.0001)
    alpha = math.exp(math.log(t_min/t_max) / iterations)

    signal.signal(signal.SIGINT, signal.SIG_IGN)

    #pool = multiprocess.Pool(num_workers)
    pool = ThreadPoolExecutor(num_workers)

    try:
        i = 0
        t = t_max
        while i < iterations:
            # speculatively evaluate the transformations
            # e.g. assume that none of the transformations gets accepted
            candidates = [transform.mutate() for _ in xrange(num_workers)]
            new_costs = pool.map(lambda t: t.evaluate(), candidates)

            skipped = 0
            for new_cost in new_costs:
                normalize = lambda x: x / new_cost
                diff = (normalize(cost) - normalize(new_cost))
                new_transform = candidates[skipped]
                skipped += 1
                if new_cost <= cost or t > t_min and random.random() < math.exp(diff / t):
                    # accept this transformation
                    new_transform.update_module() 
                    transform = new_transform
                    cost = new_cost

                    if best_cost > cost:
                        best_cost = cost
                        best = copy(transform)

                    break


            t *= (alpha ** skipped)
            i += skipped

            print 'cost = {0}, best cost = {1}, init cost = {2}, itr = {3}, t = {4}, alpha = {5}'\
                    .format(cost, best_cost, init_cost, i, t, alpha)

        return best.apply()

    except KeyboardInterrupt:
        pool.terminate()
        pool.join()
        exit()
