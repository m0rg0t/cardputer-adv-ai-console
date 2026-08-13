"""Remove machine-specific paths from compiler-generated firmware strings."""

from pathlib import Path

Import("env")  # type: ignore[name-defined]  # PlatformIO injects this helper.


def prefix_map(source: Path, destination: str) -> list[str]:
    resolved = str(source.resolve())
    return [
        f"-ffile-prefix-map={resolved}={destination}",
        f"-fmacro-prefix-map={resolved}={destination}",
        f"-fdebug-prefix-map={resolved}={destination}",
    ]


project_dir = Path(env.subst("$PROJECT_DIR"))  # type: ignore[name-defined]
flags = prefix_map(Path.home(), "/build")
flags.extend(prefix_map(project_dir, "/src"))
env.Append(CCFLAGS=flags)  # type: ignore[name-defined]
