        #include <linux/module.h>
        #include <linux/kernel.h>
        #include <linux/init.h>
        #include <net/sock.h>
        #include <linux/udp.h>

        struct sk_buff_head *dummy(void) {
            struct sock *sk = NULL;
            return &udp_sk(sk)->reader_queue;
        }
