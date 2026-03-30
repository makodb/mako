#! /usr/bin/env python3
import sys
import copy
import traceback
import os
import os.path
import tempfile
import subprocess
import itertools
import shutil
import glob
import signal
from threading import Thread
from argparse import ArgumentParser
from logging import info, debug, error
import time

import logging

home="/home/weihai/mako"

def signal_handler(sig, frame):
    sys.exit(0)

signal.signal(signal.SIGINT, signal_handler)

logger = logging.getLogger('')

# shardIndex -> {'p2': 'host', 'p1': 'host', 'learner': 'host', 'localhost': 'host'}
map_ip=[{} for _ in range(3)]

def loader():
    for shardIdx in range(3):
        file="./bash/shard{shardIdx}.config".format(shardIdx=shardIdx)
        for line in open(file, "r").readlines():
            items=[e for e in line.split(" ") if e]
            map_ip[shardIdx][items[0]]=items[1].strip()

def gen_process_cmd(command):
  cmds=[]
  cmds.append("cd " + home + "; ")
  #cmds.append("bash bash/op.sh kill; ")
  s = "nohup " + command + " &"
  cmds.append(s)
  return ' '.join(cmds)

def run_experiments():
  def run_one_server(host, cmd):
    logger.info("starting %s @ %s", host, cmd)
    subprocess.call(['ssh', '-f', host, cmd])
  
  # 2 partitions, 1-30 warehouses
  for trd in range(1,30+1):
    nshards=2
    t_list = []
    for c in range(2):
      LOGFILE="./results/exp1_0_nshard{nshards}_sIdx{c}_trd{trd}.log".format(nshards=nshards,c=c,trd=trd)
      cmd="bash bash/shard{c}.sh {nshards} {c} {trd} > {LOGFILE} 2>&1".format(c=c,nshards=nshards,trd=trd,LOGFILE=LOGFILE)
      cmd=gen_process_cmd(cmd)
      t = Thread(target=run_one_server,
                args=(map_ip[c]["localhost"],cmd))
      t.start()
      t_list.append(t)

    #for t in t_list:
    #  t.join()
    time.sleep(45)

def main():
    logging.basicConfig(format="%(levelname)s : %(message)s")
    logger.setLevel(logging.DEBUG)
    try:
        run_experiments()
    except Exception:
        traceback.print_exc()
    finally:
        os.killpg(0, signal.SIGTERM)


if __name__ == "__main__":
  loader()
  main()