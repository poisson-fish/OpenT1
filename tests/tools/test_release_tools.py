"""Release boundary checks: reject bad inputs and preserve documentation links."""
from pathlib import Path
import json
import shutil
import sys
import tempfile
import unittest
import zipfile
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import package_release
import publish_release
import sync_wiki


class ReleaseInputs(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.stage = Path(self.temporary.name)
        for name in ("opent1.exe", "OpenAL32.dll", "README.md", "LICENSE", "THIRD_PARTY.md", "release.json"):
            (self.stage / name).write_text("fixture")

    def test_game_and_development_inputs_are_rejected(self):
        package_release.validate_stage(self.stage, "windows-x64")
        for name in ("base/mission.cs", "console.cs", "console.log", "tests/data.bin", "opent1.pdb", ".git/config"):
            with self.subTest(name=name):
                file = self.stage / name
                file.parent.mkdir(exist_ok=True, parents=True)
                file.write_text("must not ship")
                with self.assertRaises(ValueError):
                    package_release.validate_stage(self.stage, "windows-x64")
                file.unlink()

    def test_missing_executable_and_openal_are_rejected(self):
        (self.stage / "opent1.exe").unlink()
        with self.assertRaisesRegex(ValueError, "opent1.exe"):
            package_release.validate_stage(self.stage, "windows-x64")
        (self.stage / "opent1.exe").touch()
        (self.stage / "OpenAL32.dll").unlink()
        with self.assertRaisesRegex(ValueError, "OpenAL"):
            package_release.validate_stage(self.stage, "windows-x64")

    def test_only_complete_platform_sets_can_be_published(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            with self.assertRaises(ValueError):
                publish_release.validate_artifacts(directory)
            for name in ("OpenT1-windows-x64.zip", "OpenT1-linux-x64.tar.gz", "OpenT1-macos-arm64.tar.gz", "OpenT1-source.tar.gz"):
                (directory / name).touch()
            self.assertEqual(len(publish_release.validate_artifacts(directory)), 4)
            (directory / "unexpected.txt").touch()
            with self.assertRaises(ValueError):
                publish_release.validate_artifacts(directory)

    def test_numbered_tags_must_match_version_and_channel(self):
        self.assertFalse(publish_release.tag_version("v0.1.0", "0.1.0"))
        self.assertTrue(publish_release.tag_version("v0.1.0-rc.1", "0.1.0"))
        for tag in ("v0.2.0", "0.1.0", "v0.1.0-rc.0", "v0.1.0-unknown", "v0.1.0;echo bad"):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                publish_release.tag_version(tag, "0.1.0")

    def test_artifacts_from_another_commit_cannot_be_published(self):
        archive = self.stage / "OpenT1-windows-x64.zip"
        metadata = {"commit": "a" * 40, "version": "0.1.0", "platform": "windows-x64", "configuration": "Release"}
        with zipfile.ZipFile(archive, "w") as package:
            package.writestr("OpenT1-windows-x64/release.json", json.dumps(metadata))
        publish_release.validate_identity([archive], "a" * 40, "0.1.0")
        with self.assertRaisesRegex(ValueError, "identity"):
            publish_release.validate_identity([archive], "b" * 40, "0.1.0")


class WikiSources(unittest.TestCase):
    def test_repository_links_and_generated_index_are_current(self):
        sync_wiki.check_pages()

    def test_export_rewrites_page_links_and_preserves_other_pages(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            other = directory / "Community.md"
            other.write_text("User-owned page")
            sync_wiki.export(directory)
            self.assertEqual(other.read_text(), "User-owned page")
            home = (directory / "Home.md").read_text()
            self.assertIn("](Getting-started)", home)
            self.assertNotIn("](Getting-started.md)", home)
            self.assertIn("https://github.com/poisson-fish/OpenT1/tree/main/docs/wiki", home)

    def test_broken_page_links_fail_validation(self):
        with tempfile.TemporaryDirectory() as temporary:
            pages = Path(temporary)
            shutil.copytree(sync_wiki.PAGES, pages, dirs_exist_ok=True)
            with (pages / "Home.md").open("a") as stream:
                stream.write("\n[Missing page](Does-not-exist.md)\n")
            with patch.object(sync_wiki, "PAGES", pages), self.assertRaisesRegex(ValueError, "broken wiki link"):
                sync_wiki.check_pages()


if __name__ == "__main__":
    unittest.main()
