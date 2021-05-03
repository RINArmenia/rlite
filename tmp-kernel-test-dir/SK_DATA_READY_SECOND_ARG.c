        #include <linux/module.h>
        #include <linux/kernel.h>
        #include <linux/init.h>
        #include <net/sock.h>

        void dummy(void) {
            struct sock *sk = NULL;
            sk->sk_data_ready(sk, 0);
        }
