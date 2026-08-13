from __future__ import annotations

import importlib.util
from pathlib import Path

SCRIPT = Path(__file__).parents[1] / "scripts/configure.py"
SPEC = importlib.util.spec_from_file_location("gateway_configure", SCRIPT)
assert SPEC and SPEC.loader
configure = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(configure)


def test_render_env_preserves_comments_and_replaces_values() -> None:
    template = ["# comment\n", "VALUE=old\n", "OTHER=same\n"]
    rendered = configure.render_env(template, {"VALUE": "new", "ADDED": "yes"})
    assert rendered == "# comment\nVALUE=new\nOTHER=same\nADDED=yes\n"


def test_render_env_quotes_paths_with_spaces() -> None:
    rendered = configure.render_env(
        ["OBSIDIAN_VAULT_ROOT=old\n"],
        {"OBSIDIAN_VAULT_ROOT": "/Users/example/My Vault"},
    )
    assert rendered == "OBSIDIAN_VAULT_ROOT='/Users/example/My Vault'\n"


def test_validate_vault_accepts_writable_directory(tmp_path: Path) -> None:
    vault = tmp_path / "My Vault"
    vault.mkdir()
    assert configure.validate_vault(vault) == vault.resolve()


def test_existing_noninteractive_config_can_be_updated_without_vault_arg(
    tmp_path: Path, monkeypatch
) -> None:
    vault = tmp_path / "Vault"
    vault.mkdir()
    env_path = tmp_path / ".env"
    env_path.write_text(
        f"VOICE_DEVICE_TOKEN={'x' * 24}\n"
        f"OBSIDIAN_VAULT_ROOT={vault}\n"
        "OBSIDIAN_NOTES_FOLDER=Existing\n",
        encoding="utf-8",
    )
    monkeypatch.setattr(
        "sys.argv",
        [
            "configure.py",
            "--non-interactive",
            "--env-file",
            str(env_path),
            "--tts-provider",
            "elevenlabs",
            "--tts-name",
            "Jarvis",
        ],
    )

    assert configure.main() == 0
    values = configure.parse_env(env_path.read_text().splitlines())
    assert values["OBSIDIAN_NOTES_FOLDER"] == "Existing"
    assert values["TTS_PROVIDER"] == "elevenlabs"
    assert values["TTS_NAME"] == "Jarvis"
