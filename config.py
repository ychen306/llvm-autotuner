import argparse

default_config = dict(
    tunerpath='.',
    flat_profile='loop-prof.flat.csv',
    graph_profile='loop-prof.graph.data',
    run_rule='run',
    makefile='provided.mak')

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
arg_parser.add_argument("--run-rule",
        default=default_config['run_rule'],
        help="rule in makefile to run the executable")
config = arg_parser.parse_args()

