import sys
import coro

nthreads = 20000
niter = 1
nrecur = 0
total = 0.
ntotal = 0
wt = 100

def r(n):
    if n >= nrecur:
        for i in xrange(niter):
            #coro.sleep_relative(1)
            #print 123
            now1 = coro.get_usec()
            coro.sleep_relative(0.0)
            now2 = coro.get_usec()
            #sys.stdout.write('.')
            #sys.stdout.flush()
            #print(now2-now1)
    else:
        r(n+1)

def baz():
    global total, ntotal
    n1 = coro.get_usec()
    r(0)
#    for i in xrange(30000):
#        #coro.sleep_relative(1)
#        #print 123
#        #now1 = coro.now_usec
#        coro.sleep_relative(0)
#        #now2 = coro.now_usec
#        #print(now2-now1)

    n2 = coro.get_usec()
    t = n2 - n1
    ntotal += 1
    total += t
    #print >>sys.stderr, "total", n2-n1

def bar():
    oldtotal = total
    for i in xrange(nthreads):
        coro.spawn(baz)
    print "All started"
    while True:
        coro.sleep_relative(wt/1000.)
        if not ntotal:
            continue
        print "total", total / ntotal
        if oldtotal != 0. and total == oldtotal:
            #pass
            break
        oldtotal = total
    coro.set_exit(0)

if __name__ == '__main__':
    coro.spawn(bar)
    coro.event_loop()
