# Out-of-tree module build. From the Mac:  scripts/vm.sh make -C driver
obj-m := svmd.o
svmd-y := svmd_drv.o svmd_ring.o svmd_sched.o svmd_mmu.o svmd_svm.o \
	  svmd_fault.o svmd_selftest.o svmd_debugfs.o svmd_copy.o svmd_vram.o
SVMD_INCLUDE ?= $(src)/../include
ccflags-y := -I$(SVMD_INCLUDE) -I$(src)

KDIR ?= $(HOME)/gpu-svm/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
