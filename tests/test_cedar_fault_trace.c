/* Software fault fixture: load only the independent diagnostic preload. */
#include <pthread.h>
#include <stdint.h>
#include <string.h>

__attribute__((noinline)) static void fail_leaf(void)
{ *(volatile uint32_t *)(uintptr_t)0x1234 = 7; }
__attribute__((noinline)) static void fail_middle(void) { fail_leaf(); }
__attribute__((noinline)) static void *fail_thread(void *unused)
{ (void)unused; fail_middle(); return 0; }
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "thread")) {
        pthread_t thread;
        if (pthread_create(&thread, 0, fail_thread, 0)) return 3;
        pthread_join(thread, 0);
    } else fail_middle();
    return 4;
}
