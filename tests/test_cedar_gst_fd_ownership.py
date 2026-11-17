#!/usr/bin/env python3
"""Compile the actual gst-omx allocation function with mock objects and real FDs."""
import argparse
from pathlib import Path
import subprocess
import tempfile

PRELUDE = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
typedef int gint;
typedef struct { int type, fd; } GstMemory;
typedef struct { GstMemory base; GstMemory *foreign_mem; } GstOMXMemory;
typedef struct { int nShareBufFd; } OmxPrivateBuffer;
typedef struct { void *pBuffer; size_t nAllocLen; } OMX;
typedef struct { OMX *omx_buf; } GstOMXBuffer;
typedef struct { void *data[1]; } Array;
typedef struct { Array *buffers; } Port;
typedef struct { Port *port; Array *memories; int n_memories,foreign_mode; void *foreign_allocator; } GstOMXAllocator;
enum { GST_OMX_ALLOCATOR_FOREIGN_MEM_NONE, GST_OMX_ALLOCATOR_FOREIGN_MEM_DMABUF, GST_OMX_ALLOCATOR_FOREIGN_MEM_OTHER_POOL };
#define g_return_val_if_fail(c,v) do { if (!(c)) return (v); } while(0)
#define g_ptr_array_index(a,i) ((a)->data[i])
#define GST_ERROR_OBJECT(...) ((void)0)
#define GST_MINI_OBJECT(x) (x)
#define GST_OMX_MEMORY_QUARK 0
#define gst_mini_object_set_qdata(...) ((void)0)
#define g_assert_not_reached() abort()
static int wrappers, allocation_failure, duplicate_failure, interrupt_once, last_duplicate=-1, duplicate_calls;
static GstOMXMemory *gst_omx_memory_new(GstOMXAllocator *a,GstOMXBuffer *b,int flags,void *parent,int off,int size) {
 GstOMXMemory *m=calloc(1,sizeof(*m)); m->base.type=1; wrappers++; return m;
}
static void gst_memory_unref(GstMemory *m) {
 if(m->type==1) { GstOMXMemory *w=(void *)m; if(w->foreign_mem)gst_memory_unref(w->foreign_mem); wrappers--; }
 else { assert(m->type==2); assert(close(m->fd)==0); }
 free(m);
}
static GstMemory *gst_dmabuf_allocator_alloc(void *allocator,int fd,size_t size) {
 if(allocation_failure)return NULL;
 GstMemory *m=calloc(1,sizeof(*m));m->type=2;m->fd=fd;return m;
}
static void install_mem_dispose(GstOMXMemory *m) {}
static int test_fcntl(int fd,int command,int arg) {
 duplicate_calls++;
 if(interrupt_once) { interrupt_once=0;errno=EINTR;return -1; }
 if(duplicate_failure) { errno=EMFILE;return -1; }
 last_duplicate=fcntl(fd,command,arg);return last_duplicate;
}
#define fcntl test_fcntl
'''
TEST = r'''
#undef fcntl
int main(void) {
 FILE *input=tmpfile(); assert(input); int borrowed=fileno(input);
 OmxPrivateBuffer private={borrowed}; OMX omx={&private,4096}; GstOMXBuffer buffer={&omx};
 Array buffers={{&buffer}}, memories={{NULL}}; Port port={&buffers};
 GstOMXAllocator allocator={&port,&memories,1,GST_OMX_ALLOCATOR_FOREIGN_MEM_DMABUF,NULL};
 GstMemory *m;
 duplicate_failure=1;
 assert(!gst_omx_allocator_allocate(&allocator,0,NULL));
 assert(!wrappers && !memories.data[0] && fcntl(borrowed,F_GETFD)>=0);
 duplicate_failure=0;allocation_failure=1;
 assert(!gst_omx_allocator_allocate(&allocator,0,NULL));
 assert(!wrappers && !memories.data[0] && fcntl(borrowed,F_GETFD)>=0);
 assert(fcntl(last_duplicate,F_GETFD)==-1 && errno==EBADF);
 allocation_failure=0;interrupt_once=1;duplicate_calls=0;
 m=gst_omx_allocator_allocate(&allocator,0,NULL);
 assert(m && m->fd!=borrowed && duplicate_calls==2);
 assert(fcntl(m->fd,F_GETFD)&FD_CLOEXEC);
 gst_memory_unref(memories.data[0]);memories.data[0]=NULL;
 assert(!wrappers && fcntl(borrowed,F_GETFD)>=0);
 assert(fcntl(last_duplicate,F_GETFD)==-1 && errno==EBADF);
 private.nShareBufFd=-1;
 assert(!gst_omx_allocator_allocate(&allocator,0,NULL));
 assert(!wrappers && !memories.data[0] && fcntl(borrowed,F_GETFD)>=0);
 fclose(input);
 puts("PASS: actual gst-omx function preserves borrowed fd, owns CLOEXEC duplicate, and cleans all failure paths");
 return 0;
}
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('source', type=Path)
    args = ap.parse_args()
    source = args.source.read_text()
    start = source.index('GstMemory *\ngst_omx_allocator_allocate (')
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    with tempfile.TemporaryDirectory(prefix='cedar-gst-fd-') as folder:
        c = Path(folder) / 'test.c'
        exe = Path(folder) / 'test'
        c.write_text(PRELUDE + source[start:end] + TEST)
        subprocess.run(['cc', '-O1', '-g', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', str(c), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
