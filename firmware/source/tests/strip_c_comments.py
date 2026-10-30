"""Print a C file with /* */ and // comments removed (static checks must not match documentation words)."""
import re
import sys

text = open(sys.argv[1], encoding="utf-8").read()
text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
print(re.sub(r"//[^\n]*", "", text))
