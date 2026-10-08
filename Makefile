CROSS ?=
CC := $(CROSS)gcc
TARGET := $(shell $(CC) -dumpmachine)
BUILD_DIR ?= .build/$(TARGET)
CFLAGS ?= -g -Wall -O2
CPPFLAGS += -Iinclude -Isrc -D_FILE_OFFSET_BITS=64

ifneq (,$(findstring mingw,$(TARGET)))
NAME ?= openrockcli.exe
CPPFLAGS += -D_WIN32_WINNT=0x0601
APP_LDFLAGS = -municode
WINDOWS_TEST = $(BUILD_DIR)/windows_transport_test.exe
LDLIBS += -lsetupapi -lcfgmgr32
else
NAME ?= openrockcli
CPPFLAGS += $(shell pkg-config --cflags libusb-1.0)
LDLIBS += $(shell pkg-config --libs libusb-1.0)
endif

SOURCES := $(wildcard src/*.c)
OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SOURCES))
TEST_BINS := $(BUILD_DIR)/rockchip_usb_error_test.exe $(BUILD_DIR)/usb_transport_test.exe $(BUILD_DIR)/rock_protocol_test.exe $(WINDOWS_TEST) $(BUILD_DIR)/flash_audit_test.exe

.PHONY: all test test-build install clean
all: $(NAME)
$(NAME): $(OBJECTS)
	$(CC) $(LDFLAGS) $(APP_LDFLAGS) $^ -o $@ $(LDLIBS)
$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD_DIR)/rockchip_usb_error_test.exe: tests/rockchip_usb_error_test.c src/rockchip_usb_error.h include/usb_transport.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@
$(BUILD_DIR)/usb_transport_test.exe: tests/usb_transport_test.c src/usb_transport.c include/usb_transport.h include/rockusb_windows_abi.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/usb_transport_test.c src/usb_transport.c -o $@
$(BUILD_DIR)/rock_protocol_test.exe: tests/rock_protocol_test.c $(filter-out $(BUILD_DIR)/main.o $(BUILD_DIR)/usb_libusb.o $(BUILD_DIR)/usb_windows.o,$(OBJECTS))
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@
$(BUILD_DIR)/flash_audit_test.exe: tests/flash_audit_test.c $(filter-out $(BUILD_DIR)/main.o $(BUILD_DIR)/usb_libusb.o $(BUILD_DIR)/usb_windows.o,$(OBJECTS))
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@
$(BUILD_DIR)/windows_transport_test.exe: tests/windows_transport_test.c src/usb_windows.c src/usb_transport.c src/file_portable.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/windows_transport_test.c src/usb_transport.c src/file_portable.c -o $@ $(LDLIBS)
test-build: $(NAME) $(TEST_BINS)
test: test-build
	$(BUILD_DIR)/rockchip_usb_error_test.exe
	$(BUILD_DIR)/usb_transport_test.exe
	$(BUILD_DIR)/rock_protocol_test.exe $(BUILD_DIR)/protocol-fixture.img
	$(BUILD_DIR)/flash_audit_test.exe $(BUILD_DIR)/audit-fixture.img
	$(if $(WINDOWS_TEST),$(WINDOWS_TEST),true)
	python3 tests/cli_image_test.py ./$(NAME)
install: $(NAME)
	install -Dm0755 $(NAME) /usr/local/bin/openrockcli
	install -Dm0644 99-openrockcli.rules /etc/udev/rules.d/99-openrockcli.rules
	install -Dm0644 LICENSE /usr/share/licenses/openrockcli/LICENSE
clean:
	rm -rf $(BUILD_DIR)
	rm -f $(NAME)
-include $(OBJECTS:.o=.d)
