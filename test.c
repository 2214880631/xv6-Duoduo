#include "types.h"
#include "stat.h"
#include "user.h"
#include "mmu.h"

int
main(int argc, char*argv[]){
  char * addr = sbrk(0);
  sbrk(PGSIZE);
  *addr = 100;
  mprotect(addr,1);
  int z = fork();
  if(z == 0){
    printf(1,"protect: %d \n", *addr);
    munprotect(addr, 1);
    *addr = 10;
    printf(1,"unprotect: %d \n", *addr);
    exit();
  }
  else if(z>0){
    wait();
    printf(1,"trap \n");
    *addr = 10;
    exit();
  }
  exit();
}
