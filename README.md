HeapToss
--------
HeapToss is an LLVM compiler pass that identifies stack variables that may escape the declaring function's context, and moves them ("tosses" them) into the heap. This can happen in many ways, such as:

* Passing a pointer or reference to that variable to a function call
* Writing a pointer or reference to that variable to a globally-accessible location (the heap, global variables, etc).
* Writing a pointer or reference to that variable into another structure that escapes.

It batches tosses, so it will calls ```malloc``` and ```free``` at most once per function call. Batch tossing can be toggled with a macro or a parameter to ```opt```.

Note that we currently do not support calls to the ```alloca``` function, which dynamically allocates variables on the stack.

Prerequisites
=============
You must have the following installed:
* LLVM and Clang 3.1, both source and object files
* Autoconf

Compilation
===========
1. ```./autogen.sh``` (Which specifies to HeapToss where LLVM is)
2. ```./configure```
3. ```make```

```libHeapToss``` will now be in the folder ```[Release|Debug](+Asserts)?/lib```, with the base folder depending on how you compiled LLVM initially (default is ```Release+Asserts```).

Note that there is also a runtime library used for stat collection called ```libHeapToss```. I will be ripping this out soon, as it complicates the implementation.

Using HeapToss
==============
With ```clang``` or ```clang++```:
```[clang|clang++] -O1 -Xclang -load -Xclang /path/to/HeapTossPass.so```

Note that you must specify ```-O1``` or else HeapToss will not run. I hope that this can be rectified in the future (e.g. if I can figure out how to pass arguments to the plugin through ```clang```).
