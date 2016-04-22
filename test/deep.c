#include <stdio.h>

int extAdd(int i)
{
    return i;
}

int main(int argc, char** argv)
{
    int sum = 0;

    for (int i = 0; i < 10; i++)
      sum += extAdd(i);
    for (int j = 0; j < 10; j++)
      sum += extAdd(j);
    for (int k = 0; k < 10; k++)
      sum += extAdd(k);
    for (int l = 0; l < 10; l++)
      sum += extAdd(l);
    for (int m = 0; m < 10; m++)
      sum += extAdd(m);

    printf("deep = %d\n", sum);
    return 0;
}
