#!/velvet
# a script on the disk, run by typing its name.
#
# the #! line says what this is. there is no /bin/sh to name after it --
# the shell is a kernel thread rather than a program -- so what follows
# is read and ignored. the line is there so the file says what it is,
# and this machine has only one answer to give.

echo the bond endures

if test -f /welcome.txt
    echo and there is a welcome file
else
    echo but there is no welcome file
end

set N 0
while test $N -lt 3
    echo counting
    set N 3
end
