from util import call

class TuningJob(object):
    def __init__(self, makefile, obj_var, run_rule):
        self.makefile = makefile
        self.obj_var = obj_var
        self.run_rule = run_rule

    # make with `self.makefile`
    def make(self, *args, **kwargs):
        if 'OBJ' in kwargs:
            obj = kwargs['OBJ']
            del kwargs['OBJ']
            kwargs[self.obj_var] = obj

        cmd = 'make -f{file} {args}'.format(
                file=self.makefile,
                args=' '.join(list(args) + map(lambda pair: '='.join(pair), kwargs.iteritems())))

        return call(cmd)

    def run(self, exe):
        run_cmd = self.make(self.run_rule, '--just-print', EXE=exe).stdout
        return call(run_cmd)
