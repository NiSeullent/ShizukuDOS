"""Host approval gate tests; no VM or MMIO access (GPL-2.0-only)."""

from __future__ import annotations

import unittest

try:
    from .test_vbox import READY, confirm_vbox_region
except ImportError:  # unittest discovery with guest/ as the top-level directory
    from test_vbox import READY, confirm_vbox_region


READY_TEXT = ("NVME_READY BDF=00000070 ID=4E5680EE CLASS=01080200 "
              "CMD=00100003 BAR=F0408000")
PCI_TEXT = ("00:0e.0 nvme: 80ee-4e56 PIIX3\n"
            "        Class base/sub: 0108 (mass storage controller)\n"
            "        MMIO32 region #0: f0408000..f040ffff\n"
            "        Command: 0003, Status: 0010\n"
            "00:0f.0 other: 1234-5678\n")


class HostApprovalGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.ready = READY.search(READY_TEXT)
        assert self.ready is not None

    def test_exact_virtualbox_resource_allows_only_known_length(self) -> None:
        region = confirm_vbox_region(PCI_TEXT, self.ready)
        self.assertEqual(region["bar_base"], 0xF0408000)
        self.assertEqual(region["bar_bytes_host_reported"], 32768)

    def test_guest_bar_mismatch_denies_mmio(self) -> None:
        changed = READY.search(READY_TEXT.replace("F0408000", "F0409000"))
        assert changed is not None
        with self.assertRaisesRegex(RuntimeError, "disagree"):
            confirm_vbox_region(PCI_TEXT, changed)

    def test_missing_resource_or_wrong_device_denies_mmio(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "length"):
            confirm_vbox_region(PCI_TEXT.replace("MMIO32 region #0", "IO region #0"),
                                self.ready)
        with self.assertRaisesRegex(RuntimeError, "does not match"):
            confirm_vbox_region(PCI_TEXT.replace("80ee-4e56", "1234-5678"),
                                self.ready)

    def test_short_or_non_power_of_two_region_denies_mmio(self) -> None:
        for last in ("f0408007", "f040800c"):
            with self.subTest(last=last):
                with self.assertRaisesRegex(RuntimeError, "disagree"):
                    confirm_vbox_region(PCI_TEXT.replace("f040ffff", last),
                                        self.ready)


if __name__ == "__main__":
    unittest.main()
