/*
 * grab-probe: disposable kernel-level check that an evdev node is really held
 * by another process's EVIOCGRAB. Tries its own EVIOCGRAB once: EBUSY means
 * someone else (TizGB) holds the grab. If the grab unexpectedly succeeds it
 * is released immediately, so the probe never keeps a device.
 * Freestanding ARM build (same constraints as the app), libc.so.6 only.
 * usage: grab-probe /dev/input/eventN    exit 0 = EBUSY (held), 1 = not held, 2 = error
 */
extern int open(const char *, int, ...);
extern int close(int);
extern int ioctl(int, unsigned long, ...);
extern int *__errno_location(void);
extern int printf(const char *, ...);
#define EVIOCGRAB 0x40044590UL
#define EBUSY 16
int main(int argc, char **argv)
{
    if (argc != 2) { printf("usage: grab-probe /dev/input/eventN\n"); return 2; }
    int fd = open(argv[1], 0 | 0x800);
    if (fd < 0) { printf("open %s failed errno=%d\n", argv[1], *__errno_location()); return 2; }
    int on = 1, off = 0;
    int rc = ioctl(fd, EVIOCGRAB, &on);
    int err = rc ? *__errno_location() : 0;
    if (rc == 0) ioctl(fd, EVIOCGRAB, &off);
    close(fd);
    printf("probe %s EVIOCGRAB rc=%d errno=%d -> %s\n", argv[1], rc, err,
           rc && err == EBUSY ? "EBUSY: grab held by another process" : (rc ? "error" : "NOT held (grab succeeded, released)"));
    return rc && err == EBUSY ? 0 : (rc ? 2 : 1);
}
