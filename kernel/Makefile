obj-m += rlite.o
rlite-y := ctrl-dev.o io-dev.o utils.o ker-numtables.o bufs.o normal-common.o memtrack.o

obj-m += rlite-shim-loopback.o
rlite-shim-loopback-y := shim-loopback.o

obj-m += rlite-shim-eth.o
rlite-shim-eth-y := shim-eth.o

# PWD must be the kernel/ directory
EXTRA_CFLAGS := -I$(PWD)/../include
EXTRA_CFLAGS += -g -Werror

obj-m += rlite-normal.o
rlite-normal-y := normal.o

obj-m += rlite-shim-udp4.o
rlite-shim-udp4-y := shim-udp4.o

obj-m += rlite-shim-tcp4.o
rlite-shim-tcp4-y := shim-tcp4.o

obj-m += rlite-normal-tiny.o
rlite-normal-tiny-y := normal-tiny.o
CFLAGS_normal-tiny.o = -DIPCPFLAVOUR=-tiny -Drl_addr_t=uint8_t -Drl_seq_t=uint16_t -Drl_pdulen_t=uint16_t -Drl_cepid_t=uint8_t -Drl_qosid_t=uint8_t 
