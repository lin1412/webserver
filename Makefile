#
# Treat warnings as errors. This seems to be the only way to 
# convince some students of the importance of ensuring that
# their code compiles without warnings before starting to debug.
#
# Do not change this line.  We will not use your copy of the Makefile 
# we will use *this* Makefile to run check.py when grading.
#
#CFLAGS=-Wall -O3 -Werror

CFLAGS=-Wall -Werror
LDLIBS=-lpthread

# Use make's default rules
all: sysstatd

sysstatd: sysstatd.c csapp.c

clean:
	rm -f *.o sysstatd

