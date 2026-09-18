CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O1 -g
SANITIZERS ?= -fsanitize=address,undefined -fno-omit-frame-pointer
BUILD_DIR ?= .build

.PHONY: test clean

test: $(BUILD_DIR)/video-double $(BUILD_DIR)/video-single $(BUILD_DIR)/shutdown $(BUILD_DIR)/pacing
	$(BUILD_DIR)/video-double
	$(BUILD_DIR)/video-single
	$(BUILD_DIR)/shutdown
	$(BUILD_DIR)/pacing

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/capture.inc: ../src/main.c extract_capture.py | $(BUILD_DIR)
	python3 extract_capture.py ../src/main.c $@

$(BUILD_DIR)/shutdown.inc: ../src/main.c extract_capture.py | $(BUILD_DIR)
	python3 extract_capture.py --shutdown ../src/main.c $@

$(BUILD_DIR)/shutdown: test_shutdown.c $(BUILD_DIR)/shutdown.inc | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SANITIZERS) -Wno-unused-parameter -Istubs -I$(BUILD_DIR) test_shutdown.c -o $@

$(BUILD_DIR)/pacing.inc: ../src/main.c ../include/usb_descriptors.h extract_capture.py | $(BUILD_DIR)
	python3 extract_capture.py --pacing ../src/main.c $@

$(BUILD_DIR)/controls.inc: ../src/main.c extract_capture.py | $(BUILD_DIR)
	python3 extract_capture.py --controls ../src/main.c $@

$(BUILD_DIR)/pacing: test_pacing.c ../include/uvc.h $(BUILD_DIR)/pacing.inc $(BUILD_DIR)/controls.inc | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SANITIZERS) -Wno-unused-parameter -I../include -I$(BUILD_DIR) test_pacing.c -o $@

$(BUILD_DIR)/video-double: test_video.c ../src/video.c ../include/video.h ../include/uvc.h $(BUILD_DIR)/capture.inc $(wildcard stubs/*.h stubs/psp2kern/*.h stubs/psp2kern/kernel/*.h) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SANITIZERS) -Istubs -I../include -I$(BUILD_DIR) -DUVC_FRAMEBUFFER_COUNT=2 test_video.c ../src/video.c -o $@

$(BUILD_DIR)/video-single: test_video.c ../src/video.c ../include/video.h ../include/uvc.h $(BUILD_DIR)/capture.inc $(wildcard stubs/*.h stubs/psp2kern/*.h stubs/psp2kern/kernel/*.h) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SANITIZERS) -Istubs -I../include -I$(BUILD_DIR) -DUVC_FRAMEBUFFER_COUNT=1 test_video.c ../src/video.c -o $@

clean:
	rm -rf $(BUILD_DIR)
