include ../../Makefrag

########## Package-specific variables
version := 1.29
src-dir = tar-$(version)
build-dir = $(src-dir)/build
tarball = tar-$(version).tar.xz
fetch-url = http://ftp.gnu.org/gnu/tar/$(tarball)
config-opts = --prefix=/usr \
              --bindir=/bin \
              --disable-nls \
              --host=x86_64-ucb-akaros \
              --build=i686-pc-linux-gnu

########## Generic Infrastructure

PHONY := all
all: make

%.tar.xz:
	$(Q)wget $(fetch-url)

akaros-patches := $(sort $(wildcard akaros-patches/$(version)/*))

$(build-dir): $(tarball) $(akaros-patches)
	rm -fr $(src-dir)
	tar -xf $<
	$(Q)mkdir $(build-dir) &&                                                  \
	for i in $(akaros-patches); do                                             \
		(cd $(src-dir) && patch -p1 < ../$$i);                                 \
	done

PHONY += config
$(build-dir)/Makefile config: $(build-dir)
	cd $(build-dir) && env ../configure $(config-opts)

PHONY += make
make: config
	$(Q)$(MAKE) -C $(build-dir)

PHONY += install
install: make
	$(Q)$(MAKE) -C $(build-dir) install-exec DESTDIR=$(KFS_ROOT)

PHONY += clean
clean:
	$(Q)[ -d $(build-dir) ] &&                                                 \
	    [ -f $(build-dir)/Makefile ] &&                                        \
	    $(MAKE) -C $(build-dir) clean || true

PHONY += mrproper
mrproper:
	$(Q)rm -rf $(src-dir)

.PHONY: $(PHONY)
