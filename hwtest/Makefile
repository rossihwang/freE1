# e1hwtest - cross build for Sogou E1 (RK3308, glibc 2.26)
#
#   source ../cross/env.sh && make -j8
#   make clean
#
# Output: build/e1hwtest (static, no runtime deps)

CC      ?= aarch64-linux-gcc
LVGL    := third_party/lvgl
SRC     := $(wildcard src/*.c)
LVGL_SRC:= $(shell find $(LVGL)/src -name '*.c')
OBJ     := $(patsubst %.c,build/obj/%.o,$(SRC) $(LVGL_SRC))
DEP     := $(OBJ:.o=.d)

CFLAGS  := -O2 -g -std=gnu11 -march=armv8-a \
           -I. -I$(LVGL) -Ithird_party/include -DLV_CONF_INCLUDE_SIMPLE \
           -Wall -Wno-unused-parameter -Wno-sign-compare -MMD -MP
LDFLAGS := -static
LDLIBS  := -lm -lpthread

TARGET  := build/e1hwtest

all: $(TARGET)

deps:
	@test -d $(LVGL) || git clone --depth 1 --branch v9.2.2 --single-branch \
		https://gitee.com/mirrors/LVGL.git $(LVGL)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

build/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build/obj $(TARGET)

install: $(TARGET)
	../scripts/e1-hwtest.sh install

.PHONY: all clean install deps

-include $(DEP)
