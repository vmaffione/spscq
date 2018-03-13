## Fast Single Producer Single Consumer queues

This repository contains the prototype implementation of six Single Producer
Single Consumer queues, together with a program to test their performance.

Build the software with

    $ make

Then you can run

    $ ./spscq -h

to see the test options.

Supported queues:

* LQ: original Lamport queue
* LLQ: lazy Lamport queue
* BLQ: batched Lamport queue
* FFQ: FastForward queue
* IFFQ: Improved FastForward queue
* BIFFQ: Batched IFFQ
