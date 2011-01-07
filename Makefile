#
# Makefile for htb and tbf modules
#

obj-m = sch_tbf.o sch_htb.o

KVERSION=$(shell uname -r)

all:
	make -C /lib/modules/$(KVERSION)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(KVERSION)/build M=$(PWD) clean
