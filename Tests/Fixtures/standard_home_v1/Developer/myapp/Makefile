APP_NAME=betterspotlight-myapp
BUILD_DIR=build

.PHONY: all build test lint clean run

all: build

build:
	@echo "Building $(APP_NAME)..."
	@mkdir -p $(BUILD_DIR)
	@echo "compile src/main.cpp"

test:
	@echo "Running unit tests"

lint:
	@echo "Running static checks"

run:
	@echo "Launching local app instance"

clean:
	@rm -rf $(BUILD_DIR)
