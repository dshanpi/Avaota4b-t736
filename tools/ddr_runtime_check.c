// SPDX-License-Identifier: GPL-2.0-only
/* Bounded allocated-RAM check; never maps physical/firmware memory. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
int main(void){
 const size_t bytes=256u*1024u*1024u,n=bytes/8;
 volatile uint64_t *p=mmap(0,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(p==MAP_FAILED){perror("mmap");return 1;}
 const uint64_t masks[]={0,UINT64_MAX,UINT64_C(0xaaaaaaaaaaaaaaaa),UINT64_C(0x5555555555555555)};
 for(unsigned pass=0;pass<4;pass++){
  for(size_t i=0;i<n;i++)p[i]=(UINT64_C(0x9e3779b97f4a7c15)*i)^masks[pass];
  __asm__ volatile("dsb sy" ::: "memory");
  for(size_t i=0;i<n;i++){uint64_t want=(UINT64_C(0x9e3779b97f4a7c15)*i)^masks[pass],got=p[i];if(got!=want){fprintf(stderr,"FAIL pass=%u offset=%zu want=%016llx got=%016llx\n",pass,i*8,(unsigned long long)want,(unsigned long long)got);return 2;}}
  printf("PASS %u: 256MiB allocated memory verified\n",pass+1);fflush(stdout);
 }
 munmap((void*)p,bytes);puts("DDR_RUNTIME_256M_FOUR_PATTERNS_OK");return 0;
}
