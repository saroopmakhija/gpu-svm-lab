# User-space tests. Built static (glibc) in the Lima VM so they run on the
# Alpine (musl) guest:  scripts/vm.sh make -C tests
CFLAGS := -O2 -g -Wall -Wextra -static -I../include
TESTS := phase2 svm vram stress
all: $(TESTS)
%: %.c common.h ../include/uapi/svmd_drm.h ../include/svmd_regs.h
	$(CC) $(CFLAGS) -o $@ $< -lpthread
clean:
	rm -f $(TESTS)
