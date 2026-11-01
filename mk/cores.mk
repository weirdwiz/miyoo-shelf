# libretro cores and test ROMs, built and fetched by tools/cores/*.sh.
#   make cores      cross-compile mGBA and gambatte into build/cores (podman or docker)
#   make testroms   fetch pinned test ROMs into build/testroms

CORES_DIR := $(BUILD)/cores
CORES := $(CORES_DIR)/mgba_libretro.so $(CORES_DIR)/gambatte_libretro.so
CORES_DEPS := tools/cores/build.sh tools/cores/build-in-container.sh

.PHONY: cores testroms

# One container build produces both cores, so the freshness check runs here
# instead of as per-file rules (which would start the build twice).
cores:
	@stale=; for so in $(CORES); do \
		for dep in $(CORES_DEPS); do \
			if [ ! -f "$$so" ] || [ "$$dep" -nt "$$so" ]; then stale=1; fi; \
		done; \
	done; \
	if [ -z "$$stale" ]; then \
		echo "cores up to date: $(CORES)"; \
	else \
		CONTAINER="$(CONTAINER)" TOOLCHAIN="$(TOOLCHAIN)" tools/cores/build.sh; \
	fi

testroms:
	tools/cores/fetch-testroms.sh
