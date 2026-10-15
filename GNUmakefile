# Keep the upstream Linux Makefile unchanged for straightforward upstream merges.
ASCOLTO_PLATFORM ?= $(shell uname -s)

ifeq ($(ASCOLTO_PLATFORM),Darwin)
include Makefile.macos
else ifeq ($(ASCOLTO_PLATFORM),Linux)
include Makefile
else
$(error Unsupported platform '$(ASCOLTO_PLATFORM)'; expected Darwin or Linux)
endif
