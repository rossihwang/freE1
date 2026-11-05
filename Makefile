# freE1 顶层构建入口
PC   := make -f Makefile.pc
E1   := make -f Makefile.e1ui

all: pc
pc:
	$(PC) -j8
e1:
	$(E1) -j8
shots: pc
	./build_pc/freE1 shots --dir demo --out shots
test: pc
	./build_pc/freE1 swipetest
run: pc
	./run_pc.sh
clean:
	$(PC) clean; $(E1) clean
.PHONY: all pc e1 shots test run clean

# 硬件测试：独立程序（复用同一套 HAL 与 LVGL），与主 UI 分开构建
hwtest:
	$(E1) -j8
	$(MAKE) -C hwtest -j8
