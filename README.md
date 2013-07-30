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

*   basic thread synchronization primitives: signal, condition variable;

*   have a thread _join_ another thread until the latter one completes its execution;

*   have a thread _wait for_ another thread until a specified period of time elapses,
    or the latter one completes, whichever occurs first;

*   wrappers over _read(2)_, _write(2)_, _accept(2)_, _sendto(2)_, _recvfrom(2)_ syscalls;

*   relatively good performance and scalability.

Limitations:

*   unlike ironport/shrapnel, cannot scale on 32-bit platforms because of
    virtual address space limit.


Dependencies: [mkushnir/mrkcommon](https://github.com/mkushnir/mrkcommon).

The porject is gradually getting mature.

TODO
====

*   Linux;

*   more testing;

*   more documentation;

*   more functionality;


