# Build the small translated-CPU probe without building the full Metal host.
# The full host is wired to this backend in the next Intel-port milestones;
# keeping this probe separate makes the AArch64→x86_64 boundary testable on an
# Intel Mac before any graphics or service code is involved.

CMAKE ?= cmake
BUILD ?= build

.PHONY: all configure check clean

all: $(BUILD)/dm_guest_cpu_smoke

configure:
	$(CMAKE) -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=26.6

$(BUILD)/dm_guest_cpu_smoke: configure
	$(CMAKE) --build $(BUILD) --target dm_guest_cpu_smoke --parallel

check: all
	$(BUILD)/dm_guest_cpu_smoke

clean:
	rm -rf $(BUILD)
