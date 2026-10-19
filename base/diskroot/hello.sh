# a script on the disk, for the monitor: `source /hello.sh`.
#
# the monitor runs no programs, so a script is its builtins with `if`
# and `while` round them, and it says what it found by setting a
# variable, which `set` with only a name prints.

if test -f /welcome.txt
    set FOUND a welcome file
else
    set FOUND no welcome file
end

set N 0
while test $N -lt 3
    set N 3
end

set FOUND
set N
