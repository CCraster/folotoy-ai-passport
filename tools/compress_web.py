"""确定性压缩内嵌设置页；浏览器直接解压，卡片无需分配解压内存。"""
import gzip
import pathlib
import sys

source, target = map(pathlib.Path, sys.argv[1:])
target.write_bytes(gzip.compress(source.read_bytes(), compresslevel=9, mtime=0))
