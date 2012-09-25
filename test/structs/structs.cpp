#include <iostream>
#include <cstdlib>
#include <new>
#include <stdlib.h>
typedef struct
{
    int a;
    char b;
    float c;
} foo;

typedef struct
{
    int a;
    foo * afoo;
} foo2;

void workOnFoo(int * anInt)
{
    (*anInt) += 3;
}

int main(int argc, char **argv) 
{
    foo afoo = {3, 'b', 4.53};
    workOnFoo(&(afoo.a));

    printf("%d\n", afoo.a);

    foo anotherFoo = {54, 'd', 3.24};

    foo2 afoo2 = {2, &anotherFoo};
    workOnFoo(&(afoo2.afoo->a));
	return 0;
}
