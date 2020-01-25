mnthr
======

User-level threading library inspired by [ironport/shrapnel](https://github.com/ironport/shrapnel).

Based on standard POSIX _ucontext.h_ interface and the concept of
coroutines. Implements event-driven thread context switch using the
_[libev](http://software.schmorp.de/pkg/libev.html)_ -based poller.  On
FreeBSD, where it was originally developed, the _kqueue(2)_ -based poller
can be used as an alternative to _libev_.

Features:

*   basic threads management: create and schedule threads for run, request thread's
    interruption;
    
*   FreeBSD only: x86 _rdtsc_-based internal clock, calibrated by
    _gettimeofday(2)_ and the _machdep.tsc\_freq_ sysctl;

*   basic thread synchronization primitives: signal, semaphore, condition variable, reader-writrer lock;

*   have a thread _join_ another thread until the latter one completes its execution;

*   have a thread _wait for_ another thread until a specified period of time elapses,
    or the latter one completes, whichever occurs first;

*   wrappers over _read(2)_, _write(2)_, _accept(2)_, _sendto(2)_, _recvfrom(2)_ syscalls;

*   relatively good performance and scalability, derived from the
    underlying _libevent_ and _ucontext_ features.

Limitations:

*   unlike ironport/shrapnel, cannot scale on 32-bit platforms because of
    virtual address space limit.


Dependencies: [mkushnir/mncommon](https://github.com/mkushnir/mncommon).

The porject is gradually getting mature.

TODO
====

*   more testing;

*   more documentation;

*   more functionality;


Misc
====

https://github.com/creationix/libco
