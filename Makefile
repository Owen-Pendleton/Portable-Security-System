# Portable Security System. Command contract: see CLAUDE.md.
#   make            build everything this machine can build
#   make test       unit + end-to-end tests
#   make asan       same tests under AddressSanitizer + UBSan
#   make memcheck   tests under valgrind
#   make netcheck   fail if src/ uses any network API (Design.md §11)
#   make deploy PI=pi@<host>   copy the tree to the Pi and build there

CC     ?= cc
BUILD  ?= build
SAN    ?=
CFLAGS  := -std=c17 -D_GNU_SOURCE -Wall -Wextra -Werror -Wvla -O2 -g -Isrc -MMD -MP $(SAN)
LDFLAGS := $(SAN)

# pird needs libgpiod v2 (on the Pi, not the lab machines); micd needs ALSA.
HAVE_GPIOD := $(shell pkg-config --atleast-version=2.0 libgpiod 2>/dev/null && echo y)
HAVE_ALSA  := $(shell pkg-config --exists alsa 2>/dev/null && echo y)

COMMON := common/log common/time common/util common/ipc common/client common/suphb
STORE  := store/crc32 store/dirlist store/evlog store/clip
obj = $(addprefix $(BUILD)/obj/,$(addsuffix .o,$(1)))

BINS := $(BUILD)/hub $(BUILD)/camd $(BUILD)/pss $(BUILD)/supervisor
ifeq ($(HAVE_GPIOD),y)
BINS += $(BUILD)/pird
endif
ifeq ($(HAVE_ALSA),y)
BINS += $(BUILD)/micd
endif

TESTS     := test_store test_ipc test_pir_logic test_button_logic test_features test_fusion test_audio test_arm test_sup_policy test_e2e test_supervisor
TEST_BINS := $(addprefix $(BUILD)/tests/,$(TESTS))
TEST_ENV  := PSS_BIN_DIR=$(abspath $(BUILD)) PSS_TEST_DIR=$(CURDIR)/tests

.PHONY: all test asan memcheck netcheck deploy clean

all: $(BINS)
ifneq ($(HAVE_GPIOD),y)
	@echo "note: libgpiod >= 2.0 not found; pird not built here (it builds on the Pi)"
endif
ifneq ($(HAVE_ALSA),y)
	@echo "note: ALSA not found; micd not built here"
endif

$(BUILD)/obj/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/tests/%.o: tests/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/capture/pir/pird.o: CFLAGS += $(shell pkg-config --cflags libgpiod 2>/dev/null)
$(BUILD)/obj/capture/mic/micd.o: CFLAGS += $(shell pkg-config --cflags alsa 2>/dev/null)

$(BUILD)/hub: $(call obj,hub/hub analysis/fusion analysis/arm analysis/audio $(COMMON) $(STORE))
	$(CC) $(LDFLAGS) $^ -o $@ -lm

$(BUILD)/camd: $(call obj,capture/cam/camd $(COMMON) store/dirlist store/clip)
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD)/pss: $(call obj,cli/pss analysis/arm $(COMMON) $(STORE))
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD)/supervisor: $(call obj,supervisor/supervisor supervisor/policy $(COMMON))
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD)/pird: $(call obj,capture/pir/pird capture/pir/pir_logic capture/pir/button_logic $(COMMON))
	$(CC) $(LDFLAGS) $^ -o $@ $(shell pkg-config --libs libgpiod)

$(BUILD)/micd: $(call obj,capture/mic/micd capture/mic/features $(COMMON))
	$(CC) $(LDFLAGS) $^ -o $@ $(shell pkg-config --libs alsa) -lpthread -lm

$(BUILD)/tests/test_store: $(call obj,tests/test_store $(COMMON) $(STORE))
$(BUILD)/tests/test_ipc: $(call obj,tests/test_ipc $(COMMON))
$(BUILD)/tests/test_pir_logic: $(call obj,tests/test_pir_logic capture/pir/pir_logic $(COMMON))
$(BUILD)/tests/test_button_logic: $(call obj,tests/test_button_logic capture/pir/button_logic $(COMMON))
$(BUILD)/tests/test_features: $(call obj,tests/test_features capture/mic/features $(COMMON))
$(BUILD)/tests/test_fusion: $(call obj,tests/test_fusion analysis/fusion $(COMMON))
$(BUILD)/tests/test_arm: $(call obj,tests/test_arm analysis/arm $(COMMON))
$(BUILD)/tests/test_audio: $(call obj,tests/test_audio analysis/audio $(COMMON))
$(BUILD)/tests/test_e2e: $(call obj,tests/test_e2e analysis/arm $(COMMON) $(STORE))
$(BUILD)/tests/test_sup_policy: $(call obj,tests/test_sup_policy supervisor/policy $(COMMON))
$(BUILD)/tests/test_supervisor: $(call obj,tests/test_supervisor $(COMMON))
$(BUILD)/tests/fake_child: $(call obj,tests/fake_child $(COMMON))
$(TEST_BINS) $(BUILD)/tests/fake_child:
	@mkdir -p $(@D)
	$(CC) $(LDFLAGS) $^ -o $@ -lm

TEST_PROGS := $(BUILD)/hub $(BUILD)/camd $(BUILD)/pss $(BUILD)/supervisor $(BUILD)/tests/fake_child

test: $(TEST_BINS) $(TEST_PROGS)
	@set -e; for t in $(TEST_BINS); do echo "== $$t"; $(TEST_ENV) $$t; done
	@echo "== all tests passed ($(BUILD))"

# The lab machines' gcc lacks libasan; clang has a working runtime there.
ASAN_CC := $(shell command -v clang >/dev/null 2>&1 && echo clang || echo $(CC))
asan:
	$(MAKE) CC=$(ASAN_CC) BUILD=build-asan SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" test

memcheck: $(TEST_BINS) $(TEST_PROGS)
	@set -e; for t in $(TEST_BINS); do echo "== valgrind $$t"; \
		$(TEST_ENV) valgrind -q --error-exitcode=1 --leak-check=full \
		--errors-for-leak-kinds=definite,indirect $$t; done
	@echo "== memcheck clean"

netcheck:
	@if grep -rnE 'AF_INET|getaddrinfo|gethostbyname|curl|https?:' src/; then \
		echo "netcheck: FAIL (network API in src/)"; exit 1; fi
	@if grep -rnE '\bconnect\(' src/ | grep -v '^src/common/ipc.c:'; then \
		echo "netcheck: FAIL (connect() outside the AF_UNIX helper)"; exit 1; fi
	@echo "netcheck: OK (no network APIs in src/)"

deploy:
	@test -n "$(PI)" || { echo "usage: make deploy PI=pi@<host>"; exit 2; }
	rsync -a --delete --exclude .git --exclude build --exclude build-asan ./ $(PI):pss/
	ssh $(PI) 'cd pss && make'

clean:
	rm -rf build build-asan

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
