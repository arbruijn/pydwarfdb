pydwarfdb
---------

Library for working with Dwarf debugging information in python.

Based on https://github.com/kittel/libdwarfparser by Thomas Kittel.

Example:
```py
sym = pydwarfdb.SymbolManager()
pydwarfdb.DwarfParser.parseDwarfFromFilename(filename, sym)
f = sym.findFunctionByName('main')
print(f.getAddress())
stat = sym.findBaseTypeByName('stat')
print(stat.getByteSize())
print(stat.memberByName('st_size').getMemberLocation())
```
