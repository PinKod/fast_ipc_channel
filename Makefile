VERSION_DIR := src/nr_c_plus_plus_v22_3
BUILD_DIR := $(VERSION_DIR)/build_c_plus_plus
DATA_FILE ?= data/big.bin
CORES ?= 0,4
SHM_NAME ?= nr_c_plus_plus_v22_3
RUNNER := src/runner/build/runner

.PHONY: configure build runner run clean

configure:
	mkdir -p $(BUILD_DIR)
	cd $(BUILD_DIR) && cmake ..

build: configure
	cmake --build $(BUILD_DIR) -j

runner:
	./scripts/build_all.sh runner

run: build runner
	$(RUNNER) $(DATA_FILE) $(BUILD_DIR)/producer $(BUILD_DIR)/consumer $(CORES) $(SHM_NAME)

clean:
	rm -rf $(BUILD_DIR)
