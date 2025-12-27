BUILD_DIR := build
SRC_DIR := server
PROTO_DIR := protocol
RMI_VERSION_HEADER := $(BUILD_DIR)/rmi_version.h
RMI_VERSION_COUNTER := $(BUILD_DIR)/rmi_version.counter
CPPFLAGS := -I$(BUILD_DIR) -I$(PROTO_DIR)
CFLAGS := -Os -fPIE -Wall
LDFLAGS := -pthread -s -pie -w
CC := aarch64-linux-android-gcc
AS := aarch64-linux-android-as
OC := aarch64-linux-android-objcopy

debug: CFLAGS += -DDBG
debug: all

.PHONY: all clean debug rmi exploit payload payload.h force

all: rmi

rmi: $(BUILD_DIR)/rmi

exploit: rmi

payload: $(BUILD_DIR)/payload

payload.h: $(BUILD_DIR)/payload.h

$(BUILD_DIR):
	mkdir -p $@

$(RMI_VERSION_HEADER): force | $(BUILD_DIR)
	@prev=0; \
	if [ -f $(RMI_VERSION_COUNTER) ]; then prev=$$(cat $(RMI_VERSION_COUNTER)); fi; \
	next=$$((prev+1)); \
	echo $$next > $(RMI_VERSION_COUNTER); \
	printf "#ifndef RMI_VERSION_H\n#define RMI_VERSION_H\n#define RMI_VERSION %s\n#endif\n" "$$next" > $@

force:

$(BUILD_DIR)/rmi: $(BUILD_DIR)/main.o $(BUILD_DIR)/exploit.o $(BUILD_DIR)/rmi.o $(BUILD_DIR)/rmi_protocol.o | $(BUILD_DIR)
	$(CC) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/main.o: $(SRC_DIR)/main.c | $(BUILD_DIR)
	$(CC) -o $@ -c $< $(CPPFLAGS) $(CFLAGS)

$(BUILD_DIR)/exploit.o: $(SRC_DIR)/exploit.c $(BUILD_DIR)/payload.h | $(BUILD_DIR)
	$(CC) -o $@ -c $< $(CPPFLAGS) $(CFLAGS)

$(BUILD_DIR)/rmi.o: $(SRC_DIR)/rmi.c $(RMI_VERSION_HEADER) | $(BUILD_DIR)
	$(CC) -o $@ -c $< $(CPPFLAGS) $(CFLAGS)

$(BUILD_DIR)/rmi_protocol.o: $(PROTO_DIR)/rmi_protocol.c | $(BUILD_DIR)
	$(CC) -o $@ -c $< $(CPPFLAGS) $(CFLAGS)

$(BUILD_DIR)/payload.h: $(BUILD_DIR)/payload | $(BUILD_DIR)
	cd $(BUILD_DIR) && xxd -i payload payload.h

$(BUILD_DIR)/payload.o: $(SRC_DIR)/payload.s | $(BUILD_DIR)
	$(AS) -o $@ $^

$(BUILD_DIR)/payload: $(BUILD_DIR)/payload.o | $(BUILD_DIR)
	$(OC) -O binary $^ $@

clean:
	rm -rf $(BUILD_DIR) *.o *.h payload rmi
