#!/usr/bin/env python3

"""Audit macOS build products and every directly linked non-system library."""

import argparse
import os
import platform
import re
import subprocess
import sys


SYSTEM_PREFIXES = ( '/System/Library/', '/usr/lib/' )


def command_output( arguments ):
    return subprocess.check_output( arguments, text = True, stderr = subprocess.STDOUT )


def architectures( path ):
    return command_output( [ '/usr/bin/lipo', '-archs', path ] ).strip().split()


def minimum_macos( path ):
    output = command_output( [ '/usr/bin/vtool', '-show-build', path ] )
    match = re.search( r'^\s*minos\s+(\d+\.\d+)', output, re.MULTILINE )
    if match is None:
        raise RuntimeError( '%s has no macOS LC_BUILD_VERSION minimum' % path )
    return match.group( 1 )


def linked_libraries( path ):
    output = command_output( [ '/usr/bin/otool', '-L', path ] )
    return [
        line.strip().split( ' (', 1 )[0]
        for line in output.splitlines()[1:]
        if line.strip()
    ]


def version_tuple( value ):
    return tuple( int( part ) for part in value.split( '.' ) )


def within( prefix, path ):
    return os.path.commonpath( [ os.path.abspath( prefix ), os.path.abspath( path ) ] ) == os.path.abspath( prefix )


def audit( product, dependency_prefix, architecture, required_minimum, checked_libraries ):
    if not os.path.isfile( product ):
        raise RuntimeError( 'build product does not exist: %s' % product )
    product_architectures = architectures( product )
    if architecture not in product_architectures:
        raise RuntimeError( '%s lacks %s architecture (%s)' % (
            product, architecture, ', '.join( product_architectures )
        ) )
    actual_minimum = minimum_macos( product )
    if version_tuple( actual_minimum ) != version_tuple( required_minimum ):
        raise RuntimeError( '%s targets macOS %s, expected %s' % (
            product, actual_minimum, required_minimum
        ) )

    for library in linked_libraries( product ):
        if library.startswith( SYSTEM_PREFIXES ):
            continue
        if library.startswith( '@' ):
            raise RuntimeError( '%s has an unresolved runtime library: %s' % ( product, library ) )
        if not within( dependency_prefix, library ):
            raise RuntimeError( '%s links outside %s: %s' % (
                product, dependency_prefix, library
            ) )
        if not os.path.isfile( library ):
            raise RuntimeError( '%s links a missing runtime library: %s' % ( product, library ) )
        real_library = os.path.realpath( library )
        if real_library not in checked_libraries:
            library_architectures = architectures( real_library )
            if architecture not in library_architectures:
                raise RuntimeError( '%s lacks %s architecture (%s)' % (
                    real_library, architecture, ', '.join( library_architectures )
                ) )
            checked_libraries.add( real_library )

    print( '%s: %s, macOS %s, runtime linkage valid' % (
        product, architecture, actual_minimum
    ) )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument( '--dependency-prefix', default = '/opt/local' )
    parser.add_argument( '--architecture', default = platform.machine() )
    parser.add_argument( '--minimum-macos', default = '11.0' )
    parser.add_argument( 'products', nargs = '+' )
    args = parser.parse_args()

    checked_libraries = set()
    try:
        for product in args.products:
            audit(
                product,
                args.dependency_prefix,
                args.architecture,
                args.minimum_macos,
                checked_libraries,
            )
    except ( OSError, subprocess.CalledProcessError, RuntimeError ) as error:
        print( 'error: %s' % error, file = sys.stderr )
        return 1
    print( 'audited %s products and %s non-system runtime libraries' % (
        len( args.products ), len( checked_libraries )
    ) )
    return 0


if __name__ == '__main__':
    sys.exit( main() )
