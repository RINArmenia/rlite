        #include <linux/module.h>
        #include <linux/kernel.h>
        #include <linux/init.h>
        #include <linux/fs.h>
        void dummy(void) {
            struct file_operations *fops = NULL;
            (void)fops->write_iter;
            (void)fops->read_iter;
        }
