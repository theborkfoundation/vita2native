# Attach to a running v2n-arm and stop on the guest's fatal signal.
#
#   PIN=$(pgrep -x v2n-arm | tail -1)
#   gdb -nx -batch -p "$PIN" -x tools/gdb/live-attach.gdb
#
# Live attach is preferred over a core dump: the JIT region shows up as
# memfd:vita2native-jit and is often unreadable afterwards, and a Panfrost
# core is 700-800 MB.
set pagination off
set confirm off
set width 0
handle SIGSEGV stop print nopass
handle SIGILL stop print nopass
handle SIGABRT stop print nopass
continue

printf "\n=== SIGNAL ===\n"
info program
info registers
printf "\n=== pc ===\n"
x/16i $pc-24
printf "\n=== stack (guest return addresses are Thumb BL words) ===\n"
x/64wx $sp
printf "\n=== threads ===\n"
info threads
thread apply all bt 8
printf "\n=== pc as Thumb (guest code; set arm force-mode thumb) ===\n"
set arm force-mode thumb
x/12i $pc-16
