.DEFAULT_GOAL := help

SHELL := /bin/bash
CC ?= gcc
CFLAGS ?= -O2
CPPFLAGS ?= -Iinclude
EXTRA_CPPFLAGS ?=

MC_VERSION ?= 1.21.11
SERVER_JAR ?= notchian/server.jar
SERVER_JAR_URL ?= https://piston-data.mojang.com/v1/objects/64bb6d763bed0a9f1d632ec347938594144943ed/server.jar

JAVA_BIN ?= $(if $(wildcard .deps/jdk/Contents/Home/bin/java),$(abspath .deps/jdk/Contents/Home/bin/java),java)
NODE_BIN ?= $(if $(wildcard .deps/node/bin/node),$(abspath .deps/node/bin/node),node)
LINT_CFLAGS ?= -std=gnu11 -fsyntax-only -Wformat -Werror=format-security -Werror=implicit-function-declaration -Werror=implicit-int -Werror=return-type -Werror=int-conversion -Werror=incompatible-pointer-types

.PHONY: help tools-check doctor asdf-check asdf-install lint download-jar registries worldgen-sync-defaults build run all clean clean-cache distclean world-reset world-regen template-refresh

help: ## Show this help message.
	@awk 'BEGIN {FS = ":.*##"; printf "\nTargets:\n"} /^[a-zA-Z0-9_.-]+:.*##/ { printf "  %-14s %s\n", $$1, $$2 }' $(MAKEFILE_LIST)

tools-check: ## Check whether required tools are available.
	@command -v curl >/dev/null || (echo "Missing tool: curl" && exit 1)
	@command -v $(CC) >/dev/null || (echo "Missing C compiler: $(CC)" && exit 1)
	@command -v $(JAVA_BIN) >/dev/null || (echo "Missing Java binary: $(JAVA_BIN)" && exit 1)
	@command -v $(NODE_BIN) >/dev/null || (echo "Missing Node.js binary: $(NODE_BIN)" && exit 1)

asdf-check: ## Verify asdf plugins and pinned tool versions from .tool-versions.
	@./scripts/asdf-bootstrap.sh --check

asdf-install: ## Install missing asdf plugins and pinned tool versions from .tool-versions.
	@./scripts/asdf-bootstrap.sh --install

lint: ## Run compile-time lint checks for critical C issues.
	@$(CC) src/*.c $(CPPFLAGS) $(EXTRA_CPPFLAGS) $(LINT_CFLAGS)
	@echo "Lint passed"

doctor: tools-check lint ## Run project health checks (toolchain + lint).
	@echo "Doctor checks passed"

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

worldgen-sync-defaults: registries ## Derive nethr worldgen defaults from Notchian generated worldgen JSON.
	@./scripts/extract_notchian_worldgen_defaults.py

build: include/registries.h src/registries.c ## Build nethr binary.
	@$(CC) src/*.c $(CPPFLAGS) $(EXTRA_CPPFLAGS) $(CFLAGS) -o nethr
	@echo "Built ./nethr"

run: build ## Run server binary.
	@./nethr

all: registries build ## Download/generate registries and build.

TEMPLATE_HOST ?= 127.0.0.1
TEMPLATE_PORT ?= 25566
TEMPLATE_TARGET ?= 64
TEMPLATE_TIMEOUT ?= 30

template-refresh: ## Refresh assets/chunks/chunk_template_XX.bin from a running Notchian server.
	@echo "Refreshing chunk templates from $(TEMPLATE_HOST):$(TEMPLATE_PORT) (target=$(TEMPLATE_TARGET), timeout=$(TEMPLATE_TIMEOUT)s)"
	@python3 scripts/template_refresh.py \
		--host "$(TEMPLATE_HOST)" \
		--port "$(TEMPLATE_PORT)" \
		--target "$(TEMPLATE_TARGET)" \
		--timeout "$(TEMPLATE_TIMEOUT)"

world-reset: ## Delete persisted world/player state (world.bin) to force a fresh world on next run.
	@rm -f world.bin
	@echo "Removed world.bin. Next server start will regenerate world/player state."

world-regen: ## Reset world and generate a new world.meta seed (optional: SEED=1234 RNG_SEED=5678).
	@rm -f world.bin world.meta
	@seed="$${SEED:-$$(od -An -N4 -tu4 /dev/urandom | tr -d ' ')}"; \
	rng_seed="$${RNG_SEED:-$$(od -An -N4 -tu4 /dev/urandom | tr -d ' ')}"; \
	printf "NETHR_META_V1\nWORLD_SEED=%s\nRNG_SEED=%s\n" "$$seed" "$$rng_seed" > world.meta; \
	echo "Regenerated world state with WORLD_SEED=$$seed RNG_SEED=$$rng_seed"
	@if [ "$${REFRESH_TEMPLATES:-0}" = "1" ]; then \
		$(MAKE) template-refresh TEMPLATE_HOST="$(TEMPLATE_HOST)" TEMPLATE_PORT="$(TEMPLATE_PORT)" TEMPLATE_TARGET="$(TEMPLATE_TARGET)" TEMPLATE_TIMEOUT="$(TEMPLATE_TIMEOUT)"; \
	else \
		echo "Skipping template refresh (set REFRESH_TEMPLATES=1 to enable)"; \
	fi
	@if [ "$${SKIP_TEMPLATE_REFRESH:-0}" = "1" ]; then \
		echo "Note: SKIP_TEMPLATE_REFRESH is deprecated; use REFRESH_TEMPLATES=0/1"; \
	else \
		:; \
	fi

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
