#!/usr/bin/env python
import sys
import os
import socket
import struct
from argparse import ArgumentParser

sizeof_float = len(struct.pack('f', 42.0))

RESP_SIZE = 201
ERROR = '\0'
SUCCESS = '\1'

def get_socket(sockpath):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) 
    sock.connect(sockpath)
    return sock


def kill_worker(sock):
    req = struct.pack('c', '\0')
    sock.sendall(req)


def run(sock, lib):
    req = struct.pack('c'*(len(lib)+1), '\1', *lib)
    sock.sendall(req)
    resp = sock.recv(RESP_SIZE)

    if resp[0] == ERROR:
        print >>sys.stderr, resp[1:]
        sys.exit(-1)

    body = resp[1:sizeof_float+1]
    return struct.unpack('f', body)[0]


def main(args): 
    with open(args.worker_file) as workers:
        for worker in workers:
            func, sockpath = worker.split()
            if func == args.func:
                break
        else:
            print >>sys.stderr, 'unknown function', func
            sys.exit(-1) 

    sock = get_socket(sockpath) 
    if args.kill:
        kill_worker(sock)
    else:
        lib = os.path.join(os.getcwd(), args.library_path)
        time_spent = run(sock, lib)
        print time_spent


if __name__ == '__main__':
    parser = ArgumentParser()
    parser.add_argument('func', help='function to run')
    parser.add_argument('-l', '--library-path',
            help='path to shared library contianing implementation of `func`')
    parser.add_argument('-w', '--worker-file',
            help='path to file listing worker data',
            default='worker-data.txt')
    parser.add_argument('-k', '--kill',
            help='kill worker responsible for running the library',
            action='store_true')
    args = parser.parse_args()
    if not args.kill and args.library_path is None: 
        parser.error('must specify path `library_path` to run')
    main(args)
