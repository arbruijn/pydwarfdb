from distutils.core import setup
from distutils.extension import Extension
from Cython.Build import cythonize

extensions = [
	Extension('pydwarfdb', ['pydwarfdb.pyx',
		'src/array.cpp',
		'src/basetype.cpp',
		'src/consttype.cpp',
		'src/dwarfexception.cpp',
		'src/dwarfparser.cpp',
		'src/enum.cpp',
		'src/funcpointer.cpp',
		'src/function.cpp',
		'src/instance.cpp',
		'src/pointer.cpp',
		'src/refbasetype.cpp',
		'src/referencingtype.cpp',
		'src/struct.cpp',
		'src/structured.cpp',
		'src/structuredmember.cpp',
		'src/symbol.cpp',
		'src/symbolmanager.cpp',
		'src/typedef.cpp',
		'src/union.cpp',
		'src/variable.cpp'],
		language='c++',
		extra_compile_args=['-std=c++14'],
		include_dirs = ['src'],
		libraries = ['dwarf', 'elf']
	)
]

setup(
	name='pydwarfdb',
	version='0.1',
	description='Library for working with Dwarf debugging information',
	author='Arne de Bruijn',
	author_email='mail@arnedebruijn.nl',
	#maintainer='Thomas Kittel',
	#maintainer_email='thomas@kittel.it',
	#url='https://.../',
	py_modules=['pydwarfdb'],
	ext_modules = cythonize(extensions),
)
