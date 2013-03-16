User-level threading library inspired by [ironport/shrapnel](https://github.com/ironport/shrapnel).

Based on standard POSIX _ucontext.h_ interface and the concept of
coroutines. Implements event-driven thread context switch using the
FreeBSD's _kqueue(2)_ system interface.

Primary development platform: FreeBSD.

Features:

*   basic threads management: create and schedule threads for run, request thread's
    interruption;
    
*   x86 _rdtsc_-based internal clock, calibrated by _gettimeofday(2)_ and
    the _machdep.tsc\_freq_ sysctl;

*   basic thread synchronization primitives: event, condition variable.

*   wrappers over _read(2)_, _write(2)_, _accept(2)_ syscalls.

*   relatively good performance and scalability.

The porject is still in very early stage.

TODO
====

*   Linux;

*   more testing;

*   more documentation;

*   more functionality;


