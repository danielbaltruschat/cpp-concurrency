# Notes

## Memory-leaking lock-free (Treiber) stack

Treiber stacks work through a simple CAS loop on the top of the stack on a ```push``` or ```pop```.

While the lock-free CAS loop is fairly easy to understand, there are still a few nontrivial considerations. 

```compare_exchange_weak``` allows LL/SC or LR/SC instructions to be used on ARM and RISC-V for improved performance since a failure despite the CAS condition being met just means a retry rather than a correctness problem.

The actual structure of the node itself is important - an intrusive node which stores a struct of the data inside with no pointer indirection couples the lifetime of the node to the lifetime of the data. Using pointers to the data inside the node instead allows separation of lifetimes of the data and the node which is often desirable and likely allows the data to be freed earlier.

## Freeing memory

We want to free nodes once they are no longer part of the stack and there are no threads that still hold a pointer to it. There are a few approaches to this problem.

### Counting threads in pop

One way of doing this is to track how many threads are still in the ```pop``` function before deleting nodes. If there are still other threads popping from the stack, append the nodes that needs to be reclaimed onto a free list instead. This has the obvious downside of possible starvation if there is high contention for ```pop```, meaning memory is never or rarely reclaimed. 

### Hazard pointers

This approach tracks which threads still hold a pointer to the object by assigning each thread a hazard pointer, which points to the node they currently hold a pointer to. Each thread's hazard pointer must be scanned before freeing a node in order to avoid freeing a node in use.

Importantly, protecting data to some pointer ```p``` must include first publishing protection, then checking ```p``` is still valid before using the data in ```p```. This is because it is possible for another thread to concurrently delete the data in ```p```.

A large part of the efficiency of hazard pointers comes from the fact that hazard pointers are only written by one thread and only scanned by other threads on an attempt to reclaim memory. This allows for a thread to create and write to hazard pointers cheaply when the hazard pointer is exclusively in its private cache. The expensive fetch is done in one go instead to scan the hazard pointers across all threads.

### Hazard pointer efficiency

The simple hazard pointer implementation is rather inefficient, requiring a scan through the free list on every reclamation attempt and attempting to reclaim every time ```pop``` is called. A Treiber stack implementation with Folly's production-quality hazard pointer implementation was also completed for reference.

## Elimination backoff

The Treiber stack, while providing a stronger progress guarantee than the mutex-based stack, performs poorly in contentious benchmarks in practice as demonstrated in the README of this repository. 

The reason for this is that there is very little parallelisable component to the workload, since each thread is contending for the top of the stack. An optimised mutex implementation therefore enforces sequential accesses of the top of the stack without the CAS failures of the lock-free loop.

A way to mitigate this is **elimination**, where the threads can contend on a separate elimination array if the CAS on the stack fails.

The elimination is based on the idea that a push and a pop cancel out, so a thread pushing and a thread popping can linearize their operations and return without ever touching the actual stack itself.

An additional implementation detail is that the elimination array should have items of size aligned to the cache line size of the hardware. This is to avoid different elements in the array sharing a cache line, which increases bouncing of that cache line between private caches. ```std::hardware_destructive_interference_size``` was used to do this.

The implementation provided in ```elimination_backoff.cpp``` also uses Folly's hazard pointer implementation. It is also not completely accurate to the original paper but the primary concept of elimination arrays was used.

Reference: [Hendler, Shavit, and Yerushalmi, A Scalable Lock-free Stack Algorithm](https://www.cs.bgu.ac.il/~hendlerd/papers/scalable-stack.pdf).

## Timestamped stack

Another approach is to take advantage of the idea that the linearization point of the ```push``` and ```pop``` operations can be moved to any point, as long as the stack semantics are preserved.

Firstly, defining linearisability:

> A system is linearisable if, for all operations, if operation A finishes before operation B starts, B must observe A's effects.

In the Treiber stack, the linearization point is always when the CAS is successful. At this point, all subsequent operations must observe its effects. 

The timestamped stack operates by giving each thread essentially its own private stack that only it pushes onto, with each node timestamped. Several methods were given to generate these timestamps in the paper, but the simplest is just a CAS on a single 64 bit number.

On push, a node is allocated and pushed to the thread's stack. On a pop, all of the stacks are scanned and the largest (most recent) timestamped node is popped and returned. Crucially, this is safe even if a concurrent push then writes a new node to the local stack.

Consider the following scenario:

>T1: A (5) -> B (4) -> C (2)
>
>T2: D (6) -> E (3) -> F (1)
>
If we call ```pop``` on ```T1``` at this point, ```D``` is the node popped from the stack. Now say concurrently ```T2``` pushes a node ```G``` with timestamp 7 to the bottom stack. It is perfectly fine for us to pop ```D``` since we can decide that the ```pop``` linearised before the ```push```, since the operations overlap.

To make it actually possible to pop ```D``` when it is not the head of the stack, we need ```taken``` flags stored on every node so that on a ```pop```, we don't necessarily need to unlink and can just CAS on the ```taken``` flag. The nodes are then unlinked later during this or subsequent operations.

The main issue I found with this algorithm is memory reclamation. Without a garbage collector, this proves difficult. The original paper's implementation does not implement memory reclamation, instead allocating from a cyclic pool which essentially reuses memory once the pool has been exhausted without checking that this is safe. I copied this approach, but would be dubious about using it in a critical production setting.

An interesting optimisation is that the most recent node from the different per-thread stacks need not be chosen. If the timestamp of any node is **greater** than the timestamp assigned to the ```pop```, we can immediately attempt to claim it as our ```pop``` return value. Although it appears contradictory that we can pop a node that was, in physical time, pushed after the pop began, the operations overlapping means we can choose which operation linearises first, meaning we can say that the push happened before the pop. This optimisation is what motivates many of the other timestamping methods, which allow more early terminations this way.

Another interesting point is that the timestamps given to each operation do not have to be unique - two equal timestamps mean that the operations must overlap, so we can choose which operation linearises first. The only constraints on the timestamp are that, if operation A finishes before operation B starts, A's timestamp must be strictly smaller than B's, which is why the CAS is still needed in the CAS-based timestamp.


Reference: [Dodds, Mike and Haas, Andreas and Kirsch, Christoph M., A Scalable, Correct Time-Stamped Stack](https://dl.acm.org/doi/10.1145/2775051.2676963)

## Other resources used

[C++ Concurrency in Action]() by Anthony Williams

