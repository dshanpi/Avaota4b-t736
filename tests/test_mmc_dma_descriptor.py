#!/usr/bin/env python3
"""Compile the actual v18 descriptor code and exercise unsafe boundaries."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint32_t __le32;
#define BIT(n) (1U << (n))
#define DIV_ROUND_UP(n,d) (((n)+(d)-1)/(d))
#define min(a,b) ((a)<(b)?(a):(b))
#define cpu_to_le32(v) ((uint32_t)(v))
#define le32_to_cpu(v) ((uint32_t)(v))
#define READ_ONCE(v) (v)
#define dma_rmb() __asm__ volatile("" ::: "memory")
#include "t736_mmc_dma_desc.h"
int main(void) {
 struct t736_dma_desc d[T736_DMA_DESCS + 1], before[T736_DMA_DESCS + 1];
 unsigned sizes[] = {4, 8, 512, 1024, 1028, 2048, 65532, 65536};
 for (unsigned k=0; k<sizeof(sizes)/sizeof(sizes[0]); k++) {
  unsigned bytes=sizes[k], total=0;
  memset(d, 0xa5, sizeof(d));
  int n=t736_dma_describe(d, 0xfffffc00ULL, 0x40000000ULL, bytes);
  assert(n == (int)DIV_ROUND_UP(bytes, T736_DMA_CHUNK));
  assert(d[n].flags == 0xa5a5a5a5);
  for (int i=0; i<n; i++) {
   assert(d[i].length > 0 && d[i].length <= 1024);
   assert(d[i].buffer == (0x40000000ULL + total) >> 2);
   assert(d[i].next == (i == n-1 ? 0 : (0xfffffc00ULL+(i+1)*16) >> 2));
   assert(!!(d[i].flags&T736_DESC_FIRST) == (i == 0));
   assert(!!(d[i].flags&T736_DESC_LAST) == (i == n-1));
   assert(!(d[i].flags & ~(T736_DESC_OWN|T736_DESC_FIRST|T736_DESC_LAST)));
   total+=d[i].length;
  }
  assert(total == bytes);
  assert(t736_dma_descriptors_done(d,n) == 0);
  for (int i=0; i<n; i++) d[i].flags &= ~T736_DESC_OWN;
  assert(t736_dma_descriptors_done(d,n) == 1);
  d[0].flags |= T736_DESC_OWN;
  assert(t736_dma_descriptors_done(d,n) == 0);
 }
 struct {u64 desc, buf; unsigned bytes;} bad[] = {
  {0x40000000,0x40100000,0},{0x40000000,0x40100000,65540},
  {0x40000000,0x40100000,3},{0x40000004,0x40100000,512},
  {0x40000000,0x40100001,512},{0xfffffc10ULL,0x40100000,512},
  {0x40000000,0xffffff00ULL,512},{0x100000000ULL,0x40100000,512},
  {0x40000000,0x100000000ULL,512}, {UINT64_MAX,UINT64_MAX,512}
 };
 for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
  memset(d,0xa5,sizeof(d)); memcpy(before,d,sizeof(d));
  assert(t736_dma_describe(d,bad[i].desc,bad[i].buf,bad[i].bytes) == -EINVAL);
  assert(!memcmp(before,d,sizeof(d)));
 }
 assert(t736_dma_describe(d,0x40000000,0xffff0000ULL,65536)==64);
 for (int i=0;i<64;i++) d[i].flags &= ~T736_DESC_OWN;
 assert(t736_dma_poll_result(BIT(2)|BIT(3),BIT(1),true,false,false,d,64)==1);
 assert(t736_dma_poll_result(BIT(2)|BIT(3),BIT(0),true,false,false,d,64)==0);
 assert(t736_dma_poll_result(BIT(2),BIT(1),true,false,false,d,64)==0);
 assert(t736_dma_poll_result(BIT(3),BIT(1),true,false,false,d,64)==0);
 assert(t736_dma_poll_result(BIT(2)|BIT(3),BIT(0),true,true,true,d,64)==0);
 assert(t736_dma_poll_result(BIT(2)|BIT(14),BIT(0),true,true,true,d,64)==1);
 assert(t736_dma_poll_result(BIT(2)|BIT(14)|BIT(13),BIT(0),true,true,true,d,64)==1);
 assert(t736_dma_poll_result(BIT(2)|BIT(3)|BIT(13),BIT(1),true,false,false,d,64)==-EILSEQ);
 assert(t736_dma_poll_result(BIT(2)|BIT(14)|BIT(18),BIT(0),true,true,true,d,64)==-ETIMEDOUT);
 assert(t736_dma_poll_result(BIT(2)|BIT(3)|BIT(10),BIT(1),true,false,false,d,64)==-ETIMEDOUT);
 assert(t736_dma_poll_result(BIT(2)|BIT(14),BIT(0)|BIT(4),true,true,true,d,64)==-EIO);
 assert(t736_dma_poll_result(BIT(2)|BIT(14)|BIT(9),BIT(0),true,true,true,d,64)==-ETIMEDOUT);
 assert(t736_dma_poll_result(BIT(2)|BIT(3)|BIT(7),BIT(1),true,false,false,d,64)==-EILSEQ);
 d[20].flags |= T736_DESC_OWN;
 assert(t736_dma_poll_result(BIT(2)|BIT(3),BIT(1),true,false,false,d,64)==0);
 assert(t736_dma_poll_result(BIT(2)|BIT(3),BIT(1)|BIT(5),true,false,false,d,64)==-EIO);
 assert(t736_dma_poll_result(BIT(2),0,false,false,false,NULL,0)==1);
 puts("PASS: actual SMHC3 C descriptor ABI, reserved flag bits zero, low4G ends, alignment/overflow rejection, command/data/directional completion, ownership and error precedence");
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="t736-mmc-dma-test-") as tmp:
        src = Path(tmp) / "test.c"
        exe = Path(tmp) / "test"
        src.write_text(SOURCE, encoding="utf-8")
        subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined,address", "-fno-omit-frame-pointer",
                        "-I", str(ROOT / "boards"), str(src), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
