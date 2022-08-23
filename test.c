#include <stdio.h>


int main(int argc, char *argv[])
{
    struct sdshdr
    {
        long len;
        long free;
        char buf[];
    };
    int a=3;
    (void)(a);

    printf("%u", sizeof( a));
    return 0;
}
