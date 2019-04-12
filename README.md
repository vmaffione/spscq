## Fast Single Producer Single Consumer queues

This repository contains the prototype implementation of six Single Producer
Single Consumer queues, together with a program to test their performance
(`spscq`) and some example applications (`fan` and `vswitch`).
The `fan` application requires netmap (https://github.com/luigirizzo/netmap),
unless it is compiled without the `WITH_NETMAP` definition.

Build the software with

    $ make

Then you can run

    $ ./spscq -h

to see the test options of the benchmark program.

Supported queues:

* LQ: original Lamport queue
* LLQ: lazy Lamport queue
* BLQ: batched Lamport queue
* FFQ: FastForward queue
* IFFQ: Improved FastForward queue
* BIFFQ: Batched IFFQ

The `spscq.h` header file contains an inline implementation of all the queues
above, and can be easily used by any C/C++ application.

Scientific paper:
  https://onlinelibrary.wiley.com/doi/full/10.1002/spe.2675
