        #include <linux/module.h>
        #include <linux/kernel.h>
        #include <linux/init.h>
        #include <linux/timer.h>

        static void timer_fun(struct timer_list *t) {
        }

        void dummy(void) {
            struct timer_list tmr;
            timer_setup(&tmr, timer_fun, 0);
        }
