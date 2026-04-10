# Copyright (c) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

IMAGE_NAME     ?= ocudu-aerial
IMAGE_TAG      ?= latest
AERIAL_TAG     ?= 25.3.2
OCUDU_TAG      ?= release_26_04
CONTAINER_NAME ?= ocudu-aerial-dev

.PHONY: build run shell stop clean logs help

help: ## Show available targets
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | \
		awk 'BEGIN {FS = ":.*?## "}; {printf "  %-15s %s\n", $$1, $$2}'

build: ## Build the Docker image
	docker build \
		--build-arg AERIAL_TAG=$(AERIAL_TAG) \
		--build-arg OCUDU_TAG=$(OCUDU_TAG) \
		--network host \
		-t $(IMAGE_NAME):$(IMAGE_TAG) .

run: ## Run container interactively with GPU support
	docker run -it --rm \
		--gpus all \
		--privileged \
		--network host \
		--name $(CONTAINER_NAME) \
		-v /dev/hugepages:/dev/hugepages \
		$(IMAGE_NAME):$(IMAGE_TAG)

shell: ## Open bash in a running container
	docker exec -it $(CONTAINER_NAME) /bin/bash

stop: ## Stop the running container
	docker stop $(CONTAINER_NAME) 2>/dev/null || true

clean: ## Remove the Docker image
	docker rmi $(IMAGE_NAME):$(IMAGE_TAG) 2>/dev/null || true

logs: ## Show Aerial build logs from the image
	@docker run --rm $(IMAGE_NAME):$(IMAGE_TAG) \
		sh -c 'echo "=== CMAKE LOG ===" && cat /tmp/aerial-cmake.log 2>/dev/null; \
		       echo "=== BUILD LOG ===" && cat /tmp/aerial-build.log 2>/dev/null' || true
