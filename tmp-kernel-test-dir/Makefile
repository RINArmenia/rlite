ifneq ($(KERNELRELEASE),)
# Each test is compiled as a separate kernel module.
obj-m := HAVE_UDP_READER_QUEUE.o HAVE_TIMER_SETUP.o SK_DATA_READY_SECOND_ARG.o SIGNAL_PENDING_IN_SCHED_SIGNAL.o HAVE_CHRDEV_RW_ITER.o KERNEL_MODULES_OK.o 
else
all:
	make -C /lib/modules/4.4.0-165-generic/build M=$$PWD
endif
