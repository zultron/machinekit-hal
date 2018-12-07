#!/bin/bash -e

# Be sure these junk files are cleaned up; git -clean won't touch them
cleanup() {
    rm -f pipe-stderr-* pipe-stdout-*
}
trap cleanup EXIT

do_test() {
    INI=$1
    linuxcnc -r $INI
}

echo "**********  Testing python remaps"
do_test test-py.ini
echo
echo "**********  Testing ngc remaps"
do_test test-ngc.ini
