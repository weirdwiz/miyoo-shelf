# Full-system QEMU ARMv7 VM standing in for the device (see tools/vm/README.md).
.PHONY: vm-image vm-start vm-stop vm-selftest vm-test-cores vm-test
vm-image:
	sh tools/vm/build-image.sh
vm-start:
	sh tools/vm/start.sh
vm-stop:
	sh tools/vm/stop.sh
vm-selftest:
	sh tools/vm/selftest.sh
vm-test-cores:
	sh tools/vm/test_cores.sh
# Everything that runs in the VM: the harness, the device-built scaler, audio and hardware
# tests, and the real cores through the player (needs `make cores testroms player-cli-mm`).
vm-test: vm-selftest
	sh tools/vm/test_scale.sh
	sh tools/vm/test_audio.sh
	sh tools/vm/test_hw.sh
	sh tools/vm/test_cores.sh
