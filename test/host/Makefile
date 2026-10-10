CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O1
OUT    ?= $(CURDIR)/build

.PHONY: test clean
test: $(OUT)/test_scheduler
	$(OUT)/test_scheduler

$(OUT)/test_scheduler: test_scheduler.c ../../main/scheduler.c ../../main/scheduler.h
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -I../../main -o $@ test_scheduler.c ../../main/scheduler.c

clean:
	rm -rf $(OUT)
