PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=iceberg
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# We need this for testing
CORE_EXTENSIONS='httpfs;parquet;tpch'

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

start-rest-catalog: install_requirements
	./scripts/start-rest-catalog.sh

install_requirements:
	python3 -m pip install -r scripts/requirements.txt

# Custom makefile targets
data: data_clean start-rest-catalog
	python3 -m scripts.data_generators.generate_data spark-rest local

data_large: data data_clean
	python3 -m scripts.data_generators.generate_data spark-rest local

data_clean:
	rm -rf data/generated

# Install extension and DuckDB binary from GCS
# Downloads the extension matching your current git branch/commit/tag
#
# The script automatically detects:
#   - If on a tag:     downloads from tag/{tag-name}
#   - If on main:      downloads from main/{commit} (optionally specify COMMIT)
#   - If on dev branch: downloads from dev/{branch}/{commit}
#
# Usage:
#   # First, checkout the branch/tag/commit you want to install
#   git checkout main           # or: git checkout v1.0.0, git checkout feature-branch
#   make install-build          # Install to /tmp (will prompt for directory)
#
#   # Advanced usage:
#   make install-build INSTALL_DIR=~/bin     # Install to specific directory
#   make install-build COMMIT=abc123         # Install specific commit from current branch
#   make install-build PLATFORM=linux_amd64  # Force specific platform
#
# Note: The COMMIT parameter only specifies which commit hash to use from the
#       current branch. It does NOT switch branches. To install from a different
#       branch, checkout that branch first with 'git checkout <branch>'.
install-build:
	@echo "Downloading Iceberg extension from GCS..."
	@./scripts/install-duckdb-iceberg.sh $(COMMIT)

# This builds without avro, ducklake, aws useful for cross compiling locally
release_minimal:
	@echo "Building with minimal extensions (excluding avro, ducklake, aws)..."
	@$(MAKE) release EXT_CONFIG=$(PROJ_DIR)extension_config.minimal.cmake


# This cross compiles for linux arm64 using docker
local_release:
	docker build --platform linux/arm64 -t iceberg-arm64-builder -f Dockerfile.arm64-builder .
	@echo "Cross-compiling for ARM64 linux..."
	docker run --rm \
		-v "$$(pwd)":/src \
		-w /src \
		--platform linux/arm64 \
		iceberg-arm64-builder \
		bash -c ' \
			git config --global --add safe.directory /src && \
			git config --global --add safe.directory /src/duckdb && \
			git config --global --add safe.directory /src/extension-ci-tools && \
			git submodule update --init --recursive && \
			make release_minimal \
		'
