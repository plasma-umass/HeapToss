#include <iostream>
#include <cstdlib>
#include <new>
/* The following variables should end up in the heap:
 *
 * - arg1 
 * - arg2 
 * - arg6 
 * - arg7 
 * - arg8 
 * - arg9 
 * - arg11
 * - arg13
 * - arg14
 * - arg15
 * - arg18
 */


void function1(int* anInt)
{
    *anInt = (*anInt)*7;
}

int function2(int anInt)
{
    return anInt*8;
}

void function3(int ** anInt)
{
	(*(*anInt)) = 5;
}

int main(int argc, char **argv) 
{
	//SCENARIO 0
	//Passing a primitive by value. arg0 should NOT be put on the heap.
	int arg0 = 32;
    arg0 = function2(arg0);

    //SCENARIO 1
	//Passing a local via address. arg1 should be put on the heap.
    int arg1 = 7;
    function1(&arg1);

    //SCENARIO 2
    //Passing a pointer to a local variable. arg2 should be put on the heap. arg3 should NOT.
    int arg2 = 4;
    int *arg3 = &arg2;
    function1(arg3);

    //SCENARIO 3
    //Passing a pointer to a heap address. arg4 should NOT be put on the heap.
    int * arg4 = (int *) malloc(sizeof(int));
    (*arg4) = 3;
    function1(arg4);

    //SCENARIO 4
    //Passing a pointer to a heap address that turns into a local address. arg6 should be put on the heap.
    //arg5 should NOT.
    int * arg5 = (int *) malloc(sizeof(int));
    (*arg5) = 56;
    int arg6 = 5;
    arg5 = &arg6;
    function1(arg5);

    //SCENARIO 5
    //Passing a pointer by address that points to dynamically allocated memory. arg7 should be put on the heap.
    int * arg7 = (int *) malloc(sizeof(int));
    function3(&arg7);

    //SCENARIO 6
    //Passing a pointer that points to a pointer to a pointer.
    //arg9 and 8 should be put on the heap. arg10 should NOT.
    int arg8 = 34;
    int * arg9 = &arg8;
    int ** arg10 = &arg9;
    function3(arg10);

    //SCENARIO 7
    //Passing a pointer that points to a pointer to a malloc.
    //arg11 should be placed on the heap.
    int * arg11 = (int *) malloc(sizeof(int));
    int ** arg12 = &arg11;
    function3(arg12);

    //SCENARIO 8
    //Passing a pointer that points to a value onto the heap.
    //arg13 and arg14 should be placed on the heap.
    int arg13 = 35;
    int * arg14 = &arg13;
    function3(&arg14);

    //SCENARIO 9
    //Passing a pointer to the address of a local stored in an intermediate value.
    //Only arg15 should be put on the heap.
    int arg15 = 34;
    int * arg16 = &arg15;
    int ** arg17 = (int **) malloc(sizeof(int*));
    (*arg17) = arg16;
    function3(arg17);

    int arg18 = 34;

    //PHI: Simple conditional.
    //arg18 should be placed in the heap.
    if (arg15 > 10)
    {
	function1(&arg18);
    }
    else
    {
	arg18 *= 2;
    }

    int count = 0;
    for (count = 0; count < arg18; count++)
    {
       int blahblah = 34;
       blahblah *= arg15;
       printf("%d\n", blahblah);
       arg15 += 34;
    }


	return 0;
}
