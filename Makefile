#
# Copyright (C) 2005 Martin Decky
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#
# - Redistributions of source code must retain the above copyright
#   notice, this list of conditions and the following disclaimer.
# - Redistributions in binary form must reproduce the above copyright
#   notice, this list of conditions and the following disclaimer in the
#   documentation and/or other materials provided with the distribution.
# - The name of the author may not be used to endorse or promote products
#   derived from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
# IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
# OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
# IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
# INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
# NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
# THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

## Include configuration
#

-include Makefile.config

DIRS = \
	libc \
	softint \
	softfloat \
	init \
	ns \
	fb \
	console

ifeq ($(ARCH), amd64)
	DIRS += pci \
		kbd
endif
ifeq ($(ARCH), ia32)
	DIRS += pci \
		kbd
endif
ifeq ($(ARCH), mips32)
	DIRS += kbd
endif
ifeq ($(ARCH), mips32eb)
	DIRS += kbd
endif
ifeq ($(ARCH), ia64)
	DIRS += kbd
endif

BUILDS := $(addsuffix .build,$(DIRS))
CLEANS := $(addsuffix .clean,$(DIRS))

.PHONY: all config build $(BUILDS) $(CLEANS) clean distclean

all:
	tools/config.py default $(NARCH)
ifdef NARCH
 ifneq ($(ARCH), $(NARCH))
	$(MAKE) -C . clean
 endif
endif
	$(MAKE) -C . build

config:
	tools/config.py

build: $(BUILDS)

clean: $(CLEANS)
	find $(SOURCES) -name '*.o' -follow -exec rm \{\} \;
	find libc -name "_link.ld" -exec rm \{\} \;

distclean: clean
	-rm Makefile.config

$(CLEANS):
	$(MAKE) -C $(basename $@) clean ARCH=$(ARCH)

$(BUILDS):
	$(MAKE) -C $(basename $@) all ARCH=$(ARCH) COMPILER=$(COMPILER)
