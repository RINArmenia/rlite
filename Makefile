FINDCMD=-type f -and \( -name "*.c" -or -name "*.h" -or -name "*.cpp" -or -name "*.hpp" -or -name "*.py" \) | grep -v "\.mod\.c" | xargs wc -l
KERNMODDIR=/home/rlite/kernel
KERNBUILDDIR=/lib/modules/4.4.0-165-generic/build

all: usr ker

usr:
	cd build &&  $(MAKE)

ker:
	 $(MAKE) -C $(KERNBUILDDIR) M=$(KERNMODDIR) PWD=$(shell pwd)/kernel \
			WITH_SHIM_UDP4=y \
			WITH_SHIM_TCP4=y \
			modules

test: usr
	cd build && $(MAKE) test

usr_count:
	find common user include $(FINDCMD)

ker_count:
	find common kernel $(FINDCMD)

stat count:
	cloc . || (find common kernel user include $(FINDCMD); echo "install 'cloc' for better code statistics")

clean: usr_clean ker_clean

usr_clean:
	cd build && $(MAKE) clean

ker_clean:
	$(MAKE) -C $(KERNBUILDDIR) M=$(KERNMODDIR) clean

install: usr_install ker_install

usr_install:
	cd build && $(MAKE) install

ker_install: ker
	$(MAKE) -C $(KERNBUILDDIR) INSTALL_MOD_PATH=/ M=$(KERNMODDIR) modules_install

depmod: ker_install
	depmod -b / -a

format:
	clang-format -i -style=file $(shell git ls-files *.c *.h *.cpp *.hpp | grep -v wpa-supplicant)
	clang-format -i -style=google $(shell git ls-files *.proto)

intest:
	tests/run-integration-tests.sh
