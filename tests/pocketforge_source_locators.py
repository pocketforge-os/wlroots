#!/usr/bin/env python3

import configparser
import re
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EXPECTED = {
	"libdisplay-info.wrap": (
		"https://github.com/pocketforge-os/libdisplay-info.git",
		"0791e6f6cfa928f85b952806811f3df998d2e898",
	),
	"libdrm.wrap": (
		"https://github.com/pocketforge-os/drm.git",
		"b97cbde15c5c3abfe44d78e8f57139e50f612fec",
	),
	"libliftoff.wrap": (
		"https://github.com/pocketforge-os/libliftoff.git",
		"c4226a79b7a52f59bb51789b1eda112658609125",
	),
	"libxkbcommon.wrap": (
		"https://github.com/pocketforge-os/libxkbcommon.git",
		"66e702d83718bb845148322d5561ef1a9ef843c4",
	),
	"pixman.wrap": (
		"https://github.com/pocketforge-os/pixman.git",
		"f227dcbe57e8fcfb5174ffd7eb55a69b6692d512",
	),
	"seatd.wrap": (
		"https://github.com/pocketforge-os/seatd.git",
		"427b5d956afb589c4c8a1612c75644a9254d2051",
	),
	"wayland-protocols.wrap": (
		"https://github.com/pocketforge-os/wayland-protocols.git",
		"aa62366fb800a0689f4e9de83811ff33f6a91f44",
	),
	"wayland.wrap": (
		"https://github.com/pocketforge-os/wayland.git",
		"1bd29e0709db70b6463c947f150ea20dcfce9cac",
	),
}


def validate_wrap(path, expected_url, expected_revision):
	parser = configparser.ConfigParser(interpolation=None)
	with path.open(encoding="utf-8") as stream:
		parser.read_file(stream)
	if parser.sections() != ["wrap-git"]:
		raise ValueError(f"{path.name}: expected one wrap-git section")
	url = parser.get("wrap-git", "url", fallback="")
	revision = parser.get("wrap-git", "revision", fallback="")
	if url != expected_url:
		raise ValueError(f"{path.name}: unexpected URL {url!r}")
	if revision != expected_revision:
		raise ValueError(f"{path.name}: unexpected revision {revision!r}")
	if re.fullmatch(r"[0-9a-f]{40}", revision) is None:
		raise ValueError(f"{path.name}: revision is not an immutable commit")


class SourceLocatorTests(unittest.TestCase):
	def test_repository_wraps_are_complete_and_pinned(self):
		wrap_dir = ROOT / "subprojects"
		actual = {path.name for path in wrap_dir.glob("*.wrap")}
		self.assertEqual(actual, set(EXPECTED))
		for name, (url, revision) in EXPECTED.items():
			with self.subTest(name=name):
				validate_wrap(wrap_dir / name, url, revision)

	def test_exact_locator_is_accepted(self):
		with tempfile.TemporaryDirectory() as temp_dir:
			path = Path(temp_dir) / "dependency.wrap"
			path.write_text(
				"[wrap-git]\n"
				"url = https://github.com/pocketforge-os/example.git\n"
				"revision = 0123456789abcdef0123456789abcdef01234567\n",
				encoding="utf-8",
			)
			validate_wrap(
				path,
				"https://github.com/pocketforge-os/example.git",
				"0123456789abcdef0123456789abcdef01234567",
			)

	def test_mutable_revision_is_rejected(self):
		with tempfile.TemporaryDirectory() as temp_dir:
			path = Path(temp_dir) / "dependency.wrap"
			path.write_text(
				"[wrap-git]\n"
				"url = https://github.com/pocketforge-os/example.git\n"
				"revision = HEAD\n",
				encoding="utf-8",
			)
			with self.assertRaises(ValueError):
				validate_wrap(
					path,
					"https://github.com/pocketforge-os/example.git",
					"0123456789abcdef0123456789abcdef01234567",
				)

	def test_upstream_url_is_rejected(self):
		with tempfile.TemporaryDirectory() as temp_dir:
			path = Path(temp_dir) / "dependency.wrap"
			path.write_text(
				"[wrap-git]\n"
				"url = https://example.com/upstream.git\n"
				"revision = 0123456789abcdef0123456789abcdef01234567\n",
				encoding="utf-8",
			)
			with self.assertRaises(ValueError):
				validate_wrap(
					path,
					"https://github.com/pocketforge-os/example.git",
					"0123456789abcdef0123456789abcdef01234567",
				)


if __name__ == "__main__":
	unittest.main(verbosity=2)
