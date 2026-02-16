.DEFAULT_GOAL := help

SHELL := /bin/bash

MC_VERSION ?= 1.21.8
SERVER_JAR ?= notchian/server.jar
SERVER_JAR_URL ?= https://piston-data.mojang.com/v1/objects/6bce4ef400e4efaa63a13d5e6f6b500be969ef81/server.jar

JAVA_BIN ?= $(if $(wildcard .deps/jdk/Contents/Home/bin/java),$(abspath .deps/jdk/Contents/Home/bin/java),java)
NODE_BIN ?= $(if $(wildcard .deps/node/bin/node),$(abspath .deps/node/bin/node),node)

.PHONY: help tools-check download-jar registries build run all clean clean-cache distclean

help: ## Show this help message.
	@awk 'BEGIN {FS = ":.*##"; printf "\nTargets:\n"} /^[a-zA-Z0-9_.-]+:.*##/ { printf "  %-14s %s\n", $$1, $$2 }' $(MAKEFILE_LIST)

tools-check: ## Check whether required tools are available.
	@command -v curl >/dev/null || (echo "Missing tool: curl" && exit 1)
	@command -v $(JAVA_BIN) >/dev/null || (echo "Missing Java binary: $(JAVA_BIN)" && exit 1)
	@command -v node >/dev/null || command -v bun >/dev/null || command -v deno >/dev/null || (echo "Missing JS runtime: install node, bun, or deno" && exit 1)

download-jar: ## Download Minecraft server JAR to notchian/server.jar (override SERVER_JAR_URL if needed).
	@mkdir -p $(dir $(SERVER_JAR))
	@if [ -f "$(SERVER_JAR)" ]; then \
		echo "Already exists: $(SERVER_JAR)"; \
	else \
		echo "Downloading Minecraft $(MC_VERSION) server jar..."; \
		curl --http1.1 -L --fail -o "$(SERVER_JAR)" "$(SERVER_JAR_URL)"; \
		echo "Saved: $(SERVER_JAR)"; \
	fi

registries: download-jar ## Generate include/registries.h and src/registries.c.
	@PATH="$(abspath .deps/jdk/Contents/Home/bin):$(abspath .deps/node/bin):$$PATH" SERVER_JAR="$(notdir $(SERVER_JAR))" ./extract_registries.sh

build: include/registries.h src/registries.c ## Build nethr binary.
	@gcc src/*.c -O2 -Iinclude -o nethr
	@echo "Built ./nethr"

run: build ## Run server binary.
	@./nethr

all: registries build ## Download/generate registries and build.

clean: ## Remove local build and generated artifacts (keeps notchian/server.jar).
	@rm -f nethr nethr.exe world.bin include/registries.h src/registries.c
	@echo "Cleaned build outputs and generated registries"

clean-cache: ## Remove generated Notchian cache but keep notchian/server.jar.
	@rm -rf notchian/generated notchian/libraries notchian/logs notchian/versions
	@echo "Cleaned notchian generated cache"

distclean: clean clean-cache ## Remove all local artifacts, including notchian and .deps.
	@rm -rf notchian .deps
	@echo "Removed notchian and .deps"

include/registries.h src/registries.c:
	@$(MAKE) registries
