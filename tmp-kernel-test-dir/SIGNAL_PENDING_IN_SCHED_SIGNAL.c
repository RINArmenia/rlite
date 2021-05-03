        #include <linux/module.h>
        #include <linux/kernel.h>
        #include <linux/init.h>
        #include <linux/sched/signal.h>

        int dummy(void) {
            return signal_pending(NULL);
        }
